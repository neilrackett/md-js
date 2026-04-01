/**
 * File: js_worker.c
 * Description: MD-JS JavaScript Worker — Core 1 JerryScript runtime.
 *
 * Core 0 calls js_worker_init() once and js_worker_loop() from its main loop.
 * Core 1 runs core1_entry(), waiting on the multicore FIFO for work tags and
 * executing JerryScript operations (eval / call / reset / ping).
 *
 * Signalling:  32-bit FIFO words, tag in upper 8 bits (FIFO_MSG_*).
 * Data:        JsWorkerMsgBlock in BSS, protected by spin-lock JS_SPINLOCK_ID.
 * Result path: Core 1 writes result_json → ROM-in-RAM @ JS_RESULT_OFFSET
 *              (with 16-bit byte-swap so the ST can read it big-endian).
 *              Core 0 then writes the random token to unblock the ST.
 */

#include "js_worker.h"

#include <alloca.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "constants.h"
#include "debug.h"
#include "memfunc.h"
#include "term.h"
#include "tprotocol.h"

#include "jerryscript.h"

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"

/* ── Linker symbol for ROM-in-RAM base (defined in memmap_rp.ld) ────────── */
extern unsigned int __rom_in_ram_start__;

/* ── Shared state ────────────────────────────────────────────────────────── */
static JsWorkerMsgBlock s_msg;
static spin_lock_t     *s_spin_lock;

/* Cached addresses (set in js_worker_init, read-only thereafter) */
static uint32_t s_rom_base;
static uint32_t s_token_addr;
static uint32_t s_token_seed_addr;
static volatile char     *s_result_mem;
static volatile uint16_t *s_status_mem;   /* async status word at JS_STATUS_OFFSET */

/* Async call state — Core 0 only, no lock needed */
static bool     s_async_pending;
static uint64_t s_async_start_us;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void core1_entry(void);
static void core1_handle_ping(void);
static void core1_handle_upload(void);
static void core1_handle_call(void);
static void core1_handle_reset(void);
static void core1_flush_result(void);
static void js_dispatch_command(const TransmissionProtocol *proto);
static void js_send_response(uint32_t random_token);
static void js_write_busy_error(void);
static void js_write_timeout_error(void);
static void js_parse_call_payload(const uint16_t *payload_ptr);

/* ────────────────────────────────────────────────────────────────────────── */
/* Core 1 — JerryScript runtime                                              */
/* ────────────────────────────────────────────────────────────────────────── */

static void core1_entry(void) {
  DPRINTF("Core 1: MD-JS worker starting\n");

  /* Always cleanup before init — handles both first-start and post-timeout
   * restart (jerry_cleanup on an uninitialised context is a no-op). */
  jerry_cleanup();
  jerry_init(JERRY_INIT_EMPTY);

  DPRINTF("Core 1: JerryScript initialized (heap: %u KB)\n",
          JERRY_GLOBAL_HEAP_SIZE);

  while (true) {
    uint32_t msg = multicore_fifo_pop_blocking();
    uint8_t  tag = (uint8_t)((msg & FIFO_TAG_MASK) >> FIFO_TAG_SHIFT);

    switch (tag) {
      case FIFO_MSG_PING:   core1_handle_ping();   break;
      case FIFO_MSG_UPLOAD: core1_handle_upload(); break;
      case FIFO_MSG_CALL:   core1_handle_call();   break;
      case FIFO_MSG_RESET:  core1_handle_reset();  break;
      default:
        DPRINTF("Core 1: unknown FIFO tag 0x%02X\n", tag);
        multicore_fifo_push_blocking((uint32_t)FIFO_MSG_ERROR << FIFO_TAG_SHIFT);
        break;
    }
  }
}

static void core1_handle_ping(void) {
  uint32_t save = spin_lock_blocking(s_spin_lock);
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE,
           "{\"version\":\"MD-JS/1.0\",\"jerry\":\"%d.%d.%d\"}",
           JERRY_API_MAJOR_VERSION,
           JERRY_API_MINOR_VERSION,
           JERRY_API_PATCH_VERSION);
  s_msg.result_is_error = false;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking((uint32_t)FIFO_MSG_DONE << FIFO_TAG_SHIFT);
}

