/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * fetchtst.c — FETCHTST.TOS, MD/JS's fetch() tested under EmuMD (emu/test.sh
 * runs it). Fetches each URL in C:\URLS.TXT with JavaScript on the worker
 * and writes what came back to C:\RESULTS.TXT, a line each: the URL, then
 * the result's JSON. "done" ends it.
 */

#include <osbind.h>
#include <stdio.h>
#include <string.h>

#include "mdjs.h"

static const char JS[] =
    "async function get(url) {\n"
    "  const r = await fetch(url);\n"
    "  const t = await r.text();\n"
    "  return {ok: r.ok, status: r.status, length: t.length,\n"
    "          start: t.slice(0, 16)};\n"
    "}\n";

/* An async call, so a slow fetch cannot trip the bus protocol's timeout;
 * the worker gives up by itself after 10 s. */
static void get(const char *url, char *result, int size) {
  static char args[300];
  snprintf(args, sizeof(args), "[\"%s\"]", url);
  if (mdjs_call_async("get", args) != 0) {
    snprintf(result, (size_t)size, "{\"error\":\"call failed\"}");
    return;
  }
  for (int frames = 0; frames < 60 * 50; frames++) {
    const unsigned char status = mdjs_status();
    if (status == MDJS_STATUS_DONE || status == MDJS_STATUS_ERROR) {
      mdjs_result(result, size);
      return;
    }
    Vsync();
  }
  snprintf(result, (size_t)size, "{\"error\":\"no answer\"}");
}

int main(void) {
  static char url[256], result[JS_RESULT_SIZE];
  FILE *in = fopen("C:\\URLS.TXT", "r");
  FILE *out = fopen("C:\\RESULTS.TXT", "w");
  if (!in || !out) return 1;

  int present = 0;
  for (int tries = 0; tries < 10 && !present; tries++) {
    present = mdjs_ping() == 0;
    if (!present) Vsync();
  }
  if (!present || mdjs_upload(JS) != 0) {
    fprintf(out, "no MD/JS\n");
  } else {
    while (fgets(url, sizeof(url), in)) {
      url[strcspn(url, "\r\n")] = 0;
      if (!url[0]) continue;
      (void)Cconws(url);
      (void)Cconws("\r\n");
      get(url, result, sizeof(result));
      fprintf(out, "%s %s\n", url, result);
    }
  }
  fprintf(out, "done\n");
  fclose(out);
  fclose(in);
  return 0;
}
