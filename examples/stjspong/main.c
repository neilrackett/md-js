/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * stjspong.c — STJSPONG, "Pong Battle: ST vs JS" for the Atari ST (low res).
 *
 * A self-playing Pong match that pits a native 68000 AI (the "ST" player, the
 * green left paddle) against a JavaScript AI running on the SidecarTridge MD/JS
 * worker (the "JS" player, the cyan right paddle). The JS brain is uploaded to
 * the worker at start-up and then driven every few frames through the
 * non-blocking MD/JS async API:
 *
 *     mdjs_call_async("paddle", state)  →  mdjs_status()  →  mdjs_result()
 *
 * so the 68000 keeps rendering a smooth 50 fps while Core 1 predicts where the
 * ball is going. The JS side reacts with a touch of lag, which reads as it
 * "thinking ahead" against the ST's simpler reactive chase.
 *
 * The ball speeds up every few paddle bounces (shown as "SPEED n" at the top)
 * until someone can't keep up; the rally speed resets on each point.
 *
 * MD/JS is detected on the splash with mdjs_ping():
 *   - worker present -> ST vs JS
 *   - worker absent  -> ST vs ST fallback (both paddles native, no MD/JS calls)
 *
 * Pass -mock on the command line to run ST vs ST but present it as ST vs JS
 * (the splash says "worker ready" and the right paddle is labelled JS). It
 * makes no MD/JS calls, so it is for recording ST-vs-JS-looking clips under
 * Hatari, where the worker isn't emulated.
 *
 * Controls: the match plays itself. ESC = quit. First to 5 wins.
 * Needs a colour monitor (ST low res).
 *
 * Build (from the repo root): STCMD_NO_TTY=1 make examples
 */

#include <osbind.h>
#include <string.h>

#include "mdjs.h"

#define LINE_BYTES 160
#define SCREEN_SIZE 32000L

/* positions/velocities are fixed point, 1/16 pixel */
#define FP 4

#define WALL_TOP 16 /* grey walls at y 16-17 and 196-197 */
#define WALL_BOT 196
#define PLAY_TOP (WALL_TOP + 2)
#define PLAY_BOT WALL_BOT /* exclusive */

#define PAD_H 32
#define LEFT_X 8 /* paddles are 4 px wide */
#define RIGHT_X 308
#define ST_SPEED 13 /* left (ST) paddle px/frame — faster but reactive */
#define JS_SPEED 13 /* right (JS) paddle px/frame — matched to ST so it recovers from async lag */

#define BALL_VX 200      /* serve speed, 1/16 px/frame (~12.5 px/frame) */
#define VX_STEP 14       /* speed up per paddle hit */
#define VX_MAX 320       /* ~20 px/frame — swept collision keeps it catchable */
#define SERVE_DELAY 24   /* frames the ball waits at centre */
#define WIN_SCORE 5      /* first to 5 wins — snappy for a self-playing demo */
#define HITS_PER_LEVEL 4 /* paddle hits between displayed speed increments */

/* How often (frames) to fire a fresh async prediction request at the worker.
 * The paddle moves toward the last target every frame between fires, so this
 * just sets how often the JS target is refreshed. main() sets the MD/JS settle
 * to 0 for the game loop, so mdjs_call_async doesn't stall the render. */
#define JS_FIRE_INTERVAL 1

/* Match modes */
#define MODE_ST_VS_JS 0
#define MODE_ST_VS_ST 1

/* Splash return values */
#define SPLASH_QUIT (-1)

/* ── The JavaScript brain uploaded to the MD/JS worker ─────────────────────
 * Mirrors examples/stjspong/stjspong.js. paddle() is called with the ball
 * state and returns the target Y for the centre of the JS (right) paddle,
 * predicting where the ball will cross the paddle plane (reflecting off the
 * top and bottom walls). Velocities arrive in 1/16 px per frame. */
static const char JS_BRAIN[] =
    "function paddle(bx,by,bvx,bvy,padX,top,bot){"
    "var vx=bvx/16,vy=bvy/16;"
    "var mid=(top+bot)>>1;"
    "if(vx<=0)return mid;"
    "var t=(padX-bx)/vx;"
    "if(t<0)return mid;"
    "var y=by+vy*t;"
    "var span=bot-top;"
    "if(span<=0)return by;"
    "var m=(y-top)%(2*span);"
    "if(m<0)m+=2*span;"
    "if(m>span)m=2*span-m;"
    "return Math.round(top+m);"
    "}";