static void core1_handle_upload(void) {
  /* Copy source under lock, then eval without holding it. */
  uint32_t save = spin_lock_blocking(s_spin_lock);
  uint32_t src_len = s_msg.js_source_len;
  spin_unlock(s_spin_lock, save);

  jerry_value_t result = jerry_eval(
      (const jerry_char_t *)s_msg.js_source, src_len, JERRY_PARSE_NO_OPTS);

  bool is_err = jerry_value_is_exception(result);
  save = spin_lock_blocking(s_spin_lock);
  if (is_err) {
    jerry_value_t err_val = jerry_exception_value(result, false);
    jerry_value_t err_str = jerry_value_to_string(err_val);
    jerry_value_free(err_val);
    jerry_size_t sz = jerry_string_to_buffer(
        err_str, JERRY_ENCODING_UTF8,
        (jerry_char_t *)s_msg.result_json, JS_RESULT_MAX_SIZE - 1);
    s_msg.result_json[sz] = '\0';
    jerry_value_free(err_str);
  } else {
    snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE, "{\"ok\":true}");
  }
  jerry_value_free(result);
  s_msg.result_is_error = is_err;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking(
      (uint32_t)(is_err ? FIFO_MSG_ERROR : FIFO_MSG_DONE) << FIFO_TAG_SHIFT);
}

/* Maximum argv entries — caps alloca size to ~256 bytes on Core 1's 2 KB stack. */
#define JS_CALL_MAX_ARGS 32

static void core1_handle_call(void) {
  /* Copy call parameters under lock, then do all JS work without holding it. */
  char call_func[JS_CALL_FUNC_NAME_MAX];
  char call_args_json[JS_CALL_ARGS_MAX];
  uint32_t save = spin_lock_blocking(s_spin_lock);
  memcpy(call_func, s_msg.call_func, JS_CALL_FUNC_NAME_MAX);
  memcpy(call_args_json, s_msg.call_args_json, JS_CALL_ARGS_MAX);
  spin_unlock(s_spin_lock, save);

  char result_json[JS_RESULT_MAX_SIZE];
  bool result_is_error;

  jerry_value_t global    = jerry_current_realm();
  jerry_value_t fname_str = jerry_string_sz(call_func);
  jerry_value_t func_val  = jerry_object_get(global, fname_str);
  jerry_value_free(fname_str);
  jerry_value_free(global);

  if (!jerry_value_is_function(func_val)) {
    snprintf(result_json, JS_RESULT_MAX_SIZE,
             "{\"error\":\"function '%s' not found\"}", call_func);
    jerry_value_free(func_val);
    result_is_error = true;
    goto write_result;
  }

  jerry_value_t args_val = jerry_json_parse(
      (const jerry_char_t *)call_args_json,
      strlen(call_args_json));

  if (jerry_value_is_exception(args_val)) {
    snprintf(result_json, JS_RESULT_MAX_SIZE,
             "{\"error\":\"invalid args JSON\"}");
    jerry_value_free(args_val);
    jerry_value_free(func_val);
    result_is_error = true;
    goto write_result;
  }

  jerry_value_t *argv = NULL;
  jerry_length_t  argc = 0;

  if (jerry_value_is_array(args_val)) {
    argc = jerry_array_length(args_val);
    if (argc > JS_CALL_MAX_ARGS) argc = JS_CALL_MAX_ARGS;
    if (argc > 0) {
      argv = (jerry_value_t *)alloca(sizeof(jerry_value_t) * argc);
      for (jerry_length_t i = 0; i < argc; i++) {
        argv[i] = jerry_object_get_index(args_val, i);
      }
    }
  }

  jerry_value_t undefined_val = jerry_undefined();
  jerry_value_t ret = jerry_call(func_val, undefined_val, argv, argc);
  jerry_value_free(undefined_val);

  for (jerry_length_t i = 0; i < argc; i++) {
    jerry_value_free(argv[i]);
  }
  jerry_value_free(args_val);
  jerry_value_free(func_val);

  if (jerry_value_is_exception(ret)) {
    jerry_value_t err_val = jerry_exception_value(ret, false);
    jerry_value_t err_str = jerry_value_to_string(err_val);
    jerry_value_free(err_val);
    jerry_size_t sz = jerry_string_to_buffer(
        err_str, JERRY_ENCODING_UTF8,
        (jerry_char_t *)result_json, JS_RESULT_MAX_SIZE - 1);
    result_json[sz] = '\0';
    jerry_value_free(err_str);
    result_is_error = true;
  } else {
    jerry_value_t json_str = jerry_json_stringify(ret);
    jerry_value_free(ret);
    if (jerry_value_is_exception(json_str)) {
      snprintf(result_json, JS_RESULT_MAX_SIZE,
               "{\"error\":\"result not JSON-serialisable\"}");
      jerry_value_free(json_str);
      result_is_error = true;
    } else {
      jerry_size_t sz = jerry_string_to_buffer(
          json_str, JERRY_ENCODING_UTF8,
          (jerry_char_t *)result_json, JS_RESULT_MAX_SIZE - 1);
      result_json[sz] = '\0';
      jerry_value_free(json_str);
      result_is_error = false;
    }
  }

write_result:
  save = spin_lock_blocking(s_spin_lock);
  memcpy(s_msg.result_json, result_json, JS_RESULT_MAX_SIZE);
  s_msg.result_is_error = result_is_error;
  spin_unlock(s_spin_lock, save);

done:
  core1_flush_result();
  multicore_fifo_push_blocking(
      (uint32_t)(s_msg.result_is_error ? FIFO_MSG_ERROR : FIFO_MSG_DONE)
      << FIFO_TAG_SHIFT);
}

