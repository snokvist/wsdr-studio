# wsdr-studio design

_2026-10-25 · status: proposal, nothing built_

## Goal
One desktop app that owns the dongle and shows the live spectrum. No SoapySDR install, no gqrx, no
plugin paths, no manual driver juggling: install, plug in, open the app.

**Main job:** live spectrum and waterfall of Wi-Fi bands, tuned by channel.
**Non-goals:** transmitting (receive-only by design); audio demodulation; Windows/macOS for now.

## What already exists (in wifi-sdr)
| Piece | State |
|---|---|
| 8812BU (RTL8822B) streamer | 2.5–40 MS/s continuous on USB 3 (good host port), 40 MHz band at 40 MS/s; 2.5/5 MS/s on USB 2 |
| 8812CU (RTL8822C) streamer | 2.5/5 MS/s on USB 2, two coherent antennas (B leads A by ~400 ns) |
| Fixed gain / AGC | both chips (IGI pin with CCA off; ~0.95 / ~1.0 dB per step) |
| Retune | ~150 ms, 5 MHz grid + digital fine shift |
| Integrity checks | framing, sequence, deadline, staleness on every bank |
| DSP blocks | DC block, NCO (`iqdsp`) |
| Soapy module, rtl_tcp server, recorder | working; the app does not need them |

## Architecture
```
 ┌──────────────────────────── wsdr-studio (one process) ───────────────────────────┐
 │  UI thread (Dear ImGui + ImPlot, GLFW, OpenGL 3)                                 │
 │    device panel · tuning · gain · spectrum · waterfall · record button           │
 │          ▲ snapshots (spectrum rows, stats)          │ commands (tune, gain, …)  │
 │  ┌───────┴──────────────── engine ───────────────────▼──────────────────────────┐ │
 │  │ device manager: find adapter, USB2/3 + host-controller check, kernel-driver   │ │
 │  │   handover (take from rtw88 on open, give back on exit), permissions check   │ │
 │  │ stream thread: wifi-sdr Stream8822b / Stream8822c → sample ring              │ │
 │  │ DSP thread: DC block → FFT (window, size) → average / max-hold → rows        │ │
 │  │ recorder: IQ to disk (SigMF)                                                 │ │
 │  └───────────────────────────────────────────────────────────────────────────────┘ │
 └───────────────────────────────────────────────────────────────────────────────────┘
```
- **Engine separate from UI**, so a TUI or a network server can follow without touching it.
- **Settings changes restart the stream** (as the Soapy module does): the chip is never touched while
  streaming.
- **Dependencies:** wifi-sdr as a git submodule (it brings devourer as its own submodule; both pinned).
  wifi-sdr needs a small refactor first: export the core (`wsdrcore`, the 8822C streamer, firmware
  headers) as a clean CMake library target with no hard-coded devourer path.
- **Toolkit:** Dear ImGui + ImPlot. It is what SDR++ uses, handles a 40 MS/s waterfall at 60 fps, and
  links straight to the C++ core. FFT via pocketfft (header-only) or FFTW.

## DSP budget
- 40 MS/s, 8192-point FFT: ~4900 FFTs/s, well within one core. Display rows are averaged (or
  max-held) down to the screen rate, ~30–60 rows/s.
- Default view: FFT 4096–16384 selectable, Hann window, average N, optional max-hold and peak trace.
- The waterfall texture is updated one row at a time on the GPU.

## MVP (milestone 1)
- **Device panel:** which adapter, USB 2/3, host controller (warn on the known-bad one), available
  rates, kernel-driver state, with a one-line fix for each problem.
- **Tuning:** a Wi-Fi channel picker (20 MHz channels and 40 MHz pairs, 2.4 and 5 GHz) plus free
  frequency entry; 20/40 MHz; sample rate.
- **Gain:** fixed-gain slider (dB) or chip AGC.
- **Display:** spectrum (average + max-hold) and waterfall, frequency axis in MHz with channel
  markers; colour range and FFT size controls.
- **Record:** one button, IQ + SigMF metadata.
- **Both adapters:** path A / B / both, plus the A/B coherence and phase view.

```
┌ wsdr-studio ─────────────────────────────────────────────────────────────────────┐
│ [8812BU · USB3 · 67:00.3 ✓]  Channel [36+40 ▾] 5190.000 MHz  40 MS/s  Gain [■■■□] │
├──────────────────────────────────────────────────────────────────────────────────┤
│  spectrum  ‾‾‾\_/‾‾‾‾‾‾‾‾‾‾‾‾\__/‾‾‾‾‾‾‾‾‾‾‾                    ch36 | ch40          │
├──────────────────────────────────────────────────────────────────────────────────┤
│  waterfall                                                                       │
│  ░░▒▒▓▓▒▒░░░░░░░░░░░░░░░░░░░░░░░░                                                │
├──────────────────────────────────────────────────────────────────────────────────┤
│ ● REC   FFT 8192  avg 8  range −110…−20 dBFS   banks 1,234,567  faults 0           │
└──────────────────────────────────────────────────────────────────────────────────┘
```

## Later (milestone 2+)
- **Band scan:** step 40 MHz blocks across a band, stitch a band-wide view, per-channel busy time.
- **Burst / frame detector:** timestamps, length, power per burst.
- **Playback** of SigMF recordings.
- **80 MS/s snapshot mode** (full 80 MHz in short bursts; above the streaming ceiling).
- **TUI mode** and a built-in network server (rtl_tcp / SoapyRemote) for headless boxes.

## Packaging
- `.deb` or AppImage. One-time setup needs root (udev rule; decision on the rtw88 blacklist), offered
  on first run through pkexec.
- **To test first:** whether the app can take the 8812BU from rtw88 on open (after rtw88 switched it
  to USB 3) and hand it back on exit. If yes, no blacklist is needed and setup is one step.
- Licence: GPL-2.0-only; the build includes devourer's embedded Realtek firmware (linux-firmware
  redistributable licence).

## Milestones
| # | Scope |
|---|---|
| M0 | wifi-sdr core as a clean library; submodules; empty ImGui window that lists adapters |
| M1 | MVP above on the 8812BU (USB 3, 40 MS/s) |
| M2 | 8812CU two-path view; recording; first-run setup |
| M3 | band scan, burst detector, playback |
| M4 | packaging (.deb / AppImage), TUI / server |

## Open questions
- rtw88 handover (above) — decides the setup story.
- Whether 40 MHz centres other than the standard pairs behave like 36/40 (only that pair is measured).
- The 8812CU on USB 3: this board is USB 2 only; another 8812CU board might differ.
