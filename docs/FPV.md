# Analog FPV video (FM composite) with wsdr-studio

Analog FPV drones send their camera's composite video (NTSC or PAL) as FM, mostly at 5.65-5.95 GHz.
This note records what the adapters can do with such a signal, what the app does with it today (the
"analog video" mark), how that was tested without a drone, and what a picture decoder in the app would
take. Receive only: nothing here transmits.

Status (2026-11): the mark and the simulator exist and are tested on simulated signals, including
ones made from real footage. **Nothing here has met a real video transmitter yet**: that is the first
open item (below).

## Can the adapters receive it?

| Adapter / link | Continuous rate | Analog FPV |
|---|---|---|
| 8812BU on USB 3 | 40 MS/s, 40 MHz channel width (full ±20 MHz) | **Yes.** The whole channel (~15-20 MHz occupied) fits with room. |
| 8812BU on USB 3 | 20 MS/s (~18.4 MHz at −3 dB) | Marginal: the outer FM sidebands are cut. Black and white should still be watchable; colour likely suffers. |
| 8812BU on USB 2, 8812CU, 8812EU | 5 MS/s | No. Their gapped 40 MS/s banks hold 205 µs, about 3 lines, then a gap: no picture. |

- **Frequencies.** The FPV bands (A, B, E, F, R: 5645-5945 MHz; L: 5362-5621 MHz) are inside the
  tuning range (5080-6165 MHz, 5 MHz grid; off-grid channels such as R1 5658 are shifted digitally).
  But wifi-sdr's measurements stop at 5885 MHz (ch 177; `notes/ac54-analysis.md`, the Spectral Map
  sweeps): **E6-E8 (5905-5945) and R8 (5917) are tunable but the front end's response there is
  unmeasured.** It may be weaker.
- **Gain.** FM has a constant envelope, so the chip's AGC or fixed gain both work. A nearby
  transmitter (25-800 mW) will overload the adapter at high fixed gain: lower it.
- **Two antennas.** The 8812BU's paths A and B are coherent at 40 MS/s (B leads A by ~146 ns,
  uncompensated). That allows diversity combining like a diversity goggle receiver (not done).
- **The adapter's own artefacts.** The 8822C's centre notch does not matter (the 8812CU cannot stream
  20/40 MS/s continuously). The 8812BU's centre dip at 40 MHz is on 2.4 GHz only (2.4 GHz video
  senders). The n × 40 MHz spurs are weak next to a video carrier.
- **CPU.** FM demodulation is one `atan2` per sample: cheap next to the Wi-Fi frame finder that
  already runs at 40 MS/s.

## The "analog video" mark (implemented)

With **Mark signals** on (Single and Compare, continuous 20 and 40 MHz spans), the signal finder
(`src/signals.cpp`, `Finder::video`) looks for analog video once the finder has warmed up (~13 ms after a start or
retune) and then every 200 ms:

1. The last 0.8-2 ms of samples are FM-demodulated. They are first mixed by their power-weighted mean
   frequency (one complex multiply on the conjugate product, no per-sample mixer), so nothing wraps.
   The result is averaged to 5 MS/s.
2. Line sync is a 4.7 µs plateau at one end of the frequency swing, once per line. Which end depends
   on the transmitter, so both are tried. Pulses 2.5-7.5 µs long, beyond a fifth of the way from
   that end (2% quantile) to the median, are collected.
3. The longest chain of pulses whole line periods apart is kept. Missed pulses and the vertical
   interval's half-line pulses are skipped, up to 10 lines. It must cover at least 60% of the lines
   in the stretch, fit a straight line to within 0.4 µs, and give 15.0-16.5 kHz.
4. Reported: the **line rate** (15.625 kHz: 625 lines, PAL/SECAM; 15.734 kHz: 525 lines, NTSC), the
   **mean frequency** (averaged over the windows), and the **deviation**. Deviation is 3.4 × the sync
   depth (tip to blanking, from the back porch). In both standards white is that far from the sync
   tip, so the figure does not depend on the picture.
