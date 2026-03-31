/**
 * File: mdjs.h
 * Description: MD-JS ST-side client library — public header.
 *
 * Provides a simple C API for Atari ST programs to communicate with the
 * MD-JS JavaScript Worker running on the SidecarTridge RP2040.
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
#define CMD_JS_PING    0x0010
#define CMD_JS_UPLOAD  0x0011
#define CMD_JS_CALL    0x0012
#define CMD_JS_RESET   0x0013

/* ── Shared memory address of the JS result buffer ──────────────────────── */
/* ROM4 base $FA0000 + offset $F100 = $FAF100.                               */
#define JS_RESULT_ADDR ((volatile char *)0xFAF100L)
#define JS_RESULT_SIZE 2048

/* ── Maximum JS bytes per upload chunk ──────────────────────────────────── */
/* MAX_PROTOCOL_PAYLOAD_SIZE (2112) minus 10 bytes of upload header.         */
#define JS_UPLOAD_CHUNK_MAX   2102

/* ── Maximum function name length (including NUL terminator) ────────────── */
#define JS_CALL_FUNC_NAME_MAX 64

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * @brief Ping the MD-JS worker to confirm it is active.
 * On success the version JSON string is available at JS_RESULT_ADDR.
 * @return 0 on success, non-zero on timeout/error.
 */
int mdjs_ping(void);

/**
 * @brief Upload JavaScript source code to the worker.
 * The source is split into chunks automatically if needed.
 * The uploaded code is evaluated immediately after the last chunk is received.
 * @param js_source NUL-terminated JavaScript source string.
 * @return 0 on success, non-zero on error.
 */
int mdjs_upload(const char *js_source);

/**
 * @brief Call a named JavaScript function with JSON arguments.
 * @param func       NUL-terminated function name (max 63 characters).
 * @param args_json  NUL-terminated JSON array string, e.g. "[5,7]".
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

#endif /* MDJS_H */