/* 8x8 font, only the glyphs the game needs */
static const char font_chars[] = "0123456789ABCDEFGHIJKLMNOPRSTUVWXY=!/";
static const unsigned char font[][8] = {
    {0x7C, 0xC6, 0xCE, 0xD6, 0xE6, 0xC6, 0x7C, 0x00}, /* 0 */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 1 */
    {0x7C, 0xC6, 0x06, 0x1C, 0x30, 0x60, 0xFE, 0x00}, /* 2 */
    {0x7C, 0xC6, 0x06, 0x1C, 0x06, 0xC6, 0x7C, 0x00}, /* 3 */
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00}, /* 4 */
    {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00}, /* 5 */
    {0x3C, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00}, /* 6 */
    {0xFE, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}, /* 7 */
    {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00}, /* 8 */
    {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00}, /* 9 */
    {0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0x00}, /* A */
    {0xFC, 0xC6, 0xC6, 0xFC, 0xC6, 0xC6, 0xFC, 0x00}, /* B */
    {0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00}, /* C */
    {0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00}, /* D */
    {0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xFE, 0x00}, /* E */
    {0xFE, 0xC0, 0xC0, 0xF8, 0xC0, 0xC0, 0xC0, 0x00}, /* F */
    {0x7C, 0xC6, 0xC0, 0xDE, 0xC6, 0xC6, 0x7C, 0x00}, /* G */
    {0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00}, /* H */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* I */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0x78, 0x00}, /* J */
    {0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0xC6, 0x00}, /* K */
    {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFE, 0x00}, /* L */
    {0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00}, /* M */
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00}, /* N */
    {0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00}, /* O */
    {0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00}, /* P */
    {0xFC, 0xC6, 0xC6, 0xFC, 0xD8, 0xCC, 0xC6, 0x00}, /* R */
    {0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00}, /* S */
    {0xFE, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x00}, /* T */
    {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00}, /* U */
    {0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00}, /* V */
    {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00}, /* W */
    {0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00}, /* X */
    {0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x38, 0x38, 0x00}, /* Y */
    {0x00, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0x00, 0x00}, /* = */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00}, /* / */
};

/* --- screen --- */

static unsigned char *buf[2]; /* double buffer */
static int draw_buf;
static unsigned char *back; /* buffer being drawn into */

/* --- game state --- */

static int mode;         /* MODE_ST_VS_JS / MODE_ST_VS_ST */
static int js_available; /* 1 if the worker accepted the brain and answers */
static int label_js;     /* present the right paddle as "JS" (real JS or -mock) */

static int left_y, right_y;  /* paddle top y, pixels */
static int bx, by, bvx, bvy; /* ball top-left, 1/16 px */
static int serve_timer;      /* ball held at centre while > 0 */
static int left_score, right_score;
static int left_wins, right_wins; /* games won this session (HUD counters) */
static int rally_hits;            /* paddle hits this rally */
static int speed_level;           /* 1 + rally_hits / HITS_PER_LEVEL */
static int left_bias, right_bias; /* per-rally aim error, grows with speed */
static long frame;
static int quit;

/* JS async paddle tracking */
static int js_inflight;   /* an async call is outstanding */
static int js_target;     /* last target Y for the JS paddle centre */
static long js_last_fire; /* frame of the last async fire */

static void clear_back(void) { memset(back, 0, SCREEN_SIZE); }

/* OR a sprite into every bitplane set in color (palette index 1-15) */
static void draw_sprite(int x, int y, const unsigned short *rows, int h,
                        int color) {
  unsigned char *p = back + (long)y * LINE_BYTES + ((x >> 4) << 3);
  int shift = x & 15;
  int last_group = (x >> 4) >= 19;
  int i, pl;

  for (i = 0; i < h; i++) {
    unsigned long d = ((unsigned long)rows[i] << 16) >> shift;
    unsigned short hi = (unsigned short)(d >> 16);
    unsigned short lo = (unsigned short)d;
    for (pl = 0; pl < 4; pl++) {
      if (color & (1 << pl)) {
        *(unsigned short *)(p + (pl << 1)) |= hi;
        if (lo && !last_group) *(unsigned short *)(p + (pl << 1) + 8) |= lo;
      }
    }
    p += LINE_BYTES;
  }
}

/* 4-pixel-wide vertical bar (paddles and the ball) */
static void draw_bar(int x, int y, int h, int color) {
  unsigned char *p = back + (long)y * LINE_BYTES + ((x >> 4) << 3);
  int shift = x & 15;
  int last_group = (x >> 4) >= 19;
  unsigned long d = 0xF0000000UL >> shift;
  unsigned short hi = (unsigned short)(d >> 16);
  unsigned short lo = (unsigned short)d;
  int i, pl;

  for (i = 0; i < h; i++) {
    for (pl = 0; pl < 4; pl++) {
      if (color & (1 << pl)) {
        *(unsigned short *)(p + (pl << 1)) |= hi;
        if (lo && !last_group) *(unsigned short *)(p + (pl << 1) + 8) |= lo;
      }
    }
    p += LINE_BYTES;
  }
}

