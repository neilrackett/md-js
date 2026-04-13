# AGENTS.md — MD/JS Developer & LLM Playbook

Quick reference for humans and LLMs working on this repo. Read this before touching anything.

## What this project is

MD/JS is a microfirmware app for the SidecarTridge Multi-device (RP2040-based ROM cartridge for Atari ST). It exposes a JavaScript Worker: the ST uploads JS source via the cartridge bus, calls named functions with JSON args, and reads back JSON results from a shared memory region at `$FAF100`.

- **Core 0** — ROM emulator + tprotocol command decoder + `js_worker_loop()`
- **Core 1** — JerryScript v3.0.0 runtime (48 KB heap, `JERRY_EXTERNAL_CONTEXT=ON`)
- **ST side** — `mdjs.c` / `mdjs.h` C library + `sidecart_stubs.S` (GAS-syntax wrappers for the bus protocol)

## Environment setup

### Required tools

| Tool                           | Version             | Notes                                                                                             |
| ------------------------------ | ------------------- | ------------------------------------------------------------------------------------------------- |
| ARM GNU Toolchain              | 15.2.rel1 (or 14.x) | Download from developer.arm.com — the Homebrew `arm-none-eabi-gcc` **lacks newlib** and will fail |
| CMake                          | 3.26+               | `brew install cmake`                                                                              |
| atarist-toolkit-docker / stcmd | latest              | For 68000 cross-compile (`vasm`, `vlink`, `m68k-atari-mint-gcc`)                                  |
| Python 3                       | any recent          | Used by build script for firmware.py                                                              |

### Key environment variables

```bash
# Required — point at the ARM GNU Toolchain bin/ dir (not Homebrew arm-none-eabi)
export PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
```

The SDK paths (`PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`) are set automatically by `rp/build.sh` — you do not need to export them.

## Build

```bash
# First time — fetch all submodules
git submodule update --init --recursive

# Build everything
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w debug <UUID>

# Output in dist/:
#   <UUID>-v<version>.uf2   ← flash this to the SidecarTridge
#   <UUID>.json             ← app descriptor (md5 auto-filled)
#   MDJSDEMO.PRG            ← GEM demo for the ST
```

The build script: copies `version.txt` → builds ST target (vasm + m68k-atari-mint-gcc) → generates `target_firmware.h` → builds RP2040 target (CMake + ARM GCC) → assembles dist/.

## Key files

```
rp/src/
  CMakeLists.txt        Add sources, link libraries, JerryScript config here
  emul.c                Firmware entry point — calls js_worker_init() + js_worker_loop()
  js_worker.c           Core 0 dispatcher + Core 1 JerryScript worker
  js_worker.h / include/js_worker.h
                        Command IDs (0x10–0x15), JS_STATUS_OFFSET, MDJS_STATUS_* constants,
                        message block struct, API
  jerry_port.c          Minimal JerryScript port layer (context, log, fatal, stubs)
  term.c / term.h       tprotocol buffer management; exposes term_consume_protocol()
  include/tprotocol.h   TransmissionProtocol struct, MAX_PROTOCOL_PAYLOAD_SIZE (2112)

target/atarist/src/
  mdjs.h / mdjs.c       ST-side C library — include these in ST projects
  sidecart_stubs.S      GAS-syntax translation of send_sync / send_sync_write
  demo_gem.c            Reference GEM application
  main.s                ROM cartridge header + boot stub (vasm devpac syntax)
```

## JerryScript integration — known pitfalls

These caused build failures during initial development:

1. **LTO sets `CMAKE_AR` to host `gcc-ar`** — breaks cross-compilation. Fixed with `set(ENABLE_LTO OFF CACHE BOOL "" FORCE)` before `add_subdirectory(jerryscript)`.

2. **`JERRY_CMDLINE=ON` by default** — tries to build a host CLI executable, fails with arm-none-eabi. Fixed with `JERRY_CMDLINE/TEST/SNAPSHOT=OFF`, `JERRY_PORT=OFF`, `JERRY_EXT=OFF`.

3. **`JERRY_PORT=OFF` removes the port library** — but `jerry-core` still requires `jerry_port_*` symbols. Implemented in `rp/src/jerry_port.c`.

4. **JerryScript 3.0.0 API renames** vs older docs:
   - `jerry_get_value_from_error(val, free)` → `jerry_exception_value(val, free)`
   - `jerry_string_to_char_buffer(str, buf, sz)` → `jerry_string_to_buffer(str, JERRY_ENCODING_UTF8, buf, sz)`

5. **GCC 15 `-Wno-unterminated-string-initialization`** — JerryScript date helper has `char[7][3]` arrays initialised with 4-char string literals. Suppressed only for `jerry-core` via `target_compile_options`.

6. **Homebrew arm-none-eabi-gcc has no newlib sysroot** — download the official ARM GNU Toolchain from developer.arm.com and set `PICO_TOOLCHAIN_PATH` to its `bin/` dir.