static void core1_handle_reset(void) {
  jerry_cleanup();
  jerry_init(JERRY_INIT_EMPTY);

  uint32_t save = spin_lock_blocking(s_spin_lock);
  s_msg.js_source_len    = 0;
  s_msg.chunks_expected  = 0;
  s_msg.chunks_received  = 0;
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE,
           "{\"ok\":true,\"reset\":true}");
  s_msg.result_is_error = false;
  spin_unlock(s_spin_lock, save);

  core1_flush_result();
  multicore_fifo_push_blocking((uint32_t)FIFO_MSG_DONE << FIFO_TAG_SHIFT);
}

/**
 * Copy result_json to the ROM-in-RAM result area with 16-bit byte-swap so
 * the Atari ST sees the bytes in big-endian order when reading through the
 * cartridge ROM address space.
 */
static void core1_flush_result(void) {
  uint32_t save = spin_lock_blocking(s_spin_lock);
  size_t len = strnlen(s_msg.result_json, JS_RESULT_MAX_SIZE - 1) + 1;
  /* Round up to even byte count required by the 16-bit swap macro */
  size_t copy_len = (len + 1u) & ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(s_msg.result_json,
                                    (void *)s_result_mem,
                                    copy_len);
  /* Update async status so the ST can see DONE/ERROR before the FIFO push. */
  *s_status_mem = (uint16_t)(s_msg.result_is_error
                              ? MDJS_STATUS_ERROR
                              : MDJS_STATUS_DONE);
  spin_unlock(s_spin_lock, save);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Core 0 — command dispatcher                                                */
/* ────────────────────────────────────────────────────────────────────────── */

/**
 * Write the random token back to shared memory to unblock the ST, and
 * seed the next token value.
 */
static void js_send_response(uint32_t random_token) {
  TPROTO_SET_RANDOM_TOKEN(s_token_addr, random_token);
  uint32_t new_seed = rand();
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, new_seed);
}

/**
 * Wait for Core 1 to finish, with timeout recovery.
 * Returns the FIFO message tag, or FIFO_MSG_ERROR on timeout.
 */
static uint8_t js_wait_for_core1(void) {
  uint32_t resp = 0;
  bool     ok   = multicore_fifo_pop_timeout_us(JS_CALL_TIMEOUT_US, &resp);
  if (!ok) {
    DPRINTF("js_worker: Core 1 timeout — resetting\n");
    uint32_t save = spin_lock_blocking(s_spin_lock);
    js_write_timeout_error();
    spin_unlock(s_spin_lock, save);

    /* Reset Core 1. jerry_cleanup() is called at the top of core1_entry()
     * before re-init, so the old heap is always freed on restart. */
    multicore_reset_core1();
    multicore_launch_core1(core1_entry);
    return FIFO_MSG_ERROR;
  }
  return (uint8_t)((resp & FIFO_TAG_MASK) >> FIFO_TAG_SHIFT);
}

/* Write an error JSON literal to the result buffer and update the status word.
 * Must be called while holding s_spin_lock. */
static void js_write_timeout_error(void) {
  snprintf(s_msg.result_json, JS_RESULT_MAX_SIZE, "{\"error\":\"timeout\"}");
  s_msg.result_is_error = true;
  size_t len      = strnlen(s_msg.result_json, JS_RESULT_MAX_SIZE - 1) + 1;
  size_t copy_len = (len + 1u) & ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16(s_msg.result_json, (void *)s_result_mem, copy_len);
  *s_status_mem = (uint16_t)MDJS_STATUS_ERROR;
}

