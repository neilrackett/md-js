# Changelog

All notable changes to MD/JS are documented here.

## v1.1.0 — 2026-07-13

- Added **STJSPONG** ("Pong Battle: ST vs JS"), a self-playing Pong example that drives the MD/JS worker from a real-time game loop through the non-blocking async API. See [examples/stjspong/](examples/stjspong/).
- Hardened `mdjs_ping()`: worker detection is now fast and timeout-free — it checks the readiness flag before issuing the protocol command, so it no longer risks a long hang or a false positive when no worker is present.
- Added `mdjs_set_settle()` / `mdjs_get_settle()` (plus `MDJS_SETTLE_DEFAULT`) to tune the inter-command settle delay; set it to `0` in a warm loop to remove per-call latency.
