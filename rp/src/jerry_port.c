/**
 * File: jerry_port.c
 * Description: Minimal JerryScript port layer for RP2040.
 *
 * Provides the symbols required by jerry-core when JERRY_PORT=OFF and
 * JERRY_EXTERNAL_CONTEXT=ON. The context block (jerry_context_t + heap) is
 * allocated from the heap once at jerry_init() and freed at jerry_cleanup().
 */

#include "jerryscript-port.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ── Context (JERRY_EXTERNAL_CONTEXT=ON) ──────────────────────────────────── */

static jerry_context_t *current_context_p = NULL;

size_t jerry_port_context_alloc(size_t context_size)
{
    /* 48 KB heap — must match JERRY_GLOBAL_HEAP_SIZE in CMakeLists.txt */
    size_t total = context_size + 48u * 1024u;
    current_context_p = (jerry_context_t *)malloc(total);
    return total;
}

jerry_context_t *jerry_port_context_get(void)
{
    return current_context_p;
}

void jerry_port_context_free(void)
{
    free(current_context_p);
    current_context_p = NULL;
}

/* ── Process ──────────────────────────────────────────────────────────────── */

void jerry_port_init(void) {}

void JERRY_ATTR_NORETURN jerry_port_fatal(jerry_fatal_code_t code)
{
    (void)code;
    abort();
}

/* ── I/O ──────────────────────────────────────────────────────────────────── */

void jerry_port_log(const char *message_p)
{
    fputs(message_p, stderr);
}

/* ── Filesystem (no-op stubs — modules not used) ─────────────────────────── */

jerry_char_t *jerry_port_path_normalize(const jerry_char_t *path_p, jerry_size_t path_size)
{
    (void)path_size;
    /* Return a copy so the caller can unconditionally free it. */
    return (jerry_char_t *)strdup((const char *)path_p);
}

void jerry_port_path_free(jerry_char_t *path_p)
{
    free(path_p);
}

jerry_size_t jerry_port_path_base(const jerry_char_t *path_p)
{
    (void)path_p;
    return 0;
}

jerry_char_t *jerry_port_source_read(const char *file_name_p, jerry_size_t *out_size_p)
{
    (void)file_name_p;
    *out_size_p = 0;
    return NULL;
}

void jerry_port_source_free(jerry_char_t *buffer_p)
{
    free(buffer_p);
}

/* ── Date (stub — Date object not needed on the ST side) ─────────────────── */

double jerry_port_current_time(void)
{
    return 0.0;
}

int32_t jerry_port_local_tza(double unix_ms)
{
    (void)unix_ms;
    return 0;
}
