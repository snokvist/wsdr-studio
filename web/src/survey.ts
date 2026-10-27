// SPDX-License-Identifier: GPL-2.0-only
// The band ruler's survey: per channel, the strongest peak seen over the noise (dB), kept for the
// session.
import { RANGES } from './bands';
import { centre } from './channels';
import type { Row } from './protocol';

// the channels surveyed: every band ruler cell, with the half-width of the slice measured
const SEGS = [
  { chans: RANGES['2.4'].groups.flatMap((g) => g.chans), half: 2.5e6 },
  { chans: RANGES['5'].groups.flatMap((g) => g.chans), half: 9e6 },
];

export type Survey = Map<number, number>;
// Each cell's own slice (2.4 GHz: its 5 MHz, so one AP does not light up nine overlapping channels;
// 5 GHz: its 20 MHz channel). The slice's peak against the noise: the lower quartile of the peaks of
// windows over the central 80 % of the span (the edges roll off, a notch dips), as wide as the slice
// but small enough that there are at least 8 of them; a quartile, not the median, so a signal filling
// half the span is not taken for the noise (it holds while a quarter of the span is quiet). Each
// bin is already a peak over many FFT frames, so a wider slice adds well under 1 dB on noise alone.
// A cell counts once two of the view's surveyed rows in a row (within a second) show the same excess:
// a lone noise spike does not, so the bars do not creep up over a session.
const pending = new WeakMap<Survey, Map<string, { db: number; t: number }>>();
export function survey(s: Survey, r: Row) {
  if (r.bins < 16) return;
  const f0 = r.f0, f1 = r.f0 + r.df * (r.bins - 1);
  const floors = new Map<number, number>(); // window width (bins) -> noise
  const floorFor = (width: number) => {
    let v = floors.get(width);
    if (v !== undefined) return v;
    const a = Math.floor(r.bins * 0.1), end = Math.floor(r.bins * 0.9), peaks: number[] = [];
    const n = Math.max(1, Math.min(width, Math.floor((end - a) / 8)));
    const step = Math.max(n, Math.floor((end - a) / 256)); // at most ~256 windows
    for (let i = a; i + n <= end; i += step) {
      let p = -1e9;
      for (let b = i; b < i + n; b++) p = Math.max(p, r.max[b]);
      peaks.push(p);
    }
    peaks.sort((x, y) => x - y);
    v = peaks.length ? peaks[Math.floor(peaks.length / 4)] : -1e9;
    floors.set(width, v);
    return v;
  };
  let prev = pending.get(s);
  if (!prev) pending.set(s, (prev = new Map()));
  for (const { chans, half } of SEGS)
    for (const c of chans) {
      const fc = centre(c);
      if (fc < f0 - 1e6 || fc > f1 + 1e6) continue; // its centre must be on the axis
      const b0 = Math.max(0, Math.ceil((fc - half - f0) / r.df)), b1 = Math.min(r.bins - 1, Math.floor((fc + half - f0) / r.df));
      if (b1 < b0) continue;
      let peak = -1e9;
      for (let b = b0; b <= b1; b++) peak = Math.max(peak, r.max[b]);
      const floor = floorFor(b1 - b0 + 1);
      if (floor <= -1e9) continue;
      const now = peak - floor, key = `${r.slot}:${c}`, before = prev.get(key);
      prev.set(key, { db: now, t: r.t });
      if (before && r.t - before.t < 1) s.set(c, Math.max(s.get(c) ?? 0, Math.min(now, before.db)));
    }
}

// The ruler shows it as a bar under each cell (neutral: cyan and purple are the views, amber warns),
// stronger with more dB over the noise: 0 at ~3 dB, 1 at 30 dB.
export const level = (db: number) => Math.min(1, Math.max(0, (db - 3) / 27));
