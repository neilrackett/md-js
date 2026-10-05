# Changelog

All notable changes to MD/JS are documented here.

## Unreleased

- Fixed `fetch()` stalling on a response bigger than about 5.8 KB until it timed out: the firmware never told lwIP it had taken the data, so TCP's receive window never reopened.
- `fetch()` now gives the server's `status` and `statusText`, and `ok` is true only for a 2xx status, as in a browser: a 404 used to come back as `ok: true, status: 200`.
- `fetch()` no longer cuts a path over 127 characters short (and fetches another page); a host name that long fails instead of being cut.
- MD/JS answers the ST as soon as it boots: the worker starts before WiFi connects (which can take 30 s), where the ST used to report the worker missing. lwIP is now polled all the time too, so DHCP leases are renewed.
- The httpc library is built for the same `pico_cyw43_arch` flavour as the firmware (it was built for threadsafe_background, with lwIP, the CYW43 driver and mbedtls copied in), and without mbedtls, as TLS is off.
- The README and AGENTS.md: the JavaScript heap is 32 KB, and httpc does not follow redirects.
- MD/JS runs in Hatari on your computer with [EmuMD](https://github.com/neilrackett/emumd), Wi-Fi and all. See "Running it on your computer" in the README; `emu/test.sh` tests `fetch()` and booting with slow WiFi.
- Portability, for that: ROM-in-RAM and flash addresses are kept in `uintptr_t`, `network_scan()`'s nested functions are static functions (clang has no nested functions) and it returns 0 as documented, and `tprotocol.h`'s ARM store is only used on 32-bit ARM.

## v1.1.0 — 2026-07-13

- Added **STJSPONG** ("Pong Battle: ST vs JS"), a self-playing Pong example that drives the MD/JS worker from a real-time game loop through the non-blocking async API. See [examples/stjspong/](examples/stjspong/).
- Hardened `mdjs_ping()`: worker detection is now fast and timeout-free — it checks the readiness flag before issuing the protocol command, so it no longer risks a long hang or a false positive when no worker is present.
- Added `mdjs_set_settle()` / `mdjs_get_settle()` (plus `MDJS_SETTLE_DEFAULT`) to tune the inter-command settle delay; set it to `0` in a warm loop to remove per-call latency.
