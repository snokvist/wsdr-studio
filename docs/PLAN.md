# wsdr-studio build plan (MVP)

_2026-10-25 · implements docs/DESIGN.md. Desktop first; live spectrum is the main job._
_Revision 3: web UI (React) + C++ backend, AppImage; independent review folded in (see "Review outcomes")._

## Shape
```
 wsdr-studio (AppImage)
 ├─ wsdr-studiod   C++ backend: adapter + streamer (wifi-sdr/devourer) → DSP → HTTP + WebSocket
 │                 serves the built web UI; 127.0.0.1 only; per-launch token
 └─ web/           React + TypeScript (Vite): spectrum + waterfall (canvas 2D), controls, status
 desktop/          Electron shell (bundled): starts wsdr-studiod --ui-pipe, reads {port, token} from its
                   stdout, shows http://127.0.0.1:PORT/ locked to that origin; token to the page over IPC
```
IQ never leaves the backend; the window gets display rows (≤ 4096 bins, ≤ 30 rows/s) and status JSON.

## Inputs
- **RTL8812BU (0bda:b812):** USB 3 → 2.5–40 MS/s (40 MS/s = 40 MHz width + BW40 DFIR); USB 2 → 2.5/5.
  USB 3 currently needs an rtw88 probe after a replug; MVP shows the link speed and says so.
- **RTL8812CU (0bda:c812):** USB 2 → 2.5/5 MS/s, paths A and B. Both paths are always unpacked; the
  DSP picks A, B or A+B, so switching paths needs no restart.
- One adapter open at a time, by PID.

## Work items

