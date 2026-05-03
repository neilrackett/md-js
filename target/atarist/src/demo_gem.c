/**
 * File: demo_gem.c
 * Description: MD/JS GEM demonstration application for Atari ST.
 *
 * Demonstrates the MD/JS JavaScript Worker:
 *   1. Pings the worker to confirm it is available.
 *   2. Uploads a simple "add" function: function add(a,b){ return a+b; }
 *   3. Calls add(5, 7) and displays the result in a GEM alert dialog.
 *
 * Build with m68k-atari-mint-gcc (or equivalent 68000 cross-compiler):
 *   m68k-atari-mint-gcc -m68000 -O2 -fomit-frame-pointer \
 *       mdjs.c demo_gem.c -lgem -o MDJSDEMO.PRG
 *
 * This is a standalone demo — run it from GEM desktop or place it in AUTO/.
 */

#include <stdio.h>
#include <string.h>

/* GEM AES / VDI bindings — provided by MiNTLib or Pure C */
#include <gem.h>

#include "mdjs.h"

/* Maximum length of a form_alert string */
#define ALERT_BUF_SIZE 128

int main(void) {
  int app_id;
  char alert_str[ALERT_BUF_SIZE];
  char result[64];
  int err;

  /* Initialise GEM application */
  app_id = appl_init();
  if (app_id < 0) {
    /* Cannot even open a dialog — just exit */
    return 1;
  }

  /* ── 1. Ping the MD/JS worker ──────────────────────────────────── */
  err = mdjs_ping();
  if (err != 0) {
    form_alert(1, "[1][MD/JS worker|not detected on|this device.][OK]");
    appl_exit();
    return 0;
  }

  /* ── 2. Upload JavaScript source ───────────────────────────────── */
  err = mdjs_upload("function add(a,b){ return a+b; }");
  if (err != 0) {
    form_alert(1, "[1][MD/JS upload|failed.][OK]");
    appl_exit();
    return 0;
  }

  /* ── 3. Call add(5, 7) ─────────────────────────────────────────── */
  memset(result, 0, sizeof(result));
  err = mdjs_call("add", "[5,7]", result, (int)sizeof(result));
  if (err != 0) {
    form_alert(1, "[1][MD/JS call|failed.][OK]");
    appl_exit();
    return 0;
  }

  /* ── 4. Show the result ────────────────────────────────────────── */
  snprintf(alert_str, ALERT_BUF_SIZE, "[1][MD/JS Demo|add(5,7) = %s][OK]",
           result);
  form_alert(1, alert_str);

  appl_exit();
  return 0;
}