/* full-width horizontal bar, 2 px tall (the court walls) */
static void draw_wall(int y, int color) {
  unsigned char *p = back + (long)y * LINE_BYTES;
  int row, g, pl;

  for (row = 0; row < 2; row++) {
    unsigned char *q = p;
    for (g = 0; g < 20; g++) {
      for (pl = 0; pl < 4; pl++)
        if (color & (1 << pl)) {
          q[pl << 1] = 0xFF;
          q[(pl << 1) + 1] = 0xFF;
        }
      q += 8;
    }
    p += LINE_BYTES;
  }
}

/* spread the 8 bits of b over 16 bits (each pixel doubled) */
static unsigned short double_bits(unsigned char b) {
  unsigned short r = 0;
  int i;

  for (i = 0; i < 8; i++)
    if (b & (0x80 >> i)) r |= 0xC000 >> (i * 2);
  return r;
}

static void draw_text(int x, int y, const char *s, int color) {
  while (*s) {
    if (*s != ' ') {
      int g = 0;
      while (font_chars[g] && font_chars[g] != *s) g++;
      if (font_chars[g]) {
        unsigned short rows[8];
        int i;
        for (i = 0; i < 8; i++) rows[i] = (unsigned short)font[g][i] << 8;
        draw_sprite(x, y, rows, 8, color);
      }
    }
    x += 8;
    s++;
  }
}

/* draw_text at double size (16x16 glyphs) */
static void draw_text2x(int x, int y, const char *s, int color) {
  while (*s) {
    if (*s != ' ') {
      int g = 0;
      while (font_chars[g] && font_chars[g] != *s) g++;
      if (font_chars[g]) {
        unsigned short rows[16];
        int i;
        for (i = 0; i < 8; i++) {
          rows[i * 2] = double_bits(font[g][i]);
          rows[i * 2 + 1] = rows[i * 2];
        }
        draw_sprite(x, y, rows, 16, color);
      }
    }
    x += 16;
    s++;
  }
}

/* 3x5 mini-font for the splash credit (low 3 bits per row, MSB = left) */
static const char font3_chars[] = "X.COM/NEILRAKTGHUB";
static const unsigned char font3[][5] = {
    {5, 5, 2, 5, 5}, /* X */
    {0, 0, 0, 0, 2}, /* . */
    {7, 4, 4, 4, 7}, /* C */
    {7, 5, 5, 5, 7}, /* O */
    {5, 7, 7, 5, 5}, /* M */
    {1, 1, 2, 4, 4}, /* / */
    {6, 5, 5, 5, 5}, /* N */
    {7, 4, 6, 4, 7}, /* E */
    {7, 2, 2, 2, 7}, /* I */
    {4, 4, 4, 4, 7}, /* L */
    {6, 5, 6, 5, 5}, /* R */
    {2, 5, 7, 5, 5}, /* A */
    {5, 6, 4, 6, 5}, /* K */
    {7, 2, 2, 2, 2}, /* T */
    {3, 4, 5, 5, 3}, /* G */
    {5, 5, 7, 5, 5}, /* H */
    {5, 5, 5, 5, 7}, /* U */
    {6, 5, 6, 5, 6}, /* B */
};

/* 3x5 mini text; advance 4px per char (3 wide + 1 gap) */
static void draw_text3x5(int x, int y, const char *s, int color) {
  while (*s) {
    if (*s != ' ') {
      int g = 0;
      while (font3_chars[g] && font3_chars[g] != *s) g++;
      if (font3_chars[g]) {
        unsigned short rows[5];
        int i;
        for (i = 0; i < 5; i++) rows[i] = (unsigned short)font3[g][i] << 13;
        draw_sprite(x, y, rows, 5, color);
      }
    }
    x += 4;
    s++;
  }
}

/* draw a decimal integer with draw_text; returns nothing */
static void draw_int(int x, int y, int v, int color) {
  char s[12];
  char tmp[12];
  int n = 0, i = 0;
  unsigned int u;

  if (v < 0) {
    s[i++] = '-';
    u = (unsigned int)(-v);
  } else {
    u = (unsigned int)v;
  }
  if (u == 0) {
    tmp[n++] = '0';
  } else {
    while (u) {
      tmp[n++] = (char)('0' + (u % 10));
      u /= 10;
    }
  }
  while (n) s[i++] = tmp[--n];
  s[i] = '\0';
  draw_text(x, y, s, color);
}