### A. wifi-sdr: a consumable receive core (repo snokvist/wifi-sdr)
1. **Pin devourer.** Add it as a git submodule `external/devourer` (fork, commit 4afa386 on
   `pr/jaguar3-runtime-thread-optional`; switch to upstream master once #497/#498 merge).
   `DEVOURER_DIR` defaults to the submodule. Replace the `no_j3_runtime` SFINAE fallback with a
   hard compile error, so a build against a devourer without `j3_runtime_thread` cannot happen.
2. **Subproject-safe CMake.** `cmake_minimum_required(3.21)`; `CMAKE_CURRENT_SOURCE_DIR` /
   `CMAKE_CURRENT_BINARY_DIR` everywhere; firmware targets renamed `wsdr_fw` / `wsdr_fw_cu`, not
   `ALL` when not top-level; tools/Soapy/tests only when `PROJECT_IS_TOP_LEVEL`.
3. **One core library.** `wsdrcore` gets the 8822C pieces (`stream_cu.cpp`, `cpu8822c.cpp`, the
   8822C firmware rules, defined before `wsdrcore`); `wsdr_cu` and the Soapy module just link it.
4. **Verify with an empty consumer** (a scratch CMake project doing `add_subdirectory(wifi-sdr)` and
   linking `wsdrcore`), then the usual builds (`build`, `build-cu`, `build-soapy`) and a 5 s stream on
   each adapter.
5. **Robustness fixes in the core:**
   - `Session::open`: enumerate + `libusb_open` and report `not found` vs `permission denied`
     (EACCES) vs `busy` distinctly (typed exception).
   - Streamers: move `C2hRxPath` construction inside the try, so an unplug returns a
     `StreamStats` error instead of throwing.
6. **`Receiver`** (`src/receiver.{h,cpp}`): device lifecycle + tuning + streaming, from the Soapy
   worker's logic, but the sink gets **raw banks** (`const uint8_t *bank, size_t bytes, BankMeta`)
   so the stream thread only copies; unpacking, DC block and NCO move to the consumer's thread.
   - Settings: freq, rate_sel, wide, agc, gain_db. Setters record; the worker applies them between
     runs (30 ms settle).
   - Worker wrapped in try/catch; `LIBUSB_ERROR_NO_DEVICE` / repeated failures → state `lost` (no
     100 ms retry loop on a dead handle).
   - `stop()` joins the worker before anything is destroyed; a fresh `Session` per open.
   - Status: state (closed/opening/streaming/retuning/lost/error), banks, faults, last error,
     channel/width/centre/residual shift, measured MS/s.
   - The Soapy module is **not** ported in the MVP (no regression risk); later it can adopt Receiver.

### B. Backend `wsdr-studiod` (repo snokvist/wsdr-studio)
1. CMake; `external/wifi-sdr` submodule (pinned after A). Deps: FFTW3f, Boost.Beast/Asio
   (header-only), nlohmann-json, libusb.
2. **DeviceScan** (libusb + sysfs): pid, bus/port, link speed, host controller PCI address, bound
   kernel driver, openable. No machine-specific controller blacklist; the UI shows speed/controller
   and the live fault count.
3. **Engine:** owns the Receiver; open/close on its own thread with a state machine (requests that
   arrive during the ~5 s bring-up are queued; close-during-open closes when open returns).
   One bounded SPSC ring of raw banks (drop-oldest, counted).
4. **DSP thread:** unpack (A/B), DC block, NCO, FFT (FFTW3f, 1024–16384, Hann). Accumulate
   **linear** power (avg sum and max) per row interval, convert to dB once per row; reduce to
   ≤ 4096 display bins (avg: mean, max: max). FFT size / averaging / path are DSP settings and
   never restart the stream. Feeds the recorder from the same thread (cs16 into the recorder ring).
   **Benchmark with synthetic input at 40 MS/s before wiring the server.**
5. **Recorder:** SigMF `ci16_le` (selected path) + `.sigmf-meta`; writer thread; files only in a
   server-chosen directory (`$XDG_DATA_HOME/wsdr-studio/recordings`); dropped samples reported.
6. **Server (security in from the start):**
   - Bind 127.0.0.1 only. A random token per launch, required on `/ws` (query) and API calls.
   - Check `Origin` against `http://127.0.0.1:PORT` / `http://localhost:PORT` and `Host` against
     the same (DNS rebinding).
   - Static files from a fixed directory, canonicalised, no traversal; MIME by extension.
   - Protocol v1: `hello {version, caps}` first; client requests carry an `id`, answered by
     `ack {id}` or `error {id, msg}`; `status` (2 Hz); binary `row` frames
     {magic, version, seq u32, t_ms u64, channel u8, bins u16, f0 f64, df f64, db_min f32,
     db_step f32, u8[bins] avg, u8[bins] max}.
   - Per-client bounded send queue; a slow client loses rows, the DSP never blocks.
7. **Process lifetime:** the Electron shell owns it: one app instance (a second launch focuses the
   window); the backend runs until the shell closes its stdin (or SIGINT/SIGTERM/SIGHUP, or the shell
   dies: PR_SET_PDEATHSIG). One backend per user (instance file); the shell attaches to a running
   one. CLI: `--ui-pipe`, `--pid`, `--freq`, `--rate`, `--port`, `--record-dir`,
   `--headless-seconds N` (engine + DSP only, prints stats JSON).

### C. Web UI (`web/`)
1. Top bar: adapter select (USB 2/3, controller), Start/Stop, channel picker (2.4 GHz 1–13;
   5 GHz 20 MHz channels and 40 MHz pairs), frequency (MHz), rate (link-limited), 20/40 MHz at
   40 MS/s, gain slider (debounced) + AGC, record.
2. Spectrum: canvas 2D, avg + decaying max-hold, MHz axis, Wi-Fi channel markers, dB range.
3. Waterfall: canvas 2D (row draw + self-scroll), colormap LUT, hover readout (MHz, dB).
4. 8812CU: path A / B / A+B (overlay).
5. Status bar; socket reconnect; protocol errors shown.
6. Double-click to retune.

### D. Packaging
1. A skeleton AppImage right after the backend skeleton (B1 + B6 serving an empty page), then
   kept building.
2. Build inside a Docker container (Ubuntu 22.04, glibc 2.35) for portability; appimagetool with
   the static type-2 runtime (no libfuse2 needed on the host); linuxdeploy bundles libusb, FFTW
   (not libudev, not GL); the pinned Electron release (SHA-256) is added after linuxdeploy; a custom
   AppRun starts it (with `--no-sandbox` only where unprivileged user namespaces are blocked).
3. The firmware headers need mipsel binutils + xxd at build time: install them in the container.

### E. Tests
1. Unit (no hardware): channel/centre maps, `igi_for`, synthetic tone → bin and dBFS, linear→dB row
   reduction, row frame encode/decode (C++ and TS), SigMF meta, token/Origin/Host checks.
2. DSP benchmark at 40 MS/s synthetic (target: < 50 % of one core).
3. Headless hardware: 8812BU 40 MS/s 20 s, 8812CU 5 MS/s 20 s, retune mid-run, unplug → `lost`.
4. The real UI on the desktop (DriftWM region), screenshots in `docs/`; a 5 s recording checked.
5. Locks (`~/wifi-sdr/tools/coord.sh`) around every adapter use.

## Order
A1 → A2/A3 → A4 → A5 → A6 (wifi-sdr, pushed) → B1 → B6 skeleton + D1 → B2/B3 → B4 + benchmark →
B6 protocol → C → B5 → D2/D3 → E throughout. Commit per step.

## Deferred (M2+)
Soapy port to Receiver; A/B coherence and phase (needs the ~400 ns lead removed); two-file A+B
SigMF; remote `--listen` (needs auth); USB 3 switch without rtw88; rtw88 handover/reattach; band
scan; burst detector; playback; 80 MS/s snapshot mode.
New decoders (transmitter audio, drone Remote ID, radar, V2X car messages, ExpressLRS and more): see
[FUTURE-DECODERS.md](FUTURE-DECODERS.md).

## Review outcomes (2026-10-25)
Independent review against the wifi-sdr code. Taken: on_bank runs on the libusb reaper thread
(copy raw banks only, one ring); unplug can throw out of `run()` (constructor outside the try);
`CMAKE_SOURCE_DIR` breaks subproject builds; devourer unpinned + silent SFINAE fallback; EACCES
indistinguishable; bins/f0 precision; WebSocket CSRF / DNS-rebinding; log10 per bin cost;
protocol ids/acks/version; lifetimes and single instance; AppImage glibc/libfuse2. Not taken: the
claim that `la.restore()`/`rx.restore()` throw (each write is already wrapped in try/catch).
