# MD-JS: JavaScript Worker Microfirmware for SidecarTridge Multi-device

## Project Goal
Build a microfirmware that acts like a **web worker** for the Atari ST: upload arbitrary JavaScript code from the ST, execute it in a persistent context on the RP2040, call functions with arguments, and receive results back. Useful for general offload (math, data processing, scripting, prototyping algorithms like C2P).

## Key Requirements
- Persistent JS context across calls.
- Upload script once, then repeatedly call named functions.
- Support argument/result passing (prefer JSON for flexibility, with binary fallback for large data).
- Keep cartridge bus fully responsive while JS runs on the second core.
- Detection + graceful fallback on ST side.

## Technology Choices
- **JS Runtime**: Kaluma (JerryScript-based, designed for RP2040) – small footprint, supports modern-ish JS, JSON.parse/stringify, event loop.
- Base: `md-microfirmware-template`.
- Dual-core: Core 0/PIO for bus (`tprotocol`), Core 1 for Kaluma runtime + worker.

## Protocol Sketch
- `CMD_JS_UPLOAD`: Send JS source code.
- `CMD_JS_CALL_JSON`: Send `{"func": "name", "args": [arg1, arg2]}` → return `{"success": true, "result": value}` or error.
- Simple dispatcher in JS: `workerAPI.call(funcName, args)`.

## Starting Points & References
- `md-microfirmware-template`[](https://github.com/sidecartridge/md-microfirmware-template)
- Kaluma embedding examples for RP2040/Pico SDK.
- Official programming guide: https://docs.sidecartridge.com/sidecartridge-multidevice/programming/
- Use `tprotocol.h` for command handling and response.

## Immediate Next Steps
1. Clone template and integrate Kaluma as a library.
2. Implement basic upload + eval, then JSON-based call dispatcher.
3. Write minimal ST-side C test .PRG (detection + add(5,7) example).
4. Add error handling, persistent context, and large payload support.
5. Test in Hatari for ST code, then real hardware for JS execution.

## Risks / Notes
- JS is interpreted → slower than native C for tight loops (e.g., full C2P may need hybrid native helper).
- Memory: Kaluma + bus handler must fit in 264 KB SRAM.
- Later: Expose RP2040 PIO/C functions to JS for acceleration.

## Related Conversation Context
User wants a flexible "send JS → call functions → get results" worker. C2P was considered but deprioritized in favor of native solutions where speed matters.