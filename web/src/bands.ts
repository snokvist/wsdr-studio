// SPDX-License-Identifier: GPL-2.0-only
// The channel axis shared by the band ruler and the spectral map: one range at a time (2.4 GHz, all
// of 5 GHz, or a 5 GHz section), each channel equally wide, the other gaps between the UNII groups as
// short breaks. Channels 68-96 (between UNII-2 and UNII-2e) are no Wi-Fi channels but the radio
// receives there, so they get cells too, marked as outside Wi-Fi.
import type { Device } from './protocol';
import { centre } from './channels';
import { snap } from './util';

export type Range = '2.4' | '5' | 'u12' | 'u2e' | 'u3';
// half: a cell's half-width (Hz); out: no Wi-Fi channels (receive only, nothing to join there)
export type Group = { name: string; chans: number[]; half: number; out?: boolean };
// A piece of the axis: frequencies f0..f1 drawn over units u0..u1 (one unit = one cell).
type Piece = { f0: number; f1: number; u0: number; u1: number };
const U1: Group = { name: 'UNII-1', chans: [36, 40, 44, 48], half: 10e6 };
const U2: Group = { name: 'UNII-2', chans: [52, 56, 60, 64], half: 10e6 };
// 68-96 split where the 5 GHz sections meet (5410 MHz), whole in the 5 GHz overview
const OUT: Group = { name: 'Outside Wi-Fi', chans: [68, 72, 76, 80, 84, 88, 92, 96], half: 10e6, out: true };
const OUT_LO: Group = { ...OUT, chans: OUT.chans.slice(0, 4) };
const OUT_HI: Group = { ...OUT, chans: OUT.chans.slice(4) };
const U2E: Group = { name: 'UNII-2e', chans: [100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144], half: 10e6 };
const U3: Group = { name: 'UNII-3', chans: [149, 153, 157, 161, 165], half: 10e6 };
const U59: Group = { name: '5.9 GHz', chans: [169, 173, 177], half: 10e6 };
// edge: the frequencies the range covers (the radio tunes 2.4 GHz and 5080-6165 MHz)
export const RANGES: Record<Range, { label: string; title: string; groups: Group[]; edge: [number, number] }> = {
  '2.4': { label: '2.4 GHz', title: 'Channels 1–13 (2400–2483.5 MHz)', groups: [{ name: '', chans: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13], half: 2.5e6 }], edge: [2400e6, 2490e6] },
  '5': { label: '5 GHz', title: 'All 5 GHz channels', groups: [U1, U2, OUT, U2E, U3, U59], edge: [5080e6, 6165e6] },
  u12: { label: '36–80', title: 'Channels 36–80: UNII-1, UNII-2 and 68–80 (5150–5410 MHz)', groups: [U1, U2, OUT_LO], edge: [5080e6, 5410e6] },
  u2e: { label: '84–144', title: 'Channels 84–144: 84–96 and UNII-2e (5410–5730 MHz)', groups: [OUT_HI, U2E], edge: [5410e6, 5732.5e6] },
  u3: { label: '149–177', title: 'Channels 149–177: UNII-3 and 5.9 GHz (5735–5895 MHz)', groups: [U3, U59], edge: [5732.5e6, 6165e6] },
};
// the section a frequency is in (5 GHz: the one around it, not the whole band)
export const rangeOf = (f: number): Range => (f < 3.7e9 ? '2.4' : f < 5410e6 ? 'u12' : f < 5732.5e6 ? 'u2e' : 'u3');
const BREAK = 0.6; // units for a gap between groups (or a range edge), whatever its MHz

// The axis of a range: groups at one unit per cell, gaps and edges as short breaks.
function axis(range: Range): { pieces: Piece[]; units: number } {
  const pieces: Piece[] = [];
  const { groups, edge } = RANGES[range];
  let u = 0, f = edge[0];
  for (const g of groups) {
    const lo = centre(g.chans[0]) - g.half, hi = centre(g.chans[g.chans.length - 1]) + g.half;
    if (lo > f) {
      const w = Math.min(BREAK, (lo - f) / (2 * g.half));
      pieces.push({ f0: f, f1: lo, u0: u, u1: u + w });
      u += w;
    }
    pieces.push({ f0: lo, f1: hi, u0: u, u1: u + g.chans.length });
    u += g.chans.length;
    f = hi;
  }
  if (edge[1] > f) {
    const w = Math.min(BREAK, (edge[1] - f) / (2 * groups[groups.length - 1].half));
    pieces.push({ f0: f, f1: edge[1], u0: u, u1: u + w });
    u += w;
  }
  return { pieces, units: u };
}
export const AXES = Object.fromEntries((Object.keys(RANGES) as Range[]).map((r) => [r, axis(r)])) as Record<Range, ReturnType<typeof axis>>;
// position of a frequency on a range's axis, 0..1 (clamped to its ends)
export function pos(band: Range, f: number) {
  const { pieces, units } = AXES[band];
  const p = pieces.find((x) => f <= x.f1) ?? pieces[pieces.length - 1];
  const t = Math.min(1, Math.max(0, (f - p.f0) / (p.f1 - p.f0)));
  return Math.min(units, Math.max(0, p.u0 + t * (p.u1 - p.u0))) / units;
}
export function freqAt(band: Range, x: number) {
  const { pieces, units } = AXES[band];
  const u = x * units;
  const p = pieces.find((q) => u <= q.u1) ?? pieces[pieces.length - 1];
  return p.f0 + ((u - p.u0) / (p.u1 - p.u0)) * (p.f1 - p.f0);
}

export type SweepPlan = { centres: number[]; rate_sel: number; fs: number; step: number; gapped: boolean; lo: number; n: number };

// The Spectral Map's steps for a range on this adapter: the widest continuous span it has (gapped 40 MHz
// only when allowed); groups that touch are swept as one stretch, and each stretch gets evenly spaced
// steps a little closer than the span (its edges roll off), the outer ones a span's half inside its ends.
export function sweepPlan(range: Range, dev: Device | undefined, gappedOk: boolean): SweepPlan | null {
  if (!dev) return null;
  const cont = Math.min(40e6, dev.max_msps * 1e6), wide = (dev.gapped_msps ?? 0) * 1e6;
  const fs = gappedOk && wide > cont ? wide : cont; // gapped: 40 MHz steps on an 8812CU, 80 on an 8812BU
  const step = Math.max(5e6, Math.floor((fs * 0.875) / 5e6) * 5e6);
  const stretches: [number, number][] = [];
  for (const g of RANGES[range].groups) {
    const lo = centre(g.chans[0]) - g.half, hi = centre(g.chans[g.chans.length - 1]) + g.half;
    const last = stretches[stretches.length - 1];
    if (last && lo <= last[1] + 5e6) last[1] = Math.max(last[1], hi);
    else stretches.push([lo, hi]);
  }
  // a 40 MHz span centres within 2422-2462 / 5090-6155 MHz, an 80 MHz one within 2442-2462 / 5110-6135 MHz
  const clamp = (c: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, c));
  const tunable = (c: number) =>
    fs < 40e6 ? c : fs >= 80e6 ? (c < 3.7e9 ? clamp(c, 2442e6, 2462e6) : clamp(c, 5110e6, 6135e6)) : c < 3.7e9 ? clamp(c, 2422e6, 2462e6) : clamp(c, 5090e6, 6155e6);
  const centres: number[] = [];
  for (const [lo, hi] of stretches) {
    // the outer steps first, a MHz inside the span's flat part at the stretch's ends (its edge
    // channels heard whole) and snapped towards them, then the others spread evenly between them
    const m = fs >= 20e6 ? 1e6 : 0, a = lo + fs / 2 - m, b = hi - fs / 2 + m;
    let first = snap(a), last = snap(b);
    if (first > a && snap(first - 5e6) < first) first = snap(first - 5e6);
    if (last < b && snap(last + 5e6) > last) last = snap(last + 5e6);
    first = tunable(first);
    last = tunable(last);
    const n = last > first ? Math.ceil((last - first) / step) + 1 : 1;
    for (let i = 0; i < n; i++) {
      const c = n === 1 ? tunable(snap((lo + hi) / 2)) : i === 0 ? first : i === n - 1 ? last : tunable(snap(first + (i * (last - first)) / (n - 1)));
      if (!centres.includes(c)) centres.push(c);
    }
  }
  const lo = stretches[0][0], hi = stretches[stretches.length - 1][1];
  return { centres, rate_sel: Math.round(Math.log2(80e6 / fs)), fs, step, gapped: fs > cont, lo, n: Math.ceil((hi - lo) / 1e6) };
}
export const planId = (p: SweepPlan | null) => (p ? `${p.rate_sel}:${p.centres.join(',')}` : '');