5. Narrow bursts and steady carriers inside its band are not reported. The band runs from the sync
   tip to white, ±7 MHz, roughly the occupied band by Carson's rule, sound subcarrier included. Without this
   rule the carrier's dwell on the sync and blanking levels, flat parts of the picture and the sound
   subcarrier's lines produced about 150 "narrowband" marks a second (measured on `main` against the
   simulated recordings below).

The hover reads, e.g., *analog video, FM (NTSC, 525 lines) · ≈5806 MHz, FPV R5 5806 or A4 5805 ·
deviation 8 MHz p-p*. Channel names (`web/src/fpv.ts`) are the analog FPV channels within 3 MHz.

**Measured** (unit tests in `tests/test_core.cpp` `test_video`, and simulated recordings below):

| Signal | Found | Line rate | Frequency error | Deviation estimate |
|---|---|---|---|---|
| PAL, 40 MS/s, sync low, 8 MHz, 15 dB | every window | 15.625 | −0.9 MHz | 8.1 MHz |
| NTSC, 40 MS/s, sync high, 6 MHz, 12 dB | every window | 15.734 | +0.6 MHz | 6.1 MHz |
| NTSC, 20 MS/s, sync low, 8 MHz, 15 dB | every window | 15.734 | −0.8 MHz | 8.2 MHz |
| PAL, 20 MS/s, sync high, 10 MHz, 12 dB | every window | 15.625 | +1.1 MHz | 10.1 MHz |
| PAL on R5 beside Wi-Fi of equal power on ch 157, one 40 MHz span | every window, and 126 Wi-Fi frames still decoded | 15.625 | −0.9 MHz | 8.2 MHz |
| Real footage as NTSC, 40 MS/s, 20 / 10 / 7 dB CNR | every window at all three | 15.73 | ≤ 1 MHz | — |

(CNR is carrier to noise in 20 MHz.) At 7 dB the demodulated picture is already breaking up into FM
"sparklies" (luma correlation 0.68 with the source), and the mark is still there.

Not marked: noise, Wi-Fi, a microwave oven, Bluetooth, a display's clock harmonic switched at a line
rate, and a carrier keyed at 15.625 kHz with sync's duty cycle (`test_signals`, `test_video`).
Cost: about +1.5% of a core at 40 MS/s (the finder over 0.7 s of samples: 0.07 s before, 0.08 s
after).

**Limits:**
- The frequency is the mean over the stretch looked at, which is a horizontal band of the picture.
  Successive windows (199.7 ms apart) land almost on the same band of the frame, so a bright sky can
  bias it by a MHz or two. Channel names are a guide.
- The deviation assumes linear FM and standard levels. Pre-emphasis (if transmitters use it) and real
  transmitters' deviation and sound subcarrier (6.0 or 6.5 MHz) are assumptions until measured.
- The strongest video signal in the span takes the demodulator: a second one is not found.
- Continuous 20/40 MS/s spans only (the Mark signals rule), so in practice the 8812BU on USB 3.
- The picture is not decoded (below).

## Testing without a drone: `scripts/fpv_sim.py`