/* draw a decimal integer at double size */
static void draw_int2x(int x, int y, int v, int color) {
  char s[12];
  char tmp[12];
  int n = 0, i = 0;
  unsigned int u;

  if (v < 0) {
    s[i++] = '-';
    u = (unsigned int)(-v);
  } else {
    u = (unsigned int)v;
  }
  if (u == 0) {
    tmp[n++] = '0';
  } else {
    while (u) {
      tmp[n++] = (char)('0' + (u % 10));
      u /= 10;
    }
  }
  while (n) s[i++] = tmp[--n];
  s[i] = '\0';
  draw_text2x(x, y, s, color);
}

/* --- input --- */

/* Return the next raw scancode/char waiting, or -1 if none. */
static int poll_key(void) {
  if (Cconis()) return (int)(Crawcin() & 0xFF);
  return -1;
}

static void flush_keys(void) {
  while (Cconis()) (void)Crawcin();
}

static int rnd(int n) { return (int)(Random() % n); }

/* show what was just drawn, start drawing into the other buffer */
static void flip(void) {
  Setscreen(buf[draw_buf ^ 1], buf[draw_buf], -1);
  draw_buf ^= 1;
  back = buf[draw_buf];
  Vsync();
}

/* --- helpers for the JS bridge --- */

static char *put_int(char *p, int v) {
  char tmp[12];
  int n = 0;
  unsigned int u;

  if (v < 0) {
    *p++ = '-';
    u = (unsigned int)(-v);
  } else {
    u = (unsigned int)v;
  }
  if (u == 0) {
    *p++ = '0';
    return p;
  }
  while (u) {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  }
  while (n) *p++ = tmp[--n];
  return p;
}

/* Parse a leading (optionally signed) integer; stops at '.' or any non-digit.
 * Returns 1 and writes *out on success, 0 if there was no number. */
static int parse_int(const char *s, int *out) {
  int sign = 1;
  int got = 0;
  long v = 0;

  while (*s == ' ' || *s == '\t' || *s == '"') s++;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    got = 1;
    s++;
    if (v > 100000L) break;
  }
  if (!got) return 0;
  *out = (int)(sign * v);
  return 1;
}

static int clamp_centre(int c) {
  int lo = PLAY_TOP + PAD_H / 2;
  int hi = PLAY_BOT - PAD_H / 2;

  if (c < lo) c = lo;
  if (c > hi) c = hi;
  return c;
}

/* Predict where the ball will cross the RIGHT paddle plane, reflecting off the
 * top and bottom walls. Native mirror of the JS paddle() brain — used for the
 * ST-vs-ST right paddle and as a fallback when the worker can't answer. */
static int predict_right(void) {
  int top = PLAY_TOP + 2;
  int bot = PLAY_BOT - 2;
  int cx = (bx >> FP) + 2;
  int cy = (by >> FP) + 2;
  int mid = (top + bot) / 2;
  int span = bot - top;
  long dx, y, m;

  if (bvx <= 0) return mid;
  dx = (long)(RIGHT_X - cx);
  if (dx < 0) return mid;
  /* y = cy + (bvy/16) * (dx / (bvx/16)) = cy + bvy*dx/bvx */
  y = (long)cy + (long)bvy * dx / (long)bvx;
  if (span <= 0) return cy;
  m = (y - top) % (2L * span);
  if (m < 0) m += 2L * span;
  if (m > span) m = 2L * span - m;
  return (int)(top + m);
}

/* Reactive chase for the LEFT (ST) paddle: it follows the ball's current height
 * at all times. Unlike the JS paddle it never predicts the bounce, so it lags
 * on fast, angled shots — that lag is its handicap against the predictor. */
static int chase_left(void) { return (by >> FP) + 2; }

/* Build the JSON args for a paddle() call from the current ball state. */
static void build_js_args(char *buf_out) {
  char *p = buf_out;
  int cx = (bx >> FP) + 2;
  int cy = (by >> FP) + 2;
  int top = PLAY_TOP + 2;
  int bot = PLAY_BOT - 2;

  *p++ = '[';
  p = put_int(p, cx);
  *p++ = ',';
  p = put_int(p, cy);
  *p++ = ',';
  p = put_int(p, bvx);
  *p++ = ',';
  p = put_int(p, bvy);
  *p++ = ',';
  p = put_int(p, RIGHT_X);
  *p++ = ',';
  p = put_int(p, top);
  *p++ = ',';
  p = put_int(p, bot);
  *p++ = ']';
  *p = '\0';
}

