# Analog FPV video (FM composite) with wsdr-studio

Analog FPV drones send their camera's composite video (NTSC or PAL) as FM, mostly at 5.65-5.95 GHz.
This note records what the adapters can do with such a signal, what the app does with it today (the
"analog video" mark), how that was tested without a drone, and what a picture decoder in the app would
take. Receive only: nothing here transmits.

Status (2026-11): the mark and the simulator exist and are tested on simulated signals, including ones
made from real footage. **Nothing here has met a real video transmitter yet**: that is the first open
item (below).

## Can the adapters receive it?

| Adapter / link | Continuous rate | Analog FPV |
|---|---|---|
| 8812BU on USB 3 | 40 MS/s, 40 MHz channel width (full ±20 MHz) | **Yes.** The whole channel (~15-20 MHz occupied) fits with room. |
| 8812BU on USB 3 | 20 MS/s (~18.4 MHz at −3 dB) | Marginal: the outer FM sidebands are cut. Black and white should still be watchable when tuned to it; colour likely suffers. |
| 8812BU on USB 2, 8812CU, 8812EU | 5 MS/s | No. Their gapped 40 MS/s banks hold 205 µs, about 3 lines, then a gap: no picture. |

- **Frequencies.** The FPV bands (A, B, E, F, R: 5645-5945 MHz; L: 5362-5621 MHz) are inside the
  tuning range (5080-6165 MHz, 5 MHz grid; off-grid channels such as R1 5658 are shifted digitally).
  But nothing in wifi-sdr's evidence covers more than 5885 MHz: its Spectral Map sweeps stop at 5880 MHz.
  (`notes/ac54-analysis.md` records that the third-party ac54_sdr tool tuned up to 5885 MHz on an ASUS
  USB-AC54.) **E6-E8 (5905-5945) and R8 (5917) are tunable, but the front end's response there is
  unmeasured.** It may be weaker.
- **Gain.** FM has a constant envelope, so the chip's AGC or fixed gain both work. A nearby
  transmitter (25-800 mW) will overload the adapter at high fixed gain: lower it.
- **Two antennas.** The 8812BU's paths A and B are coherent at 40 MS/s (B leads A by ~146 ns,
  uncompensated). That allows diversity combining like a diversity goggle receiver (not done).
- **The adapter's own artefacts.**
  - The 8822B has a DC spike at its centre. The detector takes it off (below).
  - A ~1 MHz centre dip was seen once on an 8812BU at 40 MS/s on 2.4 GHz, and not at 5190 MHz. It is
    not yet measured, and its band and AGC dependence is open (wifi-sdr FINDINGS).
  - The 8822C's centre notch does not matter: the 8812CU cannot stream 20/40 MS/s continuously.
  - The n × 40 MHz spurs are weak next to a video carrier.
- **CPU.** FM demodulation is one `atan2` per sample: cheap next to the Wi-Fi frame finder that
  already runs at 40 MS/s.

## The "analog video" mark (implemented)

With **Mark signals** on (Single and Compare, continuous 20 and 40 MHz spans), the signal finder
(`src/signals.cpp`, `Finder::video`) looks for analog video once its warm-up ends (~13 ms after a start
or retune). It then looks once per 200 ms window, at a pseudo-random point within the following 20 ms
(a field), so successive looks fall on different parts of the picture.

1. **The stretch.** The last 0.8-2 ms of samples, with their mean taken off (the adapter's DC spike;
   an FM carrier averages out).
2. **Sub-bands.** At 40 MS/s, three 20 MHz sub-bands centred −10, 0 and +10 MHz are looked at
   separately. Each is mixed by quarter turns, so no trig is needed, then halfband-filtered (flat to
   ±8 MHz, −39 dB at ±12) and taken to 20 MS/s. Wi-Fi on the next channel stays out of the
   demodulator, the noise is halved, and two transmitters 20 MHz apart are both found. At 20 MS/s the
   span is the only sub-band.
3. **Demodulation.** FM-demodulated about the sub-band's centre (the sub-bands overlap, so a signal is
   within 5 MHz of one's centre). The result is averaged to 2.5 MS/s with a 3-point median, so an FM
   click is no pulse edge.
