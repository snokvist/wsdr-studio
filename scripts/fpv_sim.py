#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""An analog FPV video transmitter, simulated, as test material for the signal finder.

  make:  a video (any file ffmpeg reads; ffmpeg's own test pattern by default) as colour composite
         video (NTSC 525/59.94 or PAL 625/50), FM-modulated as an analog 5.8 GHz VTX sends it, with
         noise, written as a SigMF recording like wsdr-studio's own (ci16, 12-bit samples << 4, 40 or
         20 MS/s; at 20 MS/s through the adapter's ~18.4 MHz filter).
  check: FM-demodulates such a file back to a picture (black and white: sync, the vertical interval,
         both fields), writes it beside the source frame as a PNG and prints how well they match (the
         generator checked end to end; a small decoder of its own, nothing of the app's).

  scripts/fpv_sim.py make --video clip.mp4 --std ntsc --out /tmp/fpv     # /tmp/fpv.sigmf-data/-meta
  build/test_core --wifi /tmp/fpv.sigmf-data                              # "A 5806 MHz line 15.73 kHz"
  scripts/fpv_sim.py check /tmp/fpv.sigmf-data                            # /tmp/fpv.check.png

Needs numpy, ffmpeg (and Pillow for `check`). Writes files only; nothing is transmitted.
"""
import argparse
import json
import subprocess
import sys

import numpy as np

FS = 40e6  # generated at 40 MS/s; 20 MS/s is filtered and decimated from it
# Levels in volts from blanking (NTSC: 140 IRE = 1 V, 7.5 IRE setup), timings from the line's sync edge
STD = {
    'ntsc': dict(lines=525, fh=4.5e6 / 286, fsc=315e6 / 88, sync=-40 / 140, setup=7.5 / 140, white=100 / 140,
                 burst=20 / 140, front=1.5e-6, burst_t=(5.3e-6, 7.8e-6), act0=9.4e-6, eq=2.3e-6, vbi=18, first=21,
                 rows=480, fps=30000 / 1001, video_bw=4.2e6),
    'pal': dict(lines=625, fh=15625.0, fsc=4.43361875e6, sync=-0.3, setup=0.0, white=0.7, burst=0.15,
                front=1.65e-6, burst_t=(5.6e-6, 7.85e-6), act0=10.5e-6, eq=2.35e-6, vbi=15, first=23, rows=576,
                fps=25.0, video_bw=5.0e6),
}
WIDTH = 720


def fconv(x, h):
    """x filtered by h, aligned ('same'), through the FFT"""
    n = 1 << int(np.ceil(np.log2(len(x) + len(h))))
    y = np.fft.irfft(np.fft.rfft(x, n) * np.fft.rfft(h, n), n)
    return y[(len(h) - 1) // 2:(len(h) - 1) // 2 + len(x)]


def lowpass(cutoff, fs, taps):
    m = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / fs * m) * np.hamming(taps)
    return (h / h.sum()).astype(np.float64)


class Frames:
    """The source's frames in turn (RGB float 0..1, rows x WIDTH), at the standard's frame rate; the
    last one held once the video ends."""

    def __init__(self, video, s):
        src = ['-i', video] if video else ['-f', 'lavfi', '-i', f'testsrc2=size={WIDTH}x{s["rows"]}:rate={s["fps"]}']
        vf = f'scale={WIDTH}:{s["rows"]}:flags=area,fps={s["fps"]}'
        self.p = subprocess.Popen(['ffmpeg', '-v', 'error', *src, '-vf', vf, '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'],
                                  stdout=subprocess.PIPE)
        self.n, self.rows, self.cur, self.idx = WIDTH * s['rows'] * 3, s['rows'], None, -1

    def get(self, i):
        while self.idx < i:
            b = self.p.stdout.read(self.n)
            if len(b) < self.n:
                if self.cur is None:
                    sys.exit('fpv_sim: no frames from ffmpeg')
                return self.cur
            self.cur = np.frombuffer(b, np.uint8).reshape(self.rows, WIDTH, 3).astype(np.float32) / 255
            self.idx += 1
        return self.cur

    def close(self):
        self.p.kill()


def yuv(rgb):
    y = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    return y, 0.492 * (rgb[..., 2] - y), 0.877 * (rgb[..., 0] - y)


def composite(t, s, frames):
    """Composite video (volts) at times t (s): a continuous line grid; per field the vertical interval
    (equalising, broad (serrated), equalising half-line pulses; the second field's starts mid-line);
    burst and picture on the active lines."""
    lines, L = s['lines'], 1 / s['fh']
    h, vbi = L / 2, s['vbi']
    hh = np.floor(t / h).astype(np.int64)
    th = t - hh * h
    k = hh % (2 * lines)
    f2 = k >= lines
    fk = k - f2 * lines
    ln = np.floor(t / L).astype(np.int64)
    tau = t - ln * L
    fl = ln % lines - np.where(f2, lines // 2, 0)
    v = np.zeros(t.shape)
    eqz = (fk < vbi // 3) | ((fk >= 2 * vbi // 3) & (fk < vbi))
    broad = (fk >= vbi // 3) & (fk < 2 * vbi // 3)
    norm = fk >= vbi
    v[eqz & (th < s['eq'])] = s['sync']
    v[broad & (th < h - 4.7e-6)] = s['sync']
    v[norm & (tau < 4.7e-6)] = s['sync']
    w = 2 * np.pi * s['fsc'] * t
    vsw = np.where((ln % 2 == 1) & (s['lines'] == 625), -1.0, 1.0)  # PAL: V switched every other line
    bu = norm & (tau >= s['burst_t'][0]) & (tau < s['burst_t'][1])
    burst = -np.sin(w) if s['lines'] == 525 else (-np.sin(w) + vsw * np.cos(w)) / np.sqrt(2)
    v[bu] = s['burst'] * burst[bu]
    a1 = L - s['front']
    act = norm & (tau >= s['act0']) & (tau < a1) & (fl >= s['first']) & (fl < s['first'] + s['rows'] // 2)
    if act.any():
        row = 2 * (fl[act] - s['first']) + f2[act]
        x = (tau[act] - s['act0']) / (a1 - s['act0']) * (WIDTH - 1)
        c0 = np.minimum(np.floor(x).astype(np.int64), WIDTH - 2)
        fr = np.floor(t[act] * s['fh'] / lines).astype(np.int64)
        out = np.empty(row.shape)
        for f in np.unique(fr):
            sel = fr == f
            Y, U, V = yuv(frames.get(int(f)))
            r, c, a = row[sel], c0[sel], (x[sel] - c0[sel])
            pick = lambda img: img[r, c] * (1 - a) + img[r, c + 1] * a
            ws, vs = w[act][sel], vsw[act][sel]
            out[sel] = pick(Y) + pick(U) * np.sin(ws) + vs * pick(V) * np.cos(ws)
        v[act] = s['setup'] + (s['white'] - s['setup']) * out
    return v


def make(a):
    s = STD[a.std]
    frames = Frames(a.video, s)
    fs_out = a.rate * 1e6
    # the composite's mean over the whole file (the transmitter's PLL holds the mean frequency on the
    # channel: then so does this file's), read at 1 MS/s; the frames are read again for the samples
    acc, cnt = 0.0, 0
    for t0 in np.arange(0, a.seconds, 0.25):
        t = np.arange(t0, min(t0 + 0.25, a.seconds), 1e-6)
        acc, cnt = acc + composite(t, s, frames).sum(), cnt + len(t)
    mean = acc / cnt
    frames.close()
    frames = Frames(a.video, s)
    vlp = lowpass(s['video_bw'], FS, 81)
    rlp = lowpass(9.2e6, FS, 63) if a.rate == 20 else None  # the adapter's ~18.4 MHz at 20 MS/s
    dev = a.dev * 1e6 / (s['white'] - s['sync'])  # Hz per volt
    off = (a.freq - a.centre) * 1e6
    rng = np.random.default_rng(1)
    sigma = np.sqrt(10 ** (-a.cnr / 10) * fs_out / 40e6)
    total, chunk = int(a.seconds * FS), 1 << 20
    vtail, rtail, ph0 = np.zeros(len(vlp) - 1), np.zeros(len(rlp) - 1 if rlp is not None else 0, complex), 0.0
    out = open(a.out + '.sigmf-data', 'wb')
    written = 0
    for n0 in range(0, total, chunk):
        t = (n0 + np.arange(min(chunk, total - n0))) / FS
        cv = np.concatenate([vtail, composite(t, s, frames)])
        vtail = cv[-(len(vlp) - 1):]
        v = np.convolve(cv, vlp, 'valid')  # the camera's / transmitter's video bandwidth
        if a.audio:  # a 6.5 MHz sound subcarrier, FM with a 1 kHz tone (50 kHz deviation)
            v = v + 0.1 * np.sin(2 * np.pi * 6.5e6 * t + 50 * np.sin(2 * np.pi * 1e3 * t))
        ph = ph0 + np.cumsum(2 * np.pi * (off + a.sgn * dev * (v - mean)) / FS)
        ph0 = float(np.remainder(ph[-1], 2 * np.pi))
        x = np.exp(1j * ph)
        if rlp is not None:
            xc = np.concatenate([rtail, x])
            rtail = xc[-(len(rlp) - 1):]
            x = np.convolve(xc, rlp, 'valid')[::2]  # (chunks are of even length: every other sample of the stream)
        x = x + sigma * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
        iq = np.empty(2 * len(x), np.int16)
        iq[0::2] = np.clip(np.round(300 * x.real), -2047, 2047).astype(np.int16) * 16
        iq[1::2] = np.clip(np.round(300 * x.imag), -2047, 2047).astype(np.int16) * 16
        out.write(iq.tobytes())
        written += len(x)
    out.close()
    frames.close()
    meta = {'global': {'core:datatype': 'ci16_le', 'core:sample_rate': fs_out, 'core:version': '1.0.0',
                       'core:recorder': 'fpv_sim.py',
                       'core:description': f'simulated analog FPV transmitter: {a.std.upper()} composite, FM, '
                                           f'{a.dev} MHz deviation (sync tip to white), sync at the '
                                           f'{"low" if a.sgn > 0 else "high"} end, CNR {a.cnr} dB in 20 MHz',
                       'wsdr:samples': written,
                       'fpv_sim:truth': {'std': a.std, 'freq_mhz': a.freq, 'dev_mhz': a.dev, 'sync_low': a.sgn > 0,
                                         'cnr_db': a.cnr, 'video': a.video or 'testsrc2'}},
            'captures': [{'core:sample_start': 0, 'core:frequency': a.centre * 1e6}], 'annotations': []}
    with open(a.out + '.sigmf-meta', 'w') as f:
        json.dump(meta, f, indent=2)
    print(f'{a.out}.sigmf-data: {written / fs_out:.2f} s at {a.rate} MS/s, centre {a.centre} MHz, '
          f'{a.std.upper()} at {a.freq} MHz')


def pulses(w, th, fs, lo, hi):
    """Runs of w under th lasting lo..hi seconds: (start, end) sample indices."""
    b = np.concatenate([[False], w < th, [False]]).astype(np.int8)
    d = np.diff(b)
    st, en = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    keep = (en - st >= lo * fs) & (en - st <= hi * fs)
    return st[keep], en[keep]


def check(a):
    from PIL import Image
    base = a.file[:-len('.sigmf-data')] if a.file.endswith('.sigmf-data') else a.file
    meta = json.load(open(base + '.sigmf-meta'))
    fs, truth = meta['global']['core:sample_rate'], meta['global'].get('fpv_sim:truth', {})
    n = int(min(a.seconds, meta['global'].get('wsdr:samples', 1e12) / fs) * fs)
    raw = np.fromfile(base + '.sigmf-data', np.int16, 2 * n).astype(np.float32) / 16
    x = raw[0::2] + 1j * raw[1::2]
    x = x - x.mean()  # (the adapter's own DC)
    # demodulated as it is, and mixed to its power-weighted mean frequency first (an off-centre
    # transmitter wraps otherwise; one whose sync is at the filter's edge can do better without)
    f0 = np.angle(np.sum(x[1:] * np.conj(x[:-1])))
    best = None
    for mix in (0.0, f0):
        y = x * np.exp(-1j * mix * np.arange(len(x))) if mix else x
        d = np.angle(y[1:] * np.conj(y[:-1])) * fs / (2 * np.pi)  # Hz
        sync = fconv(d, lowpass(1.0e6, fs, int(2 * fs / 1e6) | 1))
        # the polarity whose plateaus repeat at a line period: sync tips; then the standard from that period
        for sgn in (1, -1):
            w = sgn * sync
            tip, med = np.quantile(w, 0.02), np.median(w)
            st, en = pulses(w, tip + 0.2 * (med - tip), fs, 3.5e-6, 6.5e-6)
            dd = np.diff((st + en) / 2) / fs
            good = ((dd > 60e-6) & (dd < 68e-6)).sum()
            if good > 10 and (best is None or good > best[0]):
                best = (good, sgn, np.median(dd[(dd > 60e-6) & (dd < 68e-6)]), tip + 0.2 * (med - tip), d, sync)
    if best is None:
        sys.exit('check: no line sync found')
    _, sgn, line, th, d, sync = best
    luma = fconv(d, lowpass(2.5e6, fs, int(8 * fs / 1e6) | 1))  # (under the colour subcarrier)
    std = 'pal' if abs(line - 64e-6) < abs(line - 1 / STD['ntsc']['fh']) else 'ntsc'
    s = STD[std]
    w, wl = sgn * sync, sgn * luma
    st, en = pulses(w, th, fs, 3.5e-6, 6.5e-6)
    bst, ben = pulses(w, th, fs, 20e-6, 32e-6)  # broad pulses: the vertical interval
    tip = np.median(w[((st + en) // 2)])
    blank = np.median(np.concatenate([w[e + int(1e-6 * fs): e + int(3e-6 * fs)] for e in en]))
    white = tip + (s['white'] - s['sync']) / (0 - s['sync']) * (blank - tip)
    # field starts: the first broad pulse of each group, back to the field's first half-line
    L, h = 1 / s['fh'], 0.5 / s['fh']
    starts = [b for i, b in enumerate(bst) if i == 0 or b - bst[i - 1] > 10 * L * fs]
    t0s = [b / fs - (s['vbi'] // 3) * h for b in starts]
    lead = st / fs  # (the sync's leading edge: the line's start)
    img = np.zeros((s['rows'], WIDTH))
    have = np.zeros(s['rows'], bool)
    a1 = L - s['front']
    xs = np.linspace(s['act0'], a1, WIDTH, endpoint=False)
    field_frame = None
    for t0, t1 in zip(t0s, t0s[1:] + [len(d) / fs]):
        near = lead[np.argmin(np.abs(lead - t0))]
        second = abs(((t0 - near) / L) % 1 - 0.5) < 0.25  # a field starting mid-line
        if field_frame is None and second:
            continue  # (a frame from its first field on)
        if field_frame is None:
            field_frame = int(np.floor((t0 + 0.5 * L) * s['fh'] / s['lines']))
        for ts in lead[(lead > t0) & (lead < t1 - L)]:
            fl = int(round((ts - t0) / L + 0.25))
            r = 2 * (fl - s['first']) + int(second)
            if 0 <= r < s['rows']:
                pos = (ts + xs) * fs
                i0 = np.floor(pos).astype(np.int64)
                img[r] = wl[i0] * (1 - (pos - i0)) + wl[i0 + 1] * (pos - i0)
                have[r] = True
        if second:
            break
    if field_frame is None or have.mean() < 0.9:
        sys.exit(f'check: a whole frame not found ({have.mean():.0%} of its lines)')
    y = (img - blank) / (white - blank)
    y = np.clip((y - s['setup'] / s['white']) / (1 - s['setup'] / s['white']), 0, 1)
    src = None
    vid = truth.get('video')
    if vid:
        fr = Frames(None if vid == 'testsrc2' else vid, s)
        src = yuv(fr.get(field_frame))[0]
        fr.close()
    out = base + '.check.png'
    pic = np.hstack([src, y]) if src is not None else y
    Image.fromarray(np.round(255 * pic).astype(np.uint8)).save(out)
    line_khz = 1e-3 / line
    print(f'{std.upper()} ({line_khz:.3f} kHz lines), sync at the {"low" if sgn > 0 else "high"} end, '
          f'deviation {abs(white - tip) / 1e6:.1f} MHz sync tip to white; frame {field_frame}: {out}'
          + (' (source | decoded)' if src is not None else ''))
    if src is not None:
        m = (slice(4, -4), slice(16, -16))  # (the edges: where a line's interpolation meets blanking)
        c = np.corrcoef(src[m].ravel(), y[m].ravel())[0, 1]
        print(f'luma against the source: correlation {c:.3f}, mean abs error {np.abs(src[m] - y[m]).mean():.3f}')
        if c < a.min_corr:
            sys.exit(f'check: correlation under {a.min_corr}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make', help='a video as an FM analog video transmitter, to a SigMF recording')
    m.add_argument('--video', help='any file ffmpeg reads (default: ffmpeg\'s testsrc2 pattern)')
    m.add_argument('--std', choices=STD, default='ntsc')
    m.add_argument('--seconds', type=float, default=1.0)
    m.add_argument('--rate', type=int, choices=(20, 40), default=40, help='MS/s, as the adapter streams them')
    m.add_argument('--centre', type=float, default=5800.0, help='where the adapter is tuned (MHz)')
    m.add_argument('--freq', type=float, default=5806.0, help='the transmitter\'s mean frequency (MHz; R5)')
    m.add_argument('--dev', type=float, default=8.0, help='deviation, sync tip to white (MHz)')
    m.add_argument('--sync-high', dest='sgn', action='store_const', const=-1, default=1,
                   help='sync at the high end of the swing (default: the low end)')
    m.add_argument('--cnr', type=float, default=20.0, help='carrier to noise in 20 MHz (dB)')
    m.add_argument('--no-audio', dest='audio', action='store_false', help='no 6.5 MHz sound subcarrier')
    m.add_argument('--out', required=True, help='output prefix (PREFIX.sigmf-data, PREFIX.sigmf-meta)')
    c = sub.add_parser('check', help='demodulate a recording back to a picture, against its source')
    c.add_argument('file', help='PREFIX.sigmf-data')
    c.add_argument('--seconds', type=float, default=0.12, help='how much of it to read')
    c.add_argument('--min-corr', type=float, default=0.0, help='fail under this correlation with the source')
    a = p.parse_args()
    make(a) if a.cmd == 'make' else check(a)


if __name__ == '__main__':
    main()