/* Drive the JS (right) paddle target via the non-blocking MD/JS async API.
 * Reads the previous prediction if it's ready, then fires a fresh one. Falls
 * back to the native predictor for this frame if a submit fails. */
static void update_js_target(void) {
  if (js_inflight) {
    unsigned char st = mdjs_status();
    if (st == MDJS_STATUS_DONE) {
      char res[24];
      int v;
      mdjs_result(res, (int)sizeof(res));
      if (parse_int(res, &v)) js_target = clamp_centre(v);
      js_inflight = 0;
    } else if (st == MDJS_STATUS_ERROR) {
      js_inflight = 0; /* keep the last good target */
    }
    /* else BUSY: leave js_target as-is */
  }

  if (!js_inflight && (frame - js_last_fire) >= JS_FIRE_INTERVAL) {
    char args[64];
    build_js_args(args);
    if (mdjs_call_async("paddle", args) == 0) {
      js_inflight = 1;
      js_last_fire = frame;
    } else {
      /* couldn't submit — fall back this frame so the paddle never stalls */
      js_target = clamp_centre(predict_right());
    }
  }
}

/* --- court + HUD --- */

static void draw_court(void) {
  int y;

  draw_wall(WALL_TOP, 8);
  draw_wall(WALL_BOT, 8);
  for (y = PLAY_TOP + 2; y < PLAY_BOT - 4; y += 8) draw_bar(159, y, 4, 8);
}

static void draw_scores(void) {
  /* left (ST) score in green, right (JS) score in cyan */
  if (left_score >= 10)
    draw_int2x(112, 24, left_score, 5);
  else
    draw_int2x(128, 24, left_score, 5);
  draw_int2x(176, 24, right_score, 6);
}

static const char *right_name(void) { return label_js ? "JS" : "ST"; }

/* pixel width of v drawn as a decimal (win counts are small non-negatives) */
static int int_px_width(int v) {
  int digits = 1;
  unsigned int u = (unsigned int)v;
  while (u >= 10) {
    digits++;
    u /= 10;
  }
  return digits * 8;
}

static void draw_hud(void) {
  /* player labels top-left / top-right, each with a white games-won count
     tucked against it: right of ST, left of JS */
  draw_text(8, 4, "ST", 5);
  draw_int(32, 4, left_wins, 1);
  draw_text(296, 4, right_name(), 6);
  draw_int(288 - int_px_width(right_wins), 4, right_wins, 1);
  /* current rally speed, centred */
  draw_text(132, 4, "SPEED", 4);
  draw_int(180, 4, speed_level, 4);
}

static void draw_frame(void) {
  clear_back();
  draw_court();
  draw_scores();
  draw_hud();
  draw_bar(LEFT_X, left_y, PAD_H, 5);
  draw_bar(RIGHT_X, right_y, PAD_H, 6);
  if (!serve_timer || (frame & 4)) draw_bar(bx >> FP, by >> FP, 4, 1);
  flip();
}

/* --- game logic --- */

/* Re-roll each paddle's aim error for the next exchange. The error grows with
 * the rally speed, and the reactive ST (left) paddle is given a bigger spread
 * than the predictive JS (right) paddle, so the ball eventually gets past a
 * paddle — points happen, rallies don't run forever — and the JS player, being
 * the sharper shot, tends to come out ahead. */
static int aim_error(int amp) {
  if (amp <= 0) return 0;
  return rnd(2 * amp + 1) - amp;
}

static void randomize_bias(void) {
  /* On real hardware the JS paddle acts on predictions a few frames stale (the
   * MD/JS async round-trip), which the native ST paddle doesn't have — so ST
   * was winning comfortably. Give JS the tighter aim (smaller error) to pay
   * back that reaction lag; ST is the reactive, sloppier shot. */
  int la = 5 * speed_level / 2;  /* ST aim spread, ~2.5x */
  int ra = 9 * speed_level / 4;  /* JS 2.25x — a shade tighter than ST, but
                                    eased from 1.75x so ST can steal games */

  if (la > 22) la = 22;
  if (ra > 20) ra = 20;
  left_bias = aim_error(la);
  right_bias = aim_error(ra);
}

/* dir: +1 = serve toward JS (right), -1 = serve toward ST (left) */
static void serve(int dir) {
  bx = 158 << FP;
  by = ((PLAY_TOP + PLAY_BOT) / 2 - 2) << FP; /* centre vertically (ball is 4px) */
  bvx = dir * BALL_VX;
  bvy = rnd(2) ? 48 : -48;
  serve_timer = SERVE_DELAY;
  rally_hits = 0;
  speed_level = 1;
  randomize_bias();
}