4. **Sync.** Line sync is a 4.7 µs plateau at one end of the frequency swing, once per line. Which end
   depends on the transmitter, so both are tried. Pulses 2.5-7.5 µs long beyond a fifth of the way from
   that end to the median form a chain of whole line periods. The chain must:
   - skip misses and the vertical interval's half-line pulses (up to 10 lines);
   - cover at least 60% of the lines;
   - fit to within 0.4 µs, at 15.0-16.5 kHz;
   - show **the same power during the pulses as between them** (FM; a keyed carrier is not video);
   - show **the same blanking level after every pulse** (a bright pole or edge in the picture also
     lines up from line to line, but what follows it varies).
5. **Polarity.** If both ends qualify, the end is chosen by line coverage, steadier blanking and how
   close blanking sits to 30% of the way from tip to white. A transmitter already being followed keeps
   its end.
6. **Duplicates.** A signal found in two sub-bands (the same sync tip, or within 8 MHz) is taken from
   the one it is nearest the middle of.
7. **Reported.**
   - The **line rate**: 15.625 kHz is 625 lines (PAL/SECAM); 15.734 kHz is 525 lines (NTSC).
   - The **mean frequency**: the demodulated samples' mean, over samples at the signal's own power
     (another signal raises it). It is averaged over the looks it is followed for.
   - The **deviation**: 3.4 × the sync depth (tip to blanking). In both standards white is that far
     from the sync tip, so the figure does not depend on the picture.
8. **Its band** runs from the sync tip to white, ±7 MHz: about the occupied band by Carson's rule,
   including the sound subcarrier. **Every narrow burst and steady carrier inside it is left unmarked**,
   whether the video's carrier made it or not: anything else there is under the video signal. The band
   is held 2 s after the last time the signal was found, so a missed look does not let its parts
   through. Without this rule, its carrier's dwell on the sync and blanking levels, flat parts of the
   picture and its sound subcarrier make about 150 "narrowband" marks a second.

The hover reads, e.g., *analog video, FM (NTSC, 525 lines) · ≈5806 MHz, FPV R5 5806 or A4 5805 ·
deviation 8 MHz p-p*. Channel names (`web/src/fpv.ts`) are the analog FPV channels within 3 MHz: a
guide, not a match (below).

### Measured

**Unit tests** (`tests/test_core.cpp`, `test_video`) are 0.25 s each: the warm-up look and one window's.
Every case must be found at every look, with the right line rate, within 1.5 MHz of the true mean, and
with the deviation within 15% where checked. Nothing else may be reported.

| Case | Frequency | Deviation (true) |
|---|---|---|
| PAL 40 MS/s, sync low, 15 dB | −0.9 MHz | 8.1 (8) |
| NTSC 40 MS/s, sync high, 12 dB | +0.6 MHz | 6.1 (6) |
| NTSC / PAL 20 MS/s, either end, 12-15 dB | ≤ 1.2 MHz | 8.2 (8), 10.2 (10) |
| PAL with a white vertical bar 5 µs wide (a pole), sync high and sync low | ≤ 0.8 MHz | 8.1-8.2 (8): the right end taken |
| NTSC over the adapter's DC at 0.6 and 1.0 of the carrier | −0.8 MHz | 8.2 (8) |
| NTSC pre-emphasised (one-pole shelf, −11 dB under 0.3 MHz, +3 dB above) | −0.2 MHz | 3.5-3.9: the low frequencies' deviation |
| PAL on R5 beside busy Wi-Fi on ch 157 (~70% airtime), as strong and 6 dB stronger | ≤ 0.9 MHz | 8.1-8.2 (8) |
| PAL at 5810 and NTSC 6 dB weaker at 5790 MHz, one span | both, ≤ 0.9 MHz | 8.2 (8) |

(CNR is carrier to noise in 20 MHz.) Not marked: a strong carrier keyed at 15.625 kHz with sync's duty
cycle over a weaker steady one, noise, Wi-Fi, a microwave oven, Bluetooth, and a display's clock
harmonic switched at a line rate (`test_signals`, `test_video`).