`make` turns a video (any file ffmpeg reads; ffmpeg's `testsrc2` by default) into a SigMF recording
like the app's own. That's ci16, 12-bit samples << 4, centre in `core:frequency`, with the truth in
`fpv_sim:truth`. It models:

- **The composite:** NTSC 525/59.94 or PAL 625/50 colour (U/V on the subcarrier, PAL's V switch, the
  burst), a continuous line grid, the vertical interval (equalising and serrated broad pulses,
  interlaced: the second field starts mid-line), filtered to 4.2 / 5 MHz.
- **The transmitter:** a 6.5 MHz FM sound subcarrier, then FM with a given deviation (sync tip to
  white) and sync polarity, its long-term mean at the channel frequency (what a VTX's PLL holds).
- **The receiver:** noise at a given CNR in 20 MHz, and at 20 MS/s the adapter's ~18.4 MHz filter
  (an FIR stand-in for its real shape) before decimation.

`check` demodulates a recording back to a black-and-white frame. It finds the sync polarity and
standard, the vertical interval and both fields, writes `PREFIX.check.png` (source | decoded) and
prints the luma correlation with the source. This verifies the generator itself. It is also a minimal
offline reference decoder, and it should work on a real recording made with **● Rec**, without the
comparison.

```sh
scripts/fpv_sim.py make --video clip.mp4 --std ntsc --freq 5806 --centre 5800 --seconds 0.7 --out /tmp/fpv
scripts/fpv_sim.py check /tmp/fpv.sigmf-data        # NTSC (15.736 kHz lines) ... correlation 0.985
build/test_core --wifi /tmp/fpv.sigmf-data          # other signals: A 5806 MHz line 15.73 kHz
```

Picture round trips with 3 s of *Big Buck Bunny* (CC-BY Blender Foundation; used locally, not in
the repository), against the source's luma:

| Variant | Correlation |
|---|---|
| NTSC 40 MS/s, 20 dB CNR | 0.985 |
| NTSC 20 MS/s through the 18.4 MHz filter, 15 dB | 0.978 |
| PAL 40 MS/s, sync high, 10 MHz deviation, 15 dB | 0.969 |
| NTSC 40 MS/s, 10 dB | 0.949 |
| NTSC 40 MS/s, 7 dB (sparklies, torn lines) | 0.676 |

Not modelled: pre-emphasis, the adapter's DC offset and I/Q imbalance, its real filter shape,
multipath, and transmitter non-linearity.

## Watching it today, without code: SDRangel

The SoapySDR module already streams 40 MS/s at 40 MHz channel width on an 8812BU over USB 3 (bandwidth
above 25 MHz, the default at that rate; `src/soapy/SoapyWsdr.cpp` in wifi-sdr). Untested route:
SDRangel with `SOAPY_SDR_PLUGIN_PATH` pointing at the module. Use a SoapySDR input (`driver=wsdr`) at
40 MS/s tuned near the channel, and the ATV demodulator in FM mode with 625 or 525 lines. It shows
the luminance only: black and white.

## A picture decoder in the app (plan)

1. **Backend:** path A (or A+B combined; first compensate B's lead) is mixed to the video's centre,
   then FM-demodulated and low-passed to the video band. It is resampled to a line-locked grid
   (13.5 MS/s, or 4 × the colour subcarrier).
2. **Sync:** a horizontal PLL on the sync leading edges, the vertical sync from the broad pulses,
   then fields woven into frames. `fpv_sim.py check` is a minimal offline version of this.
3. **To the UI:** about 720 × 480/576 at 30/25 frames a second, sent as compressed frames
   (~1 MB/s) or raw 8-bit luma (~10 MB/s) over the existing WebSocket, drawn on a canvas.
4. **Colour:** a burst-locked subcarrier PLL, then U/V demodulation (NTSC: phase from the burst;
   PAL: the V switch and line averaging), and a comb filter to separate luma from chroma. Needs the
   40 MHz mode for the chroma sidebands.
5. **Sound:** the 6.0/6.5 MHz subcarrier through a second discriminator.
6. **De-emphasis:** add it if real transmitters turn out to use pre-emphasis.

## Next steps

1. **A real transmitter.** Record an analog VTX with **● Rec** at 40 MS/s on an 8812BU over USB 3, on
   a few channels including one above 5885 MHz (R8 or E6-E8). Then run `test_core --wifi` and
   `fpv_sim.py check` on the recordings.
2. **Measure real transmitters:** their deviation, sync polarity, pre-emphasis and sound subcarrier;
   the front end's response above 5885 MHz; the deviation and frequency estimates against the VTX's
   set channel.
3. **Cross-check** with SDRangel's ATV demodulator.
4. **The decoder in the app,** black and white first.