static void reset_game(void) {
  left_score = 0;
  right_score = 0;
  left_y = (PLAY_TOP + PLAY_BOT - PAD_H) / 2;
  right_y = left_y;
  left_bias = 0;
  right_bias = 0;
  js_inflight = 0;
  js_last_fire = 0;
  js_target = (PLAY_TOP + PLAY_BOT) / 2;
  serve(rnd(2) ? 1 : -1);
}

/* ball hit a paddle: reflect, speed up, set spin from the hit offset */
static void paddle_hit(int pad_y) {
  int off = ((by >> FP) + 2) - (pad_y + PAD_H / 2);

  bvx = -bvx;
  if (bvx > 0)
    bvx += VX_STEP;
  else
    bvx -= VX_STEP;
  if (bvx > VX_MAX) bvx = VX_MAX;
  if (bvx < -VX_MAX) bvx = -VX_MAX;
  bvy = off * 6;

  rally_hits++;
  speed_level = 1 + rally_hits / HITS_PER_LEVEL;
  randomize_bias();
}

static void move_paddle(int *py, int target_centre, int max_speed) {
  int centre = *py + PAD_H / 2;
  int d = target_centre - centre;

  if (d > max_speed) d = max_speed;
  if (d < -max_speed) d = -max_speed;
  *py += d;
  if (*py < PLAY_TOP) *py = PLAY_TOP;
  if (*py > PLAY_BOT - PAD_H) *py = PLAY_BOT - PAD_H;
}

static void play_frame(void) {
  int left_target, right_target;

  frame++;

  /* LEFT paddle = ST native reactive chaser, both modes */
  left_target = chase_left() + left_bias;

  /* RIGHT paddle = JS (async) in ST-vs-JS, native predictor otherwise */
  if (mode == MODE_ST_VS_JS && js_available) {
    update_js_target();
    right_target = js_target + right_bias;
  } else {
    right_target = predict_right() + right_bias;
  }

  move_paddle(&left_y, left_target, ST_SPEED);
  move_paddle(&right_y, right_target, JS_SPEED);

  if (serve_timer) {
    serve_timer--;
    return;
  }

  {
    int bx_old = bx;

    bx += bvx;
    by += bvy;

    /* walls */
    if (by < PLAY_TOP << FP) {
      by = PLAY_TOP << FP;
      bvy = -bvy;
    }
    if (by > (PLAY_BOT - 4) << FP) {
      by = (PLAY_BOT - 4) << FP;
      bvy = -bvy;
    }

    /* Paddles: swept plane-crossing test. Checking whether the ball's
     * leading edge crossed the paddle face this frame (rather than sitting
     * inside a narrow window) means a fast ball can't tunnel through the
     * paddle in a single step. */
    if (bvx < 0) {
      int face = (LEFT_X + 4) << FP;
      if (bx_old > face && bx <= face && (by >> FP) + 4 > left_y &&
          (by >> FP) < left_y + PAD_H) {
        bx = face;
        paddle_hit(left_y);
      }
    } else if (bvx > 0) {
      int face = (RIGHT_X - 4) << FP;
      if (bx_old < face && bx >= face && (by >> FP) + 4 > right_y &&
          (by >> FP) < right_y + PAD_H) {
        bx = face;
        paddle_hit(right_y);
      }
    }
  }

  /* out: point to the other side, serve toward the loser */
  if (bx < -(16 << FP)) {
    right_score++;
    if (right_score < WIN_SCORE) serve(-1);
  } else if (bx > 336 << FP) {
    left_score++;
    if (left_score < WIN_SCORE) serve(1);
  }
}

/* --- splash + between-point screens --- */

/* Wait for a key. Returns 1 if ESC (quit), 0 otherwise. Keeps the court and
 * the message on screen. */
/* _hz_200: the ST's 200 Hz system timer at $4BA. It lives in supervisor-only
   low memory, so read it via Supexec into a global. Basing the countdown on
   real time (not a frame counter) keeps it a true 9 seconds regardless of the
   machine's frame rate. */
static volatile long g_ticks;
static void read_hz200(void) { g_ticks = *(volatile long *)0x4BAL; }