## Inter-core protocol

Commands flow ST → Core 0 (tprotocol decode) → FIFO push → Core 1 (JerryScript) → result in ROM-in-RAM → Core 0 writes random token → ST unblocks.

- Shared data: `JsWorkerMsgBlock s_msg` in `js_worker.c`, protected by spin-lock 14 (`JS_SPINLOCK_ID`).
- Result buffer: `__rom_in_ram_start__ + JS_RESULT_OFFSET (0xF100)`, readable by ST at `$FAF100`.
- Async status word: `__rom_in_ram_start__ + JS_STATUS_OFFSET (0xF008)`, readable by ST at `$FAF008` as a single byte (no bus transaction needed).
- Timeout: `JS_CALL_TIMEOUT_US` (5 seconds). On timeout, Core 0 writes `{"error":"timeout"}` and restarts Core 1 with `multicore_reset_core1()`. Both sync and async paths use the shared `js_write_timeout_error()` helper.
- Core 1 calls `jerry_cleanup()` then `jerry_init()` at startup — this handles both first boot and post-timeout restart.

### Async call flow (`CMD_JS_CALL_ASYNC = 0x14`)

The Immediate-ACK pattern: Core 0 ACKs the ST _before_ waiting for Core 1, so the 68000 is unblocked immediately.

```
ST                        Core 0                   Core 1
│  CMD_JS_CALL_ASYNC       │                         │
│──────────────────────────►                         │
│                          │ *s_status_mem = BUSY     │
│                          │ FIFO_MSG_CALL ──────────►│
│  ACK (token written)     │                         │ JS executing...
│◄──────────────────────────│                         │
│  (does other work)       │                         │ JS finishes
│  *$FAF008 == BUSY         │                         │ result → ROM-in-RAM
│  *$FAF008 == DONE ◄────────────────────────────────  │ *s_status_mem = DONE
│  read MDJS_RESULT_ADDR      │                         │ FIFO push
│                          │ js_drain_async_fifo()    │
│                          │ s_async_pending = false  │
```

- `js_drain_async_fifo()` is called at the top of every `js_worker_loop()` iteration — non-blocking, never holds the spin-lock unless a timeout fires.
- Core 1 writes the status byte _inside_ `core1_flush_result()` while holding the spin-lock, so the ST cannot see `DONE` before the result bytes are committed.
- Only one async call can be in flight at a time. Sending `CMD_JS_CALL` or `CMD_JS_CALL_ASYNC` while `s_async_pending` is true returns `{"error":"busy"}` immediately.

### ROM-in-RAM layout

```
Offset    ST address    Purpose
────────────────────────────────────────────────────────────
0xF000    $FAF000       Random token (4 B)
0xF004    $FAF004       Token seed (4 B)
0xF008    $FAF008       Async status word (uint16_t) — low byte = status
0xF00A–0xF03F           Free
0xF040    $FAF040       TERM shared variables (indices 0–15)
0xF100    $FAF100       JS result buffer (2048 B)
```

## ST-side assembly linkage

`vasm` produces aout-format objects incompatible with `m68k-atari-mint-gcc`'s linker. The send_sync protocol routines are therefore re-implemented in GAS syntax in `sidecart_stubs.S` and compiled alongside the C code. Do not try to link vasm `.o` files with GCC.

## Guardrails — do not modify

- `fatfs-sdk/` — third-party SD library
- `pico-sdk/` — Raspberry Pi Pico SDK
- `pico-extras/` — Pico Extras
- `lib/jerryscript/` — JerryScript source (pinned to v3.0.0)

## Troubleshooting quick reference

| Symptom                                         | Cause                                   | Fix                                                                                  |
| ----------------------------------------------- | --------------------------------------- | ------------------------------------------------------------------------------------ |
| `gcc-ar: no such file` at link                  | LTO enabled, sets CMAKE_AR to host tool | `ENABLE_LTO OFF` in CMakeLists                                                       |
| `undefined reference to jerry_port_*`           | JERRY_PORT=OFF removes the port lib     | Symbols provided by `jerry_port.c`                                                   |
| `stdlib.h not found`                            | Homebrew arm-none-eabi lacks newlib     | Use official ARM GNU Toolchain                                                       |
| `-Wunterminated-string-initialization` error    | GCC 15 + JerryScript 3.0 date code      | `target_compile_options(jerry-core PRIVATE -Wno-unterminated-string-initialization)` |
| `stcmd` image not found                         | STCMD_IMAGE_TAG mismatch                | Check `stcmd` version vs app version                                                 |
| Vasm warnings about overflow / trailing garbage | Version strings passed as `-D` macros   | Harmless, ignore                                                                     |
| ST shows "not detected"                         | PING command timed out                  | Check UF2 is flashed; check UART for `MD-JS ready`                                   |
