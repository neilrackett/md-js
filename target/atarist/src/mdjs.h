/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mdjs.h
 * Description: MD/JS ST-side client library — public header.
 *
 * Provides a simple C API for Atari ST programs to communicate with the
 * MD/JS JavaScript Worker running on the SidecarTridge RP2040.
 *
 * All functions return 0 on success, non-zero on error or timeout.
 * Results are returned as NUL-terminated strings in a caller-supplied buffer.
 *
 * Usage example:
 *   if (mdjs_ping() == 0) {
 *       mdjs_upload("function add(a,b){ return a+b; }");
 *       char result[64];
 *       mdjs_call("add", "[5,7]", result, sizeof(result));
 *       // result now contains "12"
 *   }
 */

#ifndef MDJS_H
#define MDJS_H

/* ── Command IDs (must match js_worker.h on the RP2040 side) ────────────── */
#define CMD_JS_PING         0x0010
#define CMD_JS_UPLOAD       0x0011
#define CMD_JS_CALL         0x0012
#define CMD_JS_RESET        0x0013
#define CMD_JS_CALL_ASYNC   0x0014  /* Non-blocking call                      */
#define CMD_JS_POLL         0x0015  /* Poll async status                      */

/* ── Async status word ──────────────────────────────────────────────────── */
/* ROM4 base $FA0000 + offset $F008 = $FAF008.                               */
/* The RP2040 stores the status in the high byte of this bus word, so a      */
/* byte read at even address $FAF008 sees the status value directly.         */
#define MDJS_STATUS_ADDR ((volatile unsigned char *)0xFAF008L)

/* Status values (must match MDJS_STATUS_* in js_worker.h) */
#define MDJS_STATUS_IDLE  0x00  /* No async call in progress                  */
#define MDJS_STATUS_BUSY  0x01  /* Core 1 is executing                        */
#define MDJS_STATUS_DONE  0x02  /* Result ready at MDJS_RESULT_ADDR             */
#define MDJS_STATUS_ERROR 0x03  /* Error string at MDJS_RESULT_ADDR             */

/* ── Worker-ready signal ────────────────────────────────────────────────── */
/* ROM4 base $FA0000 + offset $F00A = $FAF00A.                               */
/* The RP2040 writes MDJS_READY_MAGIC to BOTH bytes of this bus word once     */
/* Core 1 has finished initialising and is idle, so a byte read at $FAF00A    */
/* returns the magic directly with no bus transaction. It reads back as 0     */
/* while the worker is busy, and as 0 (or open-bus garbage) when no           */
/* SidecarTridge / worker is present — which lets mdjs_ping() fail fast       */
/* instead of stalling on the protocol timeout. Must match MDJS_READY_* in    */
/* rp/src/include/mdjs_protocol.h. */
#define MDJS_READY_ADDR  ((volatile unsigned char *)0xFAF00AL)
#define MDJS_READY_MAGIC 0x4A

/* ── Shared memory address of the JS result buffer ──────────────────────── */
/* ROM4 base $FA0000 + offset $F100 = $FAF100.                               */
#define MDJS_RESULT_ADDR ((volatile char *)0xFAF100L)
#define JS_RESULT_SIZE 2048

/* ── Maximum JS bytes per upload chunk ──────────────────────────────────── */
/* MAX_PROTOCOL_PAYLOAD_SIZE (2112) minus 16-byte send_sync_write header.    */
#define JS_UPLOAD_CHUNK_MAX   2096

/* ── Maximum function name length (including NUL terminator) ────────────── */
#define JS_CALL_FUNC_NAME_MAX 64

/* ── Maximum call argument JSON length (including NUL terminator) ───────── */
#define JS_CALL_ARGS_MAX 2032

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Ping the MD/JS worker to confirm it is active.
 * First checks the worker-ready signal at MDJS_READY_ADDR (a zero-overhead
 * read); if the worker isn't present this returns immediately instead of
 * blocking on the protocol timeout. When the worker is present it then runs
 * the protocol ping so the version JSON string lands at MDJS_RESULT_ADDR.
 * @return 0 on success, non-zero if the worker is absent or on timeout/error.
 */
