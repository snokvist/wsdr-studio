// SPDX-License-Identifier: GPL-2.0-only
// Formatting, the tuning grid, adapter capabilities and the viewer's preferences.
import type { Range } from './bands';
import type { Channel } from './channels';
import type { Device, Rate } from './protocol';

export const FFTS = [1024, 2048, 4096, 8192, 16384];
export const RATES: Rate[] = [5, 4, 3, 2, 1].map((r) => ({ rate_sel: r, fs: 80e6 / (1 << r) }));
// What an adapter offers before Start (after Start the backend's caps say it).
export const ratesFor = (d: Device | undefined, offsetTuning: boolean): Rate[] =>
  d
    ? RATES.filter((r) => r.fs <= d.max_msps * 1e6 || r.fs <= (d.gapped_msps ?? 0) * 1e6).map((r) => {
        const gapped = r.fs > d.max_msps * 1e6;
        // as the backend: with offset tuning the 8812CU's 10/20 MHz gapped views come from a 40 MHz capture
        return { ...r, gapped, hw_fs: gapped && offsetTuning && d.chip.includes('8822C') && r.fs <= 20e6 ? 40e6 : r.fs };
      })
    : RATES;
// Share of the time a gapped span samples: one 8192-sample bank, then ~1.48 ms shipping it over USB 2.
export const gappedDuty = (fs: number) => 8192 / fs / (8192 / fs + 1.48e-3);
export const mhz = (hz: number, d = 1) => (hz / 1e6).toFixed(d);
export const spanLabel = (fs: number) => `${fs / 1e6} MHz`;
export const devKey = (d: Device) => `${d.bus}-${d.port}`;
export const devLabel = (d: Device) => `${d.model} · ${d.speed_mbps >= 5000 ? 'USB 3' : 'USB 2'} · bus ${d.bus}`;
export const devTitle = (d: Device) =>
  `${d.model} (${d.chip}), bus ${d.bus} port ${d.port}, ${d.speed_mbps} Mbit/s${d.controller ? `, host controller ${d.controller}` : ''}`;
export const bytes = (n: number) => (n >= 1e9 ? `${(n / 1e9).toFixed(n >= 1e11 ? 0 : 2)} GB` : `${(n / 1e6).toFixed(0)} MB`);
export const baseName = (p: string) => p.slice(p.lastIndexOf('/') + 1);
export const chanShort = (c: Channel) => `ch ${c.width === 40 ? c.ch + 2 : c.label}`;
export const fmtDur = (s: number) => (s >= 3600 ? `${(s / 3600).toFixed(1)} h` : s >= 60 ? `${Math.floor(s / 60)} min` : `${Math.max(0, Math.floor(s))} s`);
export const fmtClock = (s: number) => `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
// The radio tunes 2.4 GHz channels 1-13 (2412-2472, 5 MHz apart) and 14 (2484), and 5 GHz in 5 MHz
// steps from 5080 to 6165 MHz. The nearest tunable centre:
// (At a 40 MHz span the radio centres within 2422-2462 / 5090-6155 MHz; the request stays and is marked.)
export const snap = (f: number) => {
  if (f < 3.7e9) {
    if (f > 2478e6) return 2484e6;
    return 2412e6 + Math.min(12, Math.max(0, Math.round((f - 2412e6) / 5e6))) * 5e6;
  }
  return Math.min(6165e6, Math.max(5080e6, Math.round(f / 5e6) * 5e6));
};
export const bandOf = (f: number): '2.4' | '5' => (f < 3.7e9 ? '2.4' : '5');
export const UDEV_CMD = 'sudo cp tools/99-wsdr.rules /etc/udev/rules.d/ && sudo udevadm control --reload';
export const LOCKED = 'End the recording to change tuning or gain';
export const VIEW_COLOUR = ['#4fc3f7', '#c792ea'];
export const ANT_COLOUR: [string, string] = ['#ffab70', '#8fd694']; // Ant 1, Ant 2
export const SLOT_NAME = ['A', 'B'];

// Per-viewer preferences (display only); storage may be unavailable.
const PREFS_KEY = 'wsdr-studio.prefs';
export type Scale = { dbMin: number; dbMax: number; wfFloor: number };
export type Tuning = { freq: number; rate_sel: number; gain_db: number; agc: boolean };
export type Locks = { freq: boolean; span: boolean; gain: boolean };
export type Mode = 'single' | 'panorama' | 'compare' | 'map';
export const isDual = (m: Mode) => m === 'panorama' || m === 'compare'; // both views shown
export type Prefs = {
  smooth: number; wfMode: 'max' | 'avg'; scale: Scale | null; retuned: boolean; fft: number;
  tuning: Tuning | null; tuningB: Tuning | null;  // the last tuning of view A / B
  mode: Mode; locks: Locks; ghost: boolean;       // two adapters: Panorama (adjacent) or Compare (same channel)
  offsetTuning: boolean;                          // off: show what the adapter sees (notch included)
  sidebar: boolean;                               // sidebar shown
  showAvg: boolean; showPeak: boolean;            // traces drawn
  mapRange: Range; mapDwell: number; mapGapped: boolean; // Spectral Map: what it sweeps, ms per step, gapped steps allowed
  wifiMarks: boolean;                             // mark the Wi-Fi frames decoded from the samples
};
export function loadPrefs(): Prefs {
  const d: Prefs = {
    smooth: 0.7, wfMode: 'max', scale: null, retuned: false, fft: 8192, tuning: null, tuningB: null,
    mode: 'single', locks: { freq: true, span: true, gain: false }, ghost: true, offsetTuning: false,
    sidebar: true, showAvg: true, showPeak: true, mapRange: 'u12', mapDwell: 250, mapGapped: false, wifiMarks: false,
  };
  try {
    const p = JSON.parse(localStorage.getItem(PREFS_KEY) ?? '{}');
    // before v3: dual (on/off) and layout (row: side by side, column: stacked)
    if (p.mode === undefined && p.dual !== undefined) p.mode = p.dual ? (p.layout === 'column' ? 'compare' : 'panorama') : 'single';
    delete p.dual;
    delete p.layout;
    delete p.collapsed; // before: folding sidebar sections
    return { ...d, ...p };
  } catch {
    return d;
  }
}
export function savePrefs(p: Prefs) {
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(p));
  } catch {
    /* private window */
  }
}