/* Write the "busy" error JSON to the result buffer (sizeof includes NUL). */
static void __not_in_flash_func(js_write_busy_error)(void) {
  static const char busy[] = "{\"error\":\"busy\"}";
  size_t blen = (sizeof(busy) + 1u) & ~1u;
  COPY_AND_CHANGE_ENDIANESS_BLOCK16((void *)busy, (void *)s_result_mem, blen);
}

/* Parse a CALL payload (func_name\0args_json\0) into s_msg under spin-lock. */
static void js_parse_call_payload(const uint16_t *payload_ptr) {
  const char *func_name = (const char *)payload_ptr;
  size_t      fn_len    = strnlen(func_name, JS_CALL_FUNC_NAME_MAX);
  const char *args_json = func_name + fn_len + 1;

  uint32_t save = spin_lock_blocking(s_spin_lock);
  strncpy(s_msg.call_func, func_name, JS_CALL_FUNC_NAME_MAX - 1);
  s_msg.call_func[JS_CALL_FUNC_NAME_MAX - 1] = '\0';
  strncpy(s_msg.call_args_json, args_json, JS_CALL_ARGS_MAX - 1);
  s_msg.call_args_json[JS_CALL_ARGS_MAX - 1] = '\0';
  spin_unlock(s_spin_lock, save);
}

/**
 * Drain the FIFO response from an in-progress async call, if any.
 * Called at the top of js_worker_loop() — never blocks.
 */
static void __not_in_flash_func(js_drain_async_fifo)(void) {
  if (!s_async_pending) return;

  /* Async timeout: write error result and reset Core 1 */
  if (time_us_64() - s_async_start_us > (uint64_t)JS_CALL_TIMEOUT_US) {
    DPRINTF("js_worker: async timeout — resetting Core 1\n");
    uint32_t save = spin_lock_blocking(s_spin_lock);
    js_write_timeout_error();
    spin_unlock(s_spin_lock, save);
    s_async_pending = false;
    multicore_reset_core1();
    multicore_launch_core1(core1_entry);
    return;
  }

  /* Non-blocking: only drain if Core 1 has already pushed a response */
  if (!multicore_fifo_rvalid()) return;
  multicore_fifo_pop_blocking(); /* discard tag — status already written by Core 1 */
  s_async_pending = false;
}

static void js_dispatch_command(const TransmissionProtocol *proto) {
  uint32_t  random_token = TPROTO_GET_RANDOM_TOKEN(proto->payload);
  uint16_t *payload_ptr  = (uint16_t *)proto->payload;

  switch (proto->command_id) {

    /* ── CMD_JS_PING ──────────────────────────────────────────────────── */
    case CMD_JS_PING: {
      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_PING << FIFO_TAG_SHIFT);
      js_wait_for_core1();
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_UPLOAD ────────────────────────────────────────────────── */
    case CMD_JS_UPLOAD: {
      /* Skip the 4-byte random token */
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);

      uint16_t chunk_idx    = TPROTO_GET_NEXT16_PAYLOAD_PARAM16(payload_ptr);
      uint16_t total_chunks = TPROTO_GET_NEXT16_PAYLOAD_PARAM16(payload_ptr);
      uint16_t chunk_size   = TPROTO_GET_NEXT16_PAYLOAD_PARAM16(payload_ptr);
      const uint8_t *data   = (const uint8_t *)payload_ptr;

      uint32_t save = spin_lock_blocking(s_spin_lock);
      if (chunk_idx == 0) {
        s_msg.js_source_len   = 0;
        s_msg.chunks_received = 0;
        s_msg.chunks_expected = total_chunks;
      }
      uint32_t avail = JS_SOURCE_MAX - s_msg.js_source_len;
      uint32_t copy  = (chunk_size < avail) ? chunk_size : avail;
      memcpy(s_msg.js_source + s_msg.js_source_len, data, copy);
      s_msg.js_source_len += copy;
      s_msg.chunks_received++;
      bool last_chunk = (s_msg.chunks_received >= s_msg.chunks_expected);
      spin_unlock(s_spin_lock, save);

      if (last_chunk) {
        /* All chunks received — tell Core 1 to eval */
        multicore_fifo_push_blocking(
            (uint32_t)FIFO_MSG_UPLOAD << FIFO_TAG_SHIFT);
        js_wait_for_core1();
      } else {
        /* Intermediate chunk ACK — write a partial-OK into result memory */
        const char ack[] = "{\"ok\":true,\"partial\":true}";
        size_t     ack_len = ((sizeof(ack) + 1u) & ~1u);
        COPY_AND_CHANGE_ENDIANESS_BLOCK16((void *)ack,
                                          (void *)s_result_mem,
                                          ack_len);
      }
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_CALL ──────────────────────────────────────────────────── */
    case CMD_JS_CALL: {
      /* Reject sync call while an async one is in flight */
      if (s_async_pending) {
        js_write_busy_error();
        js_send_response(random_token);
        break;
      }

      /* Skip the 4-byte random token */
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);
      js_parse_call_payload(payload_ptr);

      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_CALL << FIFO_TAG_SHIFT);
      js_wait_for_core1();
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_RESET ─────────────────────────────────────────────────── */
    case CMD_JS_RESET: {
      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_RESET << FIFO_TAG_SHIFT);
      js_wait_for_core1();
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_CALL_ASYNC ───────────────────────────────────────────── */
    case CMD_JS_CALL_ASYNC: {
      /* Reject if a previous async call is still in flight */
      if (s_async_pending) {
        js_write_busy_error();
        js_send_response(random_token);
        break;
      }

      /* Skip the 4-byte random token */
      TPROTO_NEXT32_PAYLOAD_PTR(payload_ptr);
      js_parse_call_payload(payload_ptr);

      *s_status_mem      = (uint16_t)MDJS_STATUS_BUSY;
      s_async_pending    = true;
      s_async_start_us   = time_us_64();

      multicore_fifo_push_blocking((uint32_t)FIFO_MSG_CALL << FIFO_TAG_SHIFT);
      /* ACK the ST immediately — Core 1 will update status when done */
      js_send_response(random_token);
      break;
    }

    /* ── CMD_JS_POLL ─────────────────────────────────────────────────── */
    case CMD_JS_POLL: {
      /* Write current async status as JSON into the result buffer */
      char status_json[32];
      snprintf(status_json, sizeof(status_json),
               "{\"status\":%u}", (unsigned)*s_status_mem);
      size_t slen = ((strnlen(status_json, sizeof(status_json) - 1) + 2u) & ~1u);
      COPY_AND_CHANGE_ENDIANESS_BLOCK16((void *)status_json,
                                        (void *)s_result_mem,
                                        slen);
      js_send_response(random_token);
      break;
    }

    default:
      DPRINTF("js_dispatch_command: unexpected command 0x%04X\n",
              proto->command_id);
      break;
  }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