static int wait_start(const char *m, int color) {
  long start;
  int k;
  char cd[16];

  Supexec(read_hz200);
  start = g_ticks;

  flush_keys();
  for (;;) {
    /* Netflix-style auto-start: count down 9..1 over 9 real seconds (200 ticks
       each), then start the next game. A single digit keeps the line a fixed
       14 chars, centred at x = (320 - 14*8)/2 = 104. A key still starts
       immediately, so it can be left running as an attract screensaver. */
    int secs;
    Supexec(read_hz200);
    secs = 9 - (int)((g_ticks - start) / 200);
    if (secs <= 0) return 0;

    clear_back();
    draw_court();
    draw_scores();
    draw_hud();
    draw_bar(LEFT_X, left_y, PAD_H, 5);
    draw_bar(RIGHT_X, right_y, PAD_H, 6);
    draw_text2x((int)(320 - 16 * (int)strlen(m)) / 2, 88, m, color);
    strcpy(cd, "NEXT GAME IN ");
    cd[13] = (char)('0' + secs);
    cd[14] = '\0';
    draw_text(104, 120, cd, 1);
    flip();
    k = poll_key();
    if (k == 27) return 1;
    if (k >= 0) return 0;
  }
}

/* Title screen with a self-playing demo rally. detected != 0 shows the
 * "worker ready" splash (a real MD/JS worker, or -mock). Returns 0 to start
 * the match, or SPLASH_QUIT if ESC was pressed. */
static int splash(int detected) {
  long t = 0;
  int dbx = 150, dby = 110, dvx = 3, dvy = 2;
  int dlp = 100, drp = 100, c, k;

  flush_keys();
  for (;;) {
    clear_back();
    /* title: ST (green) JS (cyan) PONG (white) */
    draw_text2x(96, 20, "ST", 5);
    draw_text2x(128, 20, "JS", 6);
    draw_text2x(160, 20, "PONG", 1);

    /* subtitle (centred: 33 chars x 8px = 264, x = (320-264)/2 = 28) */
    draw_text(28, 48, "WELCOME TO THE ULTIMATE SHOWDOWN!", 1);

    /* matchup line: 1986 (green) VS (white) 2026 (cyan) */
    draw_text(112, 60, "1986", 5);
    draw_text(152, 60, "VS", 1);
    draw_text(176, 60, "2026", 6);

    /* matchup line: ST (green) VS (white) JS (cyan) */
    draw_text(128, 72, "ST", 5);
    draw_text(152, 72, "VS", 1);
    draw_text(176, 72, "JS", 6);

    /* demo rally */
    draw_bar(48, dlp, 24, 5);
    draw_bar(268, drp, 24, 6);
    draw_bar(dbx, dby, 4, 1);

    if (detected) {
      draw_text(88, 154, "MD/JS WORKER READY", 5);
      if ((t & 63) < 44) draw_text(72, 168, "PRESS ANY KEY TO START", 1);
    } else {
      draw_text(88, 154, "MD/JS NOT DETECTED", 2);
      if ((t & 63) < 44)
        draw_text(36, 168, "PRESS ANY KEY FOR ST VS ST DEMO", 1);
    }

    draw_text3x5(126, 194, "X.COM/NEILRACKETT", 8);
    flip();
    t++;

    /* animate the demo rally: ball bounces, paddles chase it */
    dbx += dvx;
    dby += dvy;
    if (dbx <= 52) {
      dbx = 52;
      dvx = 3;
      dvy = rnd(5) - 2;
    }
    if (dbx >= 264) {
      dbx = 264;
      dvx = -3;
      dvy = rnd(5) - 2;
    }
    if (dby <= 84) {
      dby = 84;
      dvy = -dvy;
    }
    if (dby >= 148) {
      dby = 148;
      dvy = -dvy;
    }
    c = dby - 10;
    if (dlp < c) dlp += 2;
    if (dlp > c) dlp -= 2;
    if (dvx > 0) {
      if (drp < c) drp += 2;
      if (drp > c) drp -= 2;
    }
    if (dlp < 84) dlp = 84;
    if (dlp > 128) dlp = 128;
    if (drp < 84) drp = 84;
    if (drp > 128) drp = 128;

    k = poll_key();
    if (k == 27) return SPLASH_QUIT;
    if (k >= 0) return 0; /* any key starts */
  }
}

/* --- MD/JS setup --- */

/* Upload the brain and confirm the worker actually answers with a sane value.
 * Returns 1 if the JS paddle can be driven, 0 to fall back to the native AI. */
static int init_js_worker(void) {
  char res[32];
  int v;

  if (mdjs_upload(JS_BRAIN) != 0) return 0;

  /* One synchronous sanity call: ball dead-centre moving right with no
   * vertical speed should predict y == 100. If the worker isn't really
   * answering (e.g. stray success over empty cartridge space) the result
   * won't parse to anything near that, so we fall back to the native AI. */
  if (mdjs_call("paddle", "[100,100,40,0,308,20,194]", res, (int)sizeof(res)) !=
      0)
    return 0;
  if (!parse_int(res, &v)) return 0;
  if (v < 40 || v > 160) return 0;
  return 1;
}