**Simulated recordings from real footage** (below), with the finder over each (`test_core --wifi`):
every look is found at 40 MS/s from NTSC, PAL with sync high, and NTSC at 20 MS/s through the
adapter's filter, all at the right line rate and frequency.

**Sensitivity**, from the same footage as NTSC (looks found out of 4 / narrowband marks let through,
0.65 s each):

| | 3 dB | 4 dB | 5 dB | 6 dB | 7 dB | 9 dB |
|---|---|---|---|---|---|---|
| 40 MS/s, 4 MHz deviation | 0/63 | 0/135 | 2/0 | 3/0 | 4/0 | 4/0 |
| 40 MS/s, 8 MHz | 1/17 | 2/32 | 3/0 | 3/0 | 3/0 | 4/0 |
| 40 MS/s, 12 MHz | 2/24 | 3/0 | 3/0 | 4/0 | 4/0 | 4/0 |
| 20 MS/s tuned to it, 8 MHz | 3/0 | 4/0 | 4/0 | — | 4/0 | — |

The picture at 40 MS/s, demodulated by `fpv_sim.py check`, has a luma correlation with the source of
0.95 at 10 dB and 0.68 at 7 dB, where it is full of FM "sparklies" and torn lines. At 5 dB the reference
decoder cannot hold a frame. So the mark appears about where the picture breaks up, a few dB before it
is gone. Below that, the old flood of narrowband marks returns, smaller than before.

**Cost:** each look takes about 4.3 ms at 40 MS/s (three sub-bands) and 1 ms at 20 MS/s. That is about
2.5% of a core at 40 MS/s.

**Seen on the way, not changed:** with an FPV transmitter as strong as busy Wi-Fi on the next channel,
the video's sidebands at the channel's edge cost the Wi-Fi frame finder most frames (3 decoded in
0.25 s, against 1,484 with the Wi-Fi 6 dB stronger). That fits how analog FPV disturbs Wi-Fi nearby.

### Limits

- **Frequency.** It is the mean over the looks, and each look covers a band of the picture (~31 lines).
  The looks are spread over the field and averaged, but a few seconds of a high-contrast scene can still
  shift it by a MHz or so. Channel names are a guide.
- **Deviation and pre-emphasis.** The deviation assumes linear FM and standard levels. With
  pre-emphasis it reads the low frequencies' deviation, a fraction of the nominal. Real transmitters'
  deviation, pre-emphasis and sound subcarrier (6.0 or 6.5 MHz) are assumptions until measured.
- **Polarity.** A bright vertical bar in the picture can still be taken for sync on the first look if
  it lines up on more lines, with steadier levels after it, than the sync does. That is unlikely but
  possible, and the end chosen first is then kept while the signal is followed.
- **Missed signals.** Two transmitters closer than ~20 MHz: the stronger takes the sub-band. A signal
  overlapping strong Wi-Fi on the same frequencies can be missed while the Wi-Fi is on; the 2 s hold
  bridges short misses.
- **Hidden marks.** Narrow bursts and steady carriers inside a found video signal's band are not marked
  (Bluetooth beside a 2.4 GHz video sender, a display harmonic under an FPV channel).
- **Spans.** Continuous 20/40 MS/s spans only (the Mark signals rule), so in practice the 8812BU on
  USB 3.
- **No picture.** The picture is not decoded (below).

## Testing without a drone: `scripts/fpv_sim.py`

`make` turns a video into a SigMF recording like the app's own. The video is any file ffmpeg reads, or
ffmpeg's `testsrc2` by default. The output is ci16 with 12-bit samples << 4, the centre in
`core:frequency` and the truth in `fpv_sim:truth`. It models:

- **The composite:** NTSC 525/59.94 or PAL 625/50 colour (U/V on the subcarrier, PAL's V switch, the
  burst), a continuous line grid, the vertical interval (equalising and serrated broad pulses,
  interlaced: the second field starts mid-line), filtered to 4.2 / 5 MHz.
- **The transmitter:** a 6.5 MHz FM sound subcarrier, then FM with a given deviation (sync tip to
  white) and sync polarity. Its mean over the whole file sits at the channel frequency, as a VTX's PLL
  holds it.