void js_worker_init(void) {
  s_rom_base         = (uint32_t)&__rom_in_ram_start__;
  s_token_addr       = s_rom_base + TERM_RANDOM_TOKEN_OFFSET;
  s_token_seed_addr  = s_rom_base + TERM_RANDON_TOKEN_SEED_OFFSET;
  s_result_mem       = (volatile char    *)(s_rom_base + JS_RESULT_OFFSET);
  s_status_mem       = (volatile uint16_t *)(s_rom_base + JS_STATUS_OFFSET);
  *s_status_mem      = (uint16_t)MDJS_STATUS_IDLE;
  s_async_pending    = false;
  s_async_start_us   = 0;

  /* Initialise spin-lock */
  s_spin_lock = spin_lock_instance(JS_SPINLOCK_ID);
  uint32_t save = spin_lock_blocking(s_spin_lock);
  memset(&s_msg, 0, sizeof(s_msg));
  spin_unlock(s_spin_lock, save);

  /* Seed the random token in shared memory */
  srand((unsigned int)time(NULL));
  uint32_t seed = rand();
  TPROTO_SET_RANDOM_TOKEN(s_token_seed_addr, seed);

  DPRINTF("js_worker_init: launching Core 1\n");
  multicore_launch_core1(core1_entry);
  DPRINTF("js_worker_init: Core 1 launched\n");
  DPRINTF("MD-JS ready. PING=0x%02X UPLOAD=0x%02X CALL=0x%02X RESET=0x%02X"
          " CALL_ASYNC=0x%02X POLL=0x%02X\n",
          CMD_JS_PING, CMD_JS_UPLOAD, CMD_JS_CALL, CMD_JS_RESET,
          CMD_JS_CALL_ASYNC, CMD_JS_POLL);
}

void __not_in_flash_func(js_worker_loop)(void) {
  js_drain_async_fifo();

  TransmissionProtocol proto = {0};
  if (term_consume_protocol(&proto)) {
    if (proto.command_id >= CMD_JS_PING &&
        proto.command_id <= CMD_JS_POLL) {
      js_dispatch_command(&proto);
    } else {
      DPRINTF("js_worker_loop: ignoring command 0x%04X\n",
              proto.command_id);
    }
  }
}
