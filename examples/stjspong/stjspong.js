/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * stjspong.js — the JavaScript "brain" for the JS player in STJSPONG.
 *
 * This is the exact source STJSPONG.TOS uploads to the MD/JS worker at start-up
 * (it's embedded as a C string in main.c; this copy is here so you can read and
 * tinker with it). During the match the ST calls paddle() a few times a second
 * through the non-blocking async API and moves the cyan right paddle toward the
 * Y it returns.
 *
 * paddle() predicts where the ball will cross the paddle's plane, reflecting
 * off the top and bottom walls, so the JS player "thinks ahead" rather than
 * just chasing the ball like the native ST player does. Try replacing it with
 * your own strategy!
 *
 * Arguments (all integers, in screen pixels unless noted):
 *   bx, by    ball centre position
 *   bvx, bvy  ball velocity, in 1/16 px per frame (divide by 16 for px/frame)
 *   padX      X of the paddle plane the JS player defends
 *   top, bot  Y bounds the ball centre bounces between
 *
 * Returns: the target Y for the centre of the JS paddle.
 */
function paddle(bx, by, bvx, bvy, padX, top, bot) {
  var vx = bvx / 16;
  var vy = bvy / 16;
  var mid = (top + bot) >> 1;

  // Ball heading away from us: sit in the middle and wait.
  if (vx <= 0) return mid;

  // Frames until the ball centre reaches our plane.
  var t = (padX - bx) / vx;
  if (t < 0) return mid;

  // Where it would be with no walls...
  var y = by + vy * t;

  // ...then fold that into [top, bot] with a triangle-wave reflection so wall
  // bounces are accounted for.
  var span = bot - top;
  if (span <= 0) return by;
  var m = (y - top) % (2 * span);
  if (m < 0) m += 2 * span;
  if (m > span) m = 2 * span - m;

  return Math.round(top + m);
}