- **The receiver:** noise at a given CNR in 20 MHz, and at 20 MS/s the adapter's ~18.4 MHz filter
  (an FIR stand-in for its real shape) before decimation.

`check` demodulates a recording back to a black-and-white frame. It finds the sync polarity and
standard, the vertical interval and both fields, writes `PREFIX.check.png` (source | decoded) and
prints the luma correlation with the source. This verifies the generator itself. It is also a minimal
offline reference decoder that should work on a real recording made with **● Rec** (without the
comparison), provided the sync tip is inside the adapter's passband. At 20 MS/s that means the
transmitter must be within ~5 MHz of the centre.

```sh
scripts/fpv_sim.py make --video clip.mp4 --std ntsc --freq 5806 --centre 5800 --seconds 0.7 --out /tmp/fpv
scripts/fpv_sim.py check /tmp/fpv.sigmf-data        # NTSC (15.736 kHz lines) ... correlation 0.985
build/test_core --wifi /tmp/fpv.sigmf-data          # other signals: A 5806 MHz line 15.73 kHz dev 8 MHz
```

Picture round trips with 3 s of *Big Buck Bunny* (CC-BY Blender Foundation; used locally, not in the
repository), against the source's luma:

| Variant | Correlation |
|---|---|
| NTSC 40 MS/s, 20 dB CNR | 0.985 |
| NTSC 20 MS/s through the 18.4 MHz filter, 15 dB | 0.978 |
| PAL 20 MS/s, 5 MHz off centre, sync high | 0.977 |
| PAL 40 MS/s, sync high, 10 MHz deviation, 15 dB | 0.969 |
| NTSC 40 MS/s, 10 dB | 0.949 |
| NTSC 40 MS/s, 7 dB (sparklies, torn lines) | 0.68 |

Not modelled: pre-emphasis (the unit tests have it), the adapter's I/Q imbalance and real filter
shape, multipath, and transmitter non-linearity.

## Watching it today, without code: SDRangel

The SoapySDR module already streams 40 MS/s at 40 MHz channel width on an 8812BU over USB 3 (bandwidth
above 25 MHz, the default at that rate; `src/soapy/SoapyWsdr.cpp` in wifi-sdr). Untested route:
SDRangel with `SOAPY_SDR_PLUGIN_PATH` pointing at the module. Use a SoapySDR input (`driver=wsdr`) at
40 MS/s tuned near the channel, and the ATV demodulator in FM mode with 625 or 525 lines. It shows
the luminance only: black and white.

## A picture decoder in the app (plan)

1. **Backend:** path A (or A+B combined; first compensate B's lead) mixed to the video's centre, then
   band-limited (as the mark's sub-bands are), FM-demodulated, low-passed to the video band, and
   resampled to a line-locked grid (13.5 MS/s, or 4 × the colour subcarrier).
2. **Sync:** a horizontal PLL on the sync leading edges, the vertical sync from the broad pulses,
   then fields woven into frames. `fpv_sim.py check` is a minimal offline version of this.
3. **To the UI:** about 720 × 480/576 at 30/25 frames a second, sent as compressed frames (~1 MB/s) or
   raw 8-bit luma (~10 MB/s) over the existing WebSocket, drawn on a canvas.
4. **Colour:** a burst-locked subcarrier PLL, then U/V demodulation (NTSC: phase from the burst; PAL:
   the V switch and line averaging), and a comb filter to separate luma from chroma. Needs the 40 MHz
   mode for the chroma sidebands.
5. **Sound:** the 6.0/6.5 MHz subcarrier through a second discriminator.
6. **De-emphasis:** add it if real transmitters turn out to use pre-emphasis.

## Next steps

1. **A real transmitter.** Record an analog VTX with **● Rec** at 40 MS/s on an 8812BU over USB 3, on a
   few channels including one above 5885 MHz (R8 or E6-E8). Then run `test_core --wifi` and
   `fpv_sim.py check` on the recordings.
2. **Measure real transmitters:** their deviation, sync polarity, pre-emphasis and sound subcarrier;
   the front end's response above 5885 MHz; the mark's frequency against the VTX's set channel.
3. **Cross-check** with SDRangel's ATV demodulator.
4. **The decoder in the app,** black and white first.