/* Case-insensitive exact match of an argument (leading -/ stripped) to want. */
static int arg_is(const char *a, const char *want) {
  while (*a == '-' || *a == '/') a++;
  while (*want) {
    char c = *a;
    if (c >= 'A' && c <= 'Z') c += 32; /* tolower */
    if (c != *want) return 0;
    a++;
    want++;
  }
  return *a == '\0';
}

/* "-mock": run ST vs ST but present it as ST vs JS, for recording social-media
 * clips under Hatari. Makes no MD/JS calls, so it is safe with no worker. */
static int wants_mock(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++)
    if (argv[i] && arg_is(argv[i], "mock")) return 1;
  return 0;
}

int main(int argc, char **argv) {
  void *old_phys, *old_log, *blk;
  short old_palette[16];
  int old_rez, i;
  int present, mock, choice;

  old_rez = (int)Getrez();
  if (old_rez == 2) {
    (void)Cconws("STJSPONG needs a colour monitor (ST low res).\r\n");
    return 1;
  }

  blk = (void *)Malloc(SCREEN_SIZE + 256);
  if (!blk) {
    (void)Cconws("Out of memory.\r\n");
    return 1;
  }

  old_phys = Physbase();
  old_log = Logbase();
  for (i = 0; i < 16; i++) old_palette[i] = Setcolor(i, -1);

  buf[0] = (unsigned char *)old_phys;
  buf[1] = (unsigned char *)(((unsigned long)blk + 255) & ~255UL);
  memset(buf[0], 0, SCREEN_SIZE);
  memset(buf[1], 0, SCREEN_SIZE);
  draw_buf = 1;
  back = buf[1];
  Setscreen(buf[0], buf[0], 0); /* low res */
  (void)Setcolor(0, 0x000);     /* black */
  (void)Setcolor(1, 0x777);     /* white: ball, text */
  (void)Setcolor(2, 0x700);     /* red */
  (void)Setcolor(3, 0x750);     /* orange */
  (void)Setcolor(4, 0x770);     /* yellow: speed */
  (void)Setcolor(5, 0x070);     /* green: ST paddle */
  (void)Setcolor(6, 0x077);     /* cyan: JS paddle */
  (void)Setcolor(7, 0x707);     /* magenta */
  (void)Setcolor(8, 0x444);     /* grey: walls, centre line */
  for (i = 9; i < 16; i++) (void)Setcolor(i, 0x000);

  mock = wants_mock(argc, argv);

  /* Detect the worker (fast, timeout-free). -mock skips the probe entirely so
   * it never touches the bus, and presents as "detected" on the splash. */
  present = 0;
  if (!mock) present = (mdjs_ping() == 0);

  quit = 0;
  choice = splash(present || mock);
  if (choice != SPLASH_QUIT) {
    js_available = 0;
    if (mock) {
      mode = MODE_ST_VS_ST; /* native AI... */
      label_js = 1;         /* ...disguised as JS for recordings */
    } else if (present) {
      mode = MODE_ST_VS_JS;
      js_available = init_js_worker();
      /* The upload + sanity call above have warmed and settled the worker, so
       * drop the per-command settle: the game loop fires mdjs_call_async every
       * few frames and the settle would stall the render. */
      if (js_available) mdjs_set_settle(0);
      label_js = 1; /* detected -> present as JS (native fallback if it glitches) */
    } else {
      mode = MODE_ST_VS_ST; /* honest no-worker fallback */
      label_js = 0;
    }

    while (!quit) {
      reset_game();
      while (left_score < WIN_SCORE && right_score < WIN_SCORE) {
        int k = poll_key();
        if (k == 27) {
          quit = 1;
          break;
        }
        play_frame();
        draw_frame();
      }
      if (!quit) {
        if (right_score >= WIN_SCORE) {
          char msg[16];
          right_wins++;
          strcpy(msg, right_name());
          strcat(msg, " WINS");
          if (wait_start(msg, 6)) break;
        } else {
          left_wins++;
          if (wait_start("ST WINS", 5)) break;
        }
      }
    }
  }

  /* restore everything */
  Setscreen(old_log, old_phys, old_rez);
  for (i = 0; i < 16; i++) (void)Setcolor(i, old_palette[i]);
  (void)Mfree(blk);

  return 0;
}