int mdjs_ping(void);

/**
 * @brief Upload JavaScript source code to the worker.
 * The source is split into chunks automatically if needed.
 * The uploaded code is evaluated immediately after the last chunk is received.
 * @param js_source NUL-terminated JavaScript source string (non-NULL).
 * @return 0 on success, non-zero on error.
 */
int mdjs_upload(const char *js_source);

/**
 * @brief Call a named JavaScript function with JSON arguments.
 * @param func       NUL-terminated function name (max 63 characters, non-NULL).
 * @param args_json  NUL-terminated JSON array string, e.g. "[5,7]".
 *                   Effective max is 2031 bytes when func is 63 chars.
 * @param result     Caller-supplied buffer for the JSON result string.
 * @param result_size Size of the result buffer in bytes.
 * @return 0 on success, non-zero on error.
 */
int mdjs_call(const char *func, const char *args_json,
              char *result, int result_size);

/**
 * @brief Reset the JS context, clearing all uploaded code.
 * @return 0 on success, non-zero on error.
 */
int mdjs_reset(void);

/**
 * @brief Submit an async JS function call and return immediately.
 * The RP2040 ACKs before Core 1 has finished executing. Poll for completion
 * with mdjs_status() or mdjs_poll().
 * @param func      NUL-terminated function name (max 63 characters, non-NULL).
 * @param args_json NUL-terminated JSON array string, e.g. "[5,7]" (non-NULL).
 *                  Effective max is 2031 bytes when func is 63 chars.
 * @return 0 on success (call submitted), MDJS_STATUS_BUSY if a prior async
 *         call is still in flight, non-zero on protocol error.
 */
int mdjs_call_async(const char *func, const char *args_json);

/**
 * @brief Copy the current result buffer from ROM-in-RAM into a C buffer.
 * Use this after mdjs_call(), or after mdjs_call_async() once the status is
 * MDJS_STATUS_DONE or MDJS_STATUS_ERROR.
 * @param result Caller-supplied buffer for the JSON result string.
 * @param result_size Size of the result buffer in bytes.
 * @return 0 on success, non-zero if result is NULL or result_size <= 0.
 */
int mdjs_result(char *result, int result_size);

/**
 * @brief Read the async status byte directly from ROM-in-RAM.
 * Zero-overhead — no bus transaction, just a memory read.
 * @return One of MDJS_STATUS_IDLE / BUSY / DONE / ERROR.
 */
unsigned char mdjs_status(void);

/**
 * @brief Send CMD_JS_POLL and return the current async status.
 * Use this when you want the RP2040 to write a JSON status object to
 * MDJS_RESULT_ADDR (e.g. {"status":2}) in addition to reading the status.
 * For a plain non-blocking check, prefer mdjs_status().
 * @return One of MDJS_STATUS_IDLE / BUSY / DONE / ERROR, or non-zero on error.
 */
int mdjs_poll(void);

/* ── Inter-command settle delay ─────────────────────────────────────────── */
/* Every command-emitting call except mdjs_ping()/mdjs_poll() busy-waits this
 * many loop iterations before sending, to dodge a back-to-back command race on
 * a freshly-booted SidecarTridge. It is a raw busy-loop count (so it scales
 * with CPU speed), not wall-clock time. */
#define MDJS_SETTLE_DEFAULT 25000L

/**
 * @brief Set the inter-command settle busy-loop length, in iterations.
 * Persists until changed. Pass 0 to disable it — safe for a caller issuing
 * commands in a warm, naturally-spaced loop (e.g. a game firing a prediction
 * every few frames) once the worker is known-good; a flaky setup can raise it.
 * @param iterations 0 to disable, or a positive count (negative clamps to 0).
 */
void mdjs_set_settle(long iterations);

/**
 * @brief Get the current settle busy-loop length, in iterations.
 */
long mdjs_get_settle(void);

#endif /* MDJS_H */
