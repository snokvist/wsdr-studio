// SPDX-License-Identifier: GPL-2.0-only
// Spectrum + waterfall renderer on two canvases sharing one frequency axis. Rows arrive straight from
// the socket (no React state); the spectrum redraws once per animation frame, the waterfall scrolls one
// device pixel per row. Stitched (Panorama): rows of several views on one axis, each drawn where its
// frequencies are; the waterfall's lines are made of each view's newest row.
import { channelsIn } from './channels';
import { TURBO } from './colormap';
import type { OtherSignal, Row, WifiFrame } from './protocol';

export type DisplayOpts = {
  dbMin: number; dbMax: number; // spectrum scale (dBFS)
  wfFloor: number;              // the waterfall's colour scale starts here (dBFS; kept in dbMin..dbMax-10)
  wfMode: 'max' | 'avg';
  wfPath: number;
  smooth: number;               // 0 = off .. 0.9 = heavy averaging of the average trace across rows
  marker: number | null;        // requested frequency (Hz), drawn when it is not the band centre
  markerLabel: string;          // e.g. "requested" or "next Start"
  chipNotch: boolean;           // 8822C: shade its DC-rejection notch at the hardware centre (gapped rows)
  notchWide: boolean;           // 8822B at 40 MHz on 2.4 GHz: its own ~+-1 MHz centre dip (shade it)
  colour: string;               // the average trace: the view's colour
  pathColours: [string, string] | null; // both antennas shown: Ant 1, Ant 2 instead
  ghostColour: string;          // the other view's average (Compare): its view's colour
  showAvg: boolean; showPeak: boolean;
  overlay: string | null;       // message over the plot (stopped, starting, ...)
  staleText: string;            // shown when rows stop arriving
  dim: boolean;                 // stopped: the last picture stays, faded
  stitch: Source[] | null;      // Panorama: the views on this axis (index = slot); null: one view
  wifi: boolean;                // mark the Wi-Fi frames found in the samples on the waterfall
};
// The marks' colours by frame format, apart from the waterfall's colour scale (white legacy OFDM,
// magenta 11n, lavender 11ac, pale yellow 802.11b; the switch's caption names each; a dark border
// keeps them visible on any colour)
export const WIFI_COLOUR = { L: '#ffffff', H: '#ff4fd8', V: '#c4a7ff', B: '#fff59d' } as const;
const WIFI_NAME = { L: 'legacy', H: '11n', V: '11ac', B: '11b' } as const;
// other signals share one colour (aquamarine); the hover names their kind
export const OTHER_COLOUR = '#7fffd4';
const OTHER_NAME = { T: 'Bluetooth', N: 'narrowband', M: 'microwave oven', E: 'BLE advertising', C: 'steady carrier' } as const;
// video line rates (kHz: 480p, 1080i/p30, 720p50, 800x600, 720p60, 1024x768, 1080p50, 1280x1024,
// 1080p60, 1440p60, 4K60): a steady carrier with a comb at one of these is most likely a display's pixel
// clock harmonic (PAL/NTSC's ~15.7 kHz is below what the fine spectrum resolves)
const LINE_RATES = [31.469, 33.75, 37.5, 37.879, 45, 48.363, 56.25, 63.981, 67.5, 88.8, 135];
// a mark on the waterfall: a Wi-Fi frame, or another signal in the same shape (rate and length 0)
type Mark = [t: number, mhz: number, bw: number, fmt: WifiFrame[3] | OtherSignal[3], rate: number, len: number, us: number, snr: number,
  ta?: string | null, bssid?: string | null, ssid?: string | null, ch?: number];
const isWifiFmt = (k: Mark[3]): k is WifiFrame[3] => k in WIFI_NAME;
const isWifi = (m: Mark) => isWifiFmt(m[3]);
// A view on a stitched axis.
export type Source = {
  lo: number; hi: number;       // the span it shows (Hz)
  colour: string;
  pathColours: [string, string] | null; // both antennas shown
  marker: number | null; markerLabel: string;
  chipNotch: boolean; notchWide: boolean;
  dim: boolean;                 // stopped: its part fades
  msg: string | null;           // e.g. "B stopped", over its part
};

const PEAK = ['#8d99a8', '#c9a28f']; // peak trace, path A / B
const AXIS_W = 46; // CSS px reserved on the left (dB axis / colour bar + time ticks)
const AXIS_H = 22; // CSS px for the frequency axis under the spectrum
const TOP = 22;    // CSS px headroom above the spectrum (the "dBFS" caption sits there)
const STALE_MS = 1000;
const FRESH_MS = 500; // stitched: a view's newest row older than this (or 3 rows) is not drawn into the waterfall
const SLOT = ['A', 'B'];
const BREAK_PX = 36; // CSS px between the pieces of a broken (stitched) axis
// A piece of the axis: frequencies f0..f1 over plot x0..x1 (device px from the plot's left edge).
type Piece = { f0: number; f1: number; x0: number; x1: number };

export class Display {
  private sp: HTMLCanvasElement;
  private wf: HTMLCanvasElement;
  private spCtx: CanvasRenderingContext2D;
  private wfCtx: CanvasRenderingContext2D;
  // traces by key: the path (one view), or slot * 2 + path (stitched)
  private last: (Row | null)[] = [null, null, null, null];
  private hold: (Float32Array | null)[] = [null, null, null, null];
  private smoothed: (Float32Array | null)[] = [null, null, null, null];
  private slotGen = [-1, -1];  // stitched: each view's run byte
  private slotAt = [0, 0];     // stitched: when each view's newest row arrived
  private keyAt = [0, 0, 0, 0]; // stitched: when each trace's newest row arrived
  private clock = -1;           // stitched: the view setting the waterfall's pace
  private wfRow: (Row | null)[] = [null, null]; // stitched: each view's newest waterfall row
  private wfSpan = '';
  private lastRowAt = 0;
  private lastRowT = 0;       // server time of the previous row (s)
  private rowsPerS = 25;      // measured row rate, for the waterfall's time ticks
  private gutterRate = 25;    // the rate the ticks were last drawn with
  private stopRows = -1;      // rows since the last Stop separator (-1: none); ticks end there
  private stopPending = false;
  private seen = 0;
  private dirty = true;
  private raf = 0;
  // Wi-Fi marks: each drawn row's time and span (newest first; one row per device pixel; null span: a
  // separator line), rows drawn so far, frames newer than the newest row (drawn when it comes) and the
  // frames kept for the hover readout
  private rowT: { t: number; lo: number; hi: number } [] = [];
  private rowNo = 0;
  private wifiPending: Mark[] = [];
  private wifiKept: { f: Mark; row: number }[] = [];
  private wifiNames = new Map<string, string>(); // BSSID -> network name, from the beacons seen here
  private hover: number | null = null; // CSS x
  private hoverWfY = -1;               // CSS y over the waterfall (-1: over the spectrum)
  private sepRows = -1;                // rows since the newest span separator (older rows: another span)
  // Separator labels: drawn on an overlay canvas, not into the waterfall's pixels (a resize rescales
  // those, and would stretch the text). row: device px from the top.
  private seps: { row: number; n: number; label: string }[] = [];
  private wfo: HTMLCanvasElement;
  private lastGen = -1;                // run byte of the newest row
  private genRows = 0;                 // rows of the current run
  private genCount = 0;                // run changes seen
  private frozen: HTMLCanvasElement | null = null; // waterfall before Stop dimmed it
  private ghost: Row | null = null;           // the other view's newest row (dual mode)
  private ghostSm: Float32Array | null = null; // ... its average, smoothed like ours
  private ghostAt = 0;                         // when it last arrived (a stopped view's trace expires)
  onHint: (text: string) => void = () => {};
  private dpr = 1;
  private off: (() => void)[] = [];
  opts: DisplayOpts = {
    dbMin: -110, dbMax: -20, wfFloor: -100, wfMode: 'max', wfPath: 0, smooth: 0.7, marker: null, markerLabel: 'requested', chipNotch: false, notchWide: false, colour: '#4fc3f7', pathColours: null, ghostColour: '#c792ea', showAvg: true, showPeak: true,
    overlay: null, staleText: 'No data from the adapter', dim: false, stitch: null, wifi: false,
  };
  onRetune: (freq: number) => void = () => {};

  constructor(sp: HTMLCanvasElement, wf: HTMLCanvasElement) {
    this.sp = sp;
    this.wf = wf;
    this.spCtx = sp.getContext('2d')!;
    this.wfCtx = wf.getContext('2d')!;
    this.wfo = document.createElement('canvas');
    this.wfo.style.cssText = 'position:absolute;pointer-events:none';
    wf.parentElement?.appendChild(this.wfo);
    const on = <K extends keyof HTMLElementEventMap>(el: HTMLElement, ev: K, fn: (e: HTMLElementEventMap[K]) => void) => {
      el.addEventListener(ev, fn);
      this.off.push(() => el.removeEventListener(ev, fn));
    };
    for (const c of [sp, wf]) {
      on(c, 'mousemove', (e) => {
        this.hover = e.offsetX;
        this.hoverWfY = c === wf ? e.offsetY : -1;
        this.dirty = true;
      });
      on(c, 'mouseleave', () => {
        this.hover = null;
        this.dirty = true;
      });
    }
    // Tune by double-click on the spectrum only: older waterfall rows may belong to another span.
    on(sp, 'dblclick', (e) => {
      const f = this.freqAt(e.offsetX);
      if (f !== null) this.onRetune(f); // the app snaps it to the radio's tuning steps
    });
    on(wf, 'dblclick', () => this.onHint('Double-click the spectrum (top plot) to tune.'));
    let wasStale = false;
    const loop = () => {
      const stale = this.isStale();
      if (stale !== wasStale) {
        wasStale = stale;
        this.dirty = true;
      }
      this.raf = requestAnimationFrame(loop); // first: a failing draw must not end the loop
      if ((window.devicePixelRatio || 1) !== this.dpr && this.wf.clientWidth && this.wf.clientHeight) this.resize(); // moved to a screen of another scale
      if (this.dirty) this.drawSpectrum();
    };
    this.raf = requestAnimationFrame(loop);
  }

  destroy() {
    cancelAnimationFrame(this.raf);
    for (const f of this.off) f();
    this.off = [];
    this.wfo.remove();
  }

  private isStale() {
    return this.seen > 0 && performance.now() - this.lastRowAt > STALE_MS;
  }

  // Layout change (window resize, a strip appearing): keep the waterfall's history, rescaled to the
  // new width and anchored at the top (newest rows).
  resize() {
    const wf = this.wf;
    // hidden (Panorama and Map hide a view's own plots): keep the picture as it is until shown again
    if (!wf.clientWidth || !wf.clientHeight) return;
    const oldDpr = this.dpr;
    const oldFrozen = this.frozen;
    this.dpr = window.devicePixelRatio || 1;
    let keep: HTMLCanvasElement | null = null;
    const w = Math.max(1, Math.round(wf.clientWidth * this.dpr)), h = Math.max(1, Math.round(wf.clientHeight * this.dpr));
    if (wf.width === w && wf.height === h && this.dpr === oldDpr) {
      // only the spectrum changed size: the waterfall and its history stay
      const sw = Math.max(1, Math.round(this.sp.clientWidth * this.dpr)), sh = Math.max(1, Math.round(this.sp.clientHeight * this.dpr));
      if (this.sp.width !== sw || this.sp.height !== sh) {
        this.sp.width = sw;
        this.sp.height = sh;
      }
      this.drawSeps();
      this.dirty = true;
      return;
    }
    if (wf.width > 1 && wf.height > 1) {
      keep = document.createElement('canvas');
      keep.width = wf.width;
      keep.height = wf.height;
      keep.getContext('2d')!.drawImage(wf, 0, 0);
    }
    for (const c of [this.sp, wf]) {
      const cw = Math.max(1, Math.round(c.clientWidth * this.dpr));
      const ch = Math.max(1, Math.round(c.clientHeight * this.dpr));
      if (c.width !== cw || c.height !== ch) {
        c.width = cw;
        c.height = ch;
      }
    }
    this.wfCtx.fillStyle = '#000';
    this.wfCtx.fillRect(0, 0, wf.width, wf.height);
    const oL = Math.round(AXIS_W * oldDpr), L = this.plotLeft();
    if (keep) {
      const rows = Math.min(keep.height, wf.height);
      this.wfCtx.drawImage(keep, oL, 0, keep.width - oL, rows, L, 0, wf.width - L, rows);
    }
    if (oldFrozen && keep) {
      // the undimmed Stop snapshot, rescaled the same way (Start puts it back)
      const f = document.createElement('canvas');
      f.width = wf.width;
      f.height = wf.height;
      const rows = Math.min(oldFrozen.height, wf.height);
      f.getContext('2d')!.drawImage(oldFrozen, oL, 0, oldFrozen.width - oL, rows, L, 0, wf.width - L, rows);
      this.frozen = f;
    }
    this.wfo.width = wf.width;
    this.wfo.height = wf.height;
    this.drawSeps();
    this.drawGutter();
    this.dirty = true;
  }

  private drawSeps() {
    // follows the waterfall (a strip appearing above it moves it without resizing it)
    const o = this.wfo, wf = this.wf;
    o.style.left = `${wf.offsetLeft + wf.clientLeft}px`;
    o.style.top = `${wf.offsetTop + wf.clientTop}px`;
    o.style.width = `${wf.clientWidth}px`;
    o.style.height = `${wf.clientHeight}px`;
    const c = o.getContext('2d')!, d = this.dpr, L = this.plotLeft();
    c.clearRect(0, 0, this.wfo.width, this.wfo.height);
    c.font = `${11 * d}px system-ui, sans-serif`;
    c.textAlign = 'left';
    c.textBaseline = 'top';
    for (const s of this.seps) {
      c.fillStyle = 'rgba(0,0,0,0.65)';
      c.fillRect(L + 4 * d, s.row + 2 * d, c.measureText(s.label).width + 8 * d, 15 * d);
      c.fillStyle = '#ffffff';
      c.fillText(s.label, L + 8 * d, s.row + 4 * d);
    }
  }

  // New stream (Start): forget the traces. The waterfall keeps its history below a separator.
  newStream() {
    if (this.frozen) {
      // undo Stop's dimming: the old rows keep their true colours (they matched the colour bar)
      this.wfCtx.drawImage(this.frozen, 0, 0);
      this.frozen = null;
    }
    this.seen = 0;
    this.lastGen = -1;
    this.genRows = 0;
    this.lastRowT = 0;
    this.last = [null, null, null, null];
    this.hold = [null, null, null, null];
    this.smoothed = [null, null, null, null];
    this.stopPending = !!this.wfSpan;
    this.dirty = true;
  }

  // Stream ended (Stop): keep the picture, faded, for a last look.
  freeze() {
    this.seen = 0; // not "stale": stopped on purpose
    this.frozen = document.createElement('canvas');
    this.frozen.width = this.wf.width;
    this.frozen.height = this.wf.height;
    this.frozen.getContext('2d')!.drawImage(this.wf, 0, 0);
    this.wfCtx.fillStyle = 'rgba(0,0,0,0.45)';
    const L = this.plotLeft();
    this.wfCtx.fillRect(L, 0, this.wf.width - L, this.wf.height);
    this.dirty = true;
  }

  hasPicture() {
    return this.last.some(Boolean);
  }

  // Forget the peak-hold (after a gain/frequency change it would describe the old setting).
  resetPeaks() {
    this.hold = [null, null, null, null];
    this.dirty = true;
  }

  // Drop one path's traces (e.g. after switching A+B -> A).
  dropPath(p: number) {
    this.last[p] = null;
    this.hold[p] = null;
    this.smoothed[p] = null;
    this.dirty = true;
  }

  setOpts(o: Partial<DisplayOpts>) {
    const rescale =
      o.dbMin !== undefined || o.dbMax !== undefined || o.wfFloor !== undefined || (o.dim !== undefined && o.dim !== this.opts.dim);
    this.opts = { ...this.opts, ...o };
    if (rescale) this.drawGutter();
    this.dirty = true;
  }

  rowsSeen() {
    return this.seen;
  }

  // Rows of the current stream run, and how many run changes have been seen (for Auto: refit on
  // rows of the NEW setting only).
  runRows() {
    return this.genRows;
  }
  runCount() {
    return this.genCount;
  }

  // Scales fitted to the signal: the spectrum from a little under the average noise floor to a little
  // over the strongest peak; the waterfall floor just above that noise floor.
  suggestRange(): { dbMin: number; dbMax: number; wfFloor: number } | null {
    const r = this.first();
    if (!r) return null;
    const pct = (a: Float32Array, q: number) => Float32Array.from(a).sort()[Math.floor(a.length * q)];
    const floor = pct(r.avg, 0.1);
    // peak-hold (the app clears it when a setting changes, so it covers only the current setting)
    let peak = -200;
    for (const h of this.hold) if (h) for (let i = 0; i < h.length; i++) peak = Math.max(peak, h[i]);
    const dbMin = Math.floor((floor - 8) / 5) * 5;
    const dbMax = Math.max(dbMin + 30, Math.ceil((peak + 5) / 5) * 5);
    // Waterfall floor just above the average noise floor: rows with no frame at all (common: 40 ms
    // windows without traffic) then show dark blue, not black; frames stand out above them.
    // peak mode: the noise-only peak over a row sits ~7-8 dB above the average floor
    const wfFloor = Math.min(dbMax - 10, Math.max(dbMin, Math.round(floor + (this.opts.wfMode === 'max' ? 6 : 1))));
    return { dbMin, dbMax, wfFloor };
  }

  // The other view's newest row (dual mode): its average is drawn over this spectrum while both share
  // the frequency axis. null clears it.
  setGhost(r: Row | null) {
    if (!r) {
      this.ghost = null;
      this.ghostSm = null;
    } else {
      const g = this.ghost;
      const same = g && g.bins === r.bins && Math.abs(g.f0 - r.f0) < r.df / 2 && Math.abs(g.df - r.df) < 1e-6;
      if (!same || !this.ghostSm) this.ghostSm = Float32Array.from(r.avg);
      else for (let i = 0; i < r.bins; i++) this.ghostSm[i] = this.ghostSm[i] * this.opts.smooth + r.avg[i] * (1 - this.opts.smooth);
      this.ghost = r;
      this.ghostAt = performance.now();
    }
    this.dirty = true;
  }

  private first() {
    return this.last.find((r) => r) ?? null;
  }

  push(r: Row) {
    this.seen++;
    const now = performance.now();
    this.lastRowAt = now;
    const st = this.opts.stitch;
    const k = st ? r.slot * 2 + r.path : r.path;
    let shown: boolean; // this row makes a waterfall line
    if (st) {
      if (r.gen !== this.slotGen[r.slot]) {
        // that view's new run: its peaks and averages no longer apply
        this.slotGen[r.slot] = r.gen;
        for (const j of [r.slot * 2, r.slot * 2 + 1]) this.hold[j] = this.smoothed[j] = null;
      }
      const fresh = this.fresh();
      // all views quiet (both stopped meanwhile): the next line marks the gap
      if (this.slotAt.every((t) => now - t > STALE_MS) && this.wfSpan) {
        this.stopPending = true;
        this.lastRowT = 0;
      }
      this.slotAt[r.slot] = now;
      this.keyAt[k] = now;
      // a path this view no longer delivers (Both -> one antenna): its trace goes
      const other = r.slot * 2 + (r.path ^ 1);
      if (this.last[other] && now - this.keyAt[other] > 2 * fresh) this.last[other] = this.hold[other] = this.smoothed[other] = null;
      const own = r.path === this.opts.wfPath || !this.last[r.slot * 2 + this.opts.wfPath];
      if (own) this.wfRow[r.slot] = r;
      // the first view still delivering sets the pace (one line per row of it)
      const clock = [0, 1].find((x) => now - this.slotAt[x] < fresh) ?? r.slot;
      if (clock !== this.clock) this.lastRowT = 0; // another view's row times: no rate across the change
      this.clock = clock;
      shown = own && r.slot === clock;
    } else {
      shown = r.path === this.opts.wfPath || !this.last[this.opts.wfPath];
    }
    if (!st && r.gen !== this.lastGen) {
      // a new run (tuning, gain, rate): peaks and averages of the old setting no longer apply
      if (this.lastGen >= 0) this.genCount++;
      this.lastGen = r.gen;
      this.genRows = 0;
      this.hold = [null, null, null, null];
      this.smoothed = [null, null, null, null];
    }
    this.genRows++;
    if (this.lastRowT && r.t > this.lastRowT && shown) {
      const rate = 1 / (r.t - this.lastRowT);
      this.rowsPerS = 0.9 * this.rowsPerS + 0.1 * rate;
      if (Math.abs(this.rowsPerS - this.gutterRate) / this.gutterRate > 0.05) this.drawGutter();
    }
    if (shown) this.lastRowT = r.t;
    const prev = this.last[k];
    if (!prev || prev.bins !== r.bins || prev.f0 !== r.f0 || prev.df !== r.df) {
      this.hold[k] = null;
      this.smoothed[k] = null;
    }
    this.last[k] = r;
    const h = this.hold[k];
    if (!h) this.hold[k] = Float32Array.from(r.max);
    else for (let i = 0; i < r.bins; i++) h[i] = Math.max(h[i] - 0.4, r.max[i]);
    const sm = this.smoothed[k];
    const a = this.opts.smooth;
    if (!sm || a <= 0) this.smoothed[k] = Float32Array.from(r.avg);
    else for (let i = 0; i < r.bins; i++) sm[i] = a * sm[i] + (1 - a) * r.avg[i];
    if (shown) this.drawWaterfallRow(r);
    this.dirty = true;
  }

  // stitched: how old a view's newest row may be and still count as current
  private fresh() {
    return Math.max(FRESH_MS, 3000 / this.rowsPerS);
  }

  // The frequencies a row covers.
  private static range(r: Row) {
    return { lo: r.f0 - r.df / 2, hi: r.f0 + r.df * (r.bins - 0.5) };
  }

  // The axis: the row's span (one view), or all the views' spans (stitched).
  private span(): { f0: number; f1: number } | null {
    const st = this.opts.stitch?.filter((x) => x.hi > x.lo);
    if (st?.length) return { f0: Math.min(...st.map((x) => x.lo)), f1: Math.max(...st.map((x) => x.hi)) };
    const r = this.first();
    if (!r) return null;
    const { lo, hi } = Display.range(r);
    return { f0: lo, f1: hi };
  }

  // The axis in pieces over a plot w px wide (x from the plot's left edge). One view, or neighbouring
  // views (a gap under half a span is shaded on the way): one piece. Views far apart: a piece each,
  // sized by its MHz, with a break between, so neither is squeezed into a sliver.
  private axis(w: number): Piece[] {
    const s = this.span();
    if (!s) return [];
    const iv = (this.opts.stitch ?? []).filter((x) => x.hi > x.lo).map((x) => [x.lo, x.hi]).sort((a, b) => a[0] - b[0]);
    const parts: number[][] = [];
    for (const [lo, hi] of iv) {
      const last = parts[parts.length - 1];
      if (last && lo - last[1] <= 0.5 * Math.min(hi - lo, last[1] - last[0])) last[1] = Math.max(last[1], hi);
      else parts.push([lo, hi]);
    }
    if (parts.length < 2) return [{ f0: s.f0, f1: s.f1, x0: 0, x1: w }];
    const brk = BREAK_PX * this.dpr, total = parts.reduce((t, [lo, hi]) => t + hi - lo, 0), avail = Math.max(1, w - brk * (parts.length - 1));
    let x = 0;
    return parts.map(([lo, hi]) => {
      const piece = { f0: lo, f1: hi, x0: x, x1: x + (avail * (hi - lo)) / total };
      x = piece.x1 + brk;
      return piece;
    });
  }
  // where a frequency is (clamped into the nearest piece), and what is at x (null: in a break)
  private static xAt(ax: Piece[], f: number) {
    const p = ax.find((q) => f <= q.f1) ?? ax[ax.length - 1];
    return p.x0 + ((Math.min(p.f1, Math.max(p.f0, f)) - p.f0) / (p.f1 - p.f0)) * (p.x1 - p.x0);
  }
  private static fAt(ax: Piece[], x: number) {
    const p = ax.find((q) => x >= q.x0 && x < q.x1);
    if (!p) return null;
    const fx = (p.f1 - p.f0) / (p.x1 - p.x0); // Hz per px
    return { f: p.f0 + (x - p.x0) * fx, fx };
  }
  private static onAxis(ax: Piece[], f: number) {
    return ax.some((q) => f >= q.f0 && f <= q.f1);
  }

  private plotLeft() {
    return Math.round(AXIS_W * this.dpr); // whole pixels: image data is put at whole columns
  }

  private freqAt(cssX: number): number | null {
    const w = this.sp.width - this.plotLeft();
    return Display.fAt(this.axis(w), cssX * this.dpr - this.plotLeft())?.f ?? null;
  }

  private wfLo() {
    return Math.min(this.opts.dbMax - 10, Math.max(this.opts.dbMin, this.opts.wfFloor));
  }

  // The floor maps to a dark blue (index 30), not black: rows without any frame stay visible as
  // "quiet", and only values well under the floor fade to black.
  private colour(db: number) {
    const lo = this.wfLo(), hi = this.opts.dbMax;
    return TURBO[Math.max(0, Math.min(255, Math.round(30 + ((db - lo) * 225) / Math.max(1, hi - lo))))];
  }

  // Left gutter of the waterfall: colour bar (scale) and time ticks.
  private drawGutter() {
    const c = this.wfCtx, d = this.dpr, H = this.wf.height, L = this.plotLeft();
    c.fillStyle = '#0b0e13';
    c.fillRect(0, 0, L, H);
    const lo = this.wfLo(), hi = this.opts.dbMax;
    const barW = Math.round(6 * d), barH = Math.min(H - 20 * d, 160 * d), y0 = 6 * d;
    if (barH > 10) {
      const img = c.createImageData(barW, Math.round(barH));
      const px = new Uint32Array(img.data.buffer);
      for (let y = 0; y < img.height; y++) {
        const v = TURBO[Math.round(255 - (225 * y) / Math.max(1, img.height - 1))]; // as colour(): floor = 30
        for (let x = 0; x < barW; x++) px[y * barW + x] = v;
      }
      c.putImageData(img, Math.round(4 * d), Math.round(y0));
    }
    c.fillStyle = '#7d8896';
    c.font = `${10 * d}px system-ui, sans-serif`;
    c.textAlign = 'left';
    c.textBaseline = 'top';
    c.fillText(`${Math.round(hi)}`, 13 * d, y0);
    c.textBaseline = 'bottom';
    c.fillText(`${Math.round(lo)}`, 13 * d, y0 + barH - 10 * d);
    c.fillText('floor', 13 * d, y0 + barH);
    // time ticks: one row = one device pixel, at the measured row rate
    const pxPerS = this.rowsPerS;
    this.gutterRate = pxPerS;
    const step = pxPerS * 10 > 60 * d ? 10 : 30;
    c.textAlign = 'right';
    c.textBaseline = 'middle';
    // no ticks across a Stop gap, nor while stopped (the picture's age keeps growing)
    const end = this.opts.dim ? 0 : this.stopRows >= 0 ? Math.min(H, this.stopRows) : H;
    for (let s = step; s * pxPerS < end; s += step) {
      const y = s * pxPerS;
      if (y < y0 + barH + 12 * d) continue;
      c.fillRect(L - 5 * d, y, 4 * d, 1);
      c.fillText(`−${s} s`, L - 7 * d, y);
    }
  }

  private drawWaterfallRow(r: Row) {
    const c = this.wfCtx, W = this.wf.width, H = this.wf.height, L = this.plotLeft();
    const w = W - L;
    if (w <= 0) return;
    c.drawImage(this.wf, L, 0, w, H - 1, L, 1, w, H - 1);
    this.rowNo++;
    this.rowT.unshift({ t: r.t, lo: 0, hi: 0 }); // its span is set once it is drawn (a separator keeps none)
    if (this.rowT.length > H) this.rowT.length = H;
    if (this.sepRows >= 0) this.sepRows++;
    if (this.seps.length) {
      for (const s of this.seps) s.row++;
      this.seps = this.seps.filter((s) => s.row < H);
      this.drawSeps();
    }
    if (this.stopRows >= 0) {
      this.stopRows++;
      if (this.stopRows <= H) this.drawGutter();
    }
    const label = (x: Row) => `${((x.f0 + x.df * (x.bins / 2 - 0.5)) / 1e6).toFixed(1)} MHz · ${+((x.df * x.bins) / 1e6).toFixed(1)} MHz span${x.gapped ? ' · gapped' : ''}`;
    const now = performance.now();
    // stitched: the views delivering now (a stopped view's part stays dark)
    const rows = (this.opts.stitch ? this.wfRow.map((x, i) => (x && now - this.slotAt[i] < this.fresh() ? x : null)) : [r])
      .filter((x): x is Row => !!x)
      .map((x) => ({ r: x, ...Display.range(x) }));
    // stitched: rows sit at their own frequencies, so only a change of the axis moves older rows off it
    const span = this.opts.stitch
      ? this.axis(w).map((q) => `${(q.f0 / 1e6).toFixed(3)}–${(q.f1 / 1e6).toFixed(3)}`).join(', ') + ' MHz'
      : label(r);
    if (this.wfSpan && (span !== this.wfSpan || this.stopPending)) {
      // A retune, span change or new run: a separator. The label describes the OLD rows below it (the
      // new rows above match the axis), so nothing below the line is misread.
      const d = this.dpr;
      const label = this.stopPending
        ? span === this.wfSpan
          ? '▼ Stop (gap)' // same tuning: only the time gap needs marking
          : `▼ before Stop: ${this.wfSpan} (not on this axis)`
        : `▼ before: ${this.wfSpan} (not on this axis)`;
      if (this.stopPending) this.stopRows = 0;
      if (span !== this.wfSpan) this.sepRows = 0; // a Stop at the same span keeps the frequency axis
      this.stopPending = false;
      c.fillStyle = '#ffffff';
      c.fillRect(L, 0, w, Math.max(1, Math.round(d)));
      // quick changes in a row: one label for them, not a stack
      const near = this.seps[0] && this.seps[0].row < 16 * d ? this.seps.shift()! : null;
      const n = (near?.n ?? 0) + 1;
      this.seps.unshift({ row: 0, n, label: n > 1 ? `▼ before: ${n} changes (older rows not on this axis)` : label });
      this.drawSeps();
      this.wfSpan = span;
      this.wifiPending = []; // frames of the old tuning
      return;
    }
    this.wfSpan = span;
    const img = c.createImageData(w, 1);
    const px = new Uint32Array(img.data.buffer);
    const ax = this.opts.stitch ? this.axis(w) : [{ f0: rows[0].lo, f1: rows[0].hi, x0: 0, x1: w }];
    for (let x = 0; x < w; x++) {
      const at = Display.fAt(ax, x);
      if (!at) {
        px[x] = 0xff1a1511; // a break in the axis
        continue;
      }
      const fa = at.f, fx = at.fx, fm = fa + fx / 2;
      // the row covering this pixel (overlapping views: the one whose centre is nearer)
      let best: Row | null = null, bd = Infinity, lo = 0;
      for (const q of rows) {
        const dist = Math.abs(fm - (q.lo + q.hi) / 2);
        if (fm >= q.lo && fm <= q.hi && dist < bd) [best, bd, lo] = [q.r, dist, q.lo];
      }
      if (!best) {
        px[x] = 0xff000000; // not covered
        continue;
      }
      const src = this.opts.wfMode === 'max' ? best.max : best.avg;
      const b0 = Math.max(0, Math.floor((fa - lo) / best.df)), b1 = Math.min(best.bins, Math.max(b0 + 1, Math.floor((fa + fx - lo) / best.df)));
      let v = -1e9;
      for (let b = b0; b < b1; b++) v = Math.max(v, src[b]);
      px[x] = this.colour(v);
    }
    c.putImageData(img, L, 0);
    if (!this.opts.stitch) {
      this.rowT[0].lo = rows[0].lo;
      this.rowT[0].hi = rows[0].hi;
      const due = this.wifiPending.filter((f) => f[0] <= r.t);
      this.wifiPending = this.wifiPending.filter((f) => f[0] > r.t);
      for (const f of due) this.wifiMark(f, 0);
    }
  }

  // Wi-Fi frames found in this view's samples: each on the row whose time covers its start (frames come
  // a little after their rows, so most land a few rows down)
  addWifi(frames: WifiFrame[], other: OtherSignal[] = []) {
    if (!this.opts.wifi || this.opts.stitch || this.opts.dim) return; // (stopped: the picture is kept as it was)
    // (a BLE advertiser's address and name ride where a Wi-Fi frame's sender and network name do)
    const marks: Mark[] = [
      ...frames,
      // (a steady carrier's comb rides in the rate slot)
      ...other.map((o): Mark => [o[0], o[1], o[2], o[3], o[8] ?? 0, 0, o[4], 0, o[5] ? `${o[5]}${o[6] ? ' (random)' : ''}` : null, null, o[7] ?? null]),
    ];
    for (const f of marks) {
      if (f[9] && f[10] != null && (f[10] || !this.wifiNames.has(f[9]))) {
        if (!this.wifiNames.has(f[9]) && this.wifiNames.size >= 300) this.wifiNames.delete(this.wifiNames.keys().next().value!);
        this.wifiNames.set(f[9], f[10] || '(hidden)');
      }
      if (!this.rowT.length || f[0] > this.rowT[0].t) {
        this.wifiPending.push(f);
        continue;
      }
      let j = 0; // rowT[j].t >= t > rowT[j + 1].t
      while (j + 1 < this.rowT.length && this.rowT[j + 1].t >= f[0]) j++;
      if (j + 1 >= this.rowT.length) continue; // older than the picture
      this.wifiMark(f, j);
    }
    if (this.wifiPending.length > 2000) this.wifiPending.splice(0, this.wifiPending.length - 2000);
  }

  // A mark at the centre of the frame's channel, on its row, in that row's own span (OFDM leaves its
  // centre subcarrier empty, so there it hides none of the signal; an 802.11b mark does cover a little);
  // a dark pixel on each side keeps it visible on any waterfall colour. Rows of marks show the
  // channel's activity.
  private wifiMark(f: Mark, y: number) {
    const rg = this.rowT[y];
    if (!rg || rg.hi <= rg.lo) return; // a separator line: no span
    const hz = f[1] * 1e6;
    if (hz <= rg.lo || hz >= rg.hi) return;
    const L = this.plotLeft(), w = this.wf.width - L, d = Math.max(1, Math.round(this.dpr));
    const x = L + Math.round(((hz - rg.lo) / (rg.hi - rg.lo)) * w);
    // 8 px of colour with a dark pixel each side, 2 px tall: a busy channel becomes a solid bar
    const c = this.wfCtx, h = Math.min(2 * d, this.wf.height - y);
    const box = (x0: number, x1: number) => { // clipped to the plot (the gutter does not scroll)
      const a = Math.max(L, x0), b = Math.min(this.wf.width, x1);
      if (b > a) c.fillRect(a, y, b - a, h);
    };
    c.fillStyle = '#0b0e13';
    box(x - 5 * d, x + 5 * d);
    const fmt = f[3];
    c.fillStyle = isWifiFmt(fmt) ? WIFI_COLOUR[fmt] : OTHER_COLOUR;
    box(x - 4 * d, x + 4 * d);
    this.wifiKept.push({ f, row: this.rowNo - y });
    if (this.wifiKept.length > 4000 && this.wifiKept[0].row < this.rowNo - this.wf.height)
      this.wifiKept = this.wifiKept.filter((k) => k.row >= this.rowNo - this.wf.height); // off the picture
  }

  // The Wi-Fi frames and other signals under the pointer: on that row (+-2) and covering that frequency
  private wifiAt(f: number, rowsAgo: number): string | null {
    const row = this.rowNo - rowsAgo;
    const near = this.wifiKept.filter((k) => Math.abs(k.row - row) <= 2 && Math.abs(f - k.f[1] * 1e6) <= (k.f[2] / 2) * 1e6);
    const hit = near.filter((k) => isWifi(k.f)), oth = near.filter((k) => !isWifi(k.f));
    const other = this.otherText(oth.map((k) => k.f));
    if (!hit.length) return other;
    const by: Record<string, number> = {};
    for (const k of hit) {
      const fmt = k.f[3];
      if (isWifiFmt(fmt)) by[WIFI_NAME[fmt]] = (by[WIFI_NAME[fmt]] ?? 0) + 1;
    }
    // (11n/ac frames carry 6 Mb/s in their legacy header whatever their real rate: legacy and 11b rates only)
    const rates = hit.filter((k) => k.f[3] === 'L' || k.f[3] === 'B').map((k) => (k.f[3] === 'B' && k.f[4] === 5 ? 5.5 : k.f[4]));
    const snr = hit.map((k) => k.f[7]).sort((a, b) => a - b);
    const kinds = Object.entries(by).map(([k, n]) => `${k} ${n}`).join(', ');
    const rate = !rates.length ? '' : Math.min(...rates) === Math.max(...rates) ? ` · ${rates[0]} Mb/s` : ` · ${Math.min(...rates)}–${Math.max(...rates)} Mb/s`;
    const chans: Record<string, number> = {};
    for (const k of hit) chans[`${k.f[1].toFixed(0)} MHz${k.f[2] > 20 ? ` (${k.f[2]} MHz)` : ''}`] = (chans[`${k.f[1].toFixed(0)} MHz${k.f[2] > 20 ? ` (${k.f[2]} MHz)` : ''}`] ?? 0) + 1;
    const ch = Object.entries(chans).sort((a, b) => b[1] - a[1]).map(([k]) => k).join(' / ');
    // who sent them (6 Mb/s frames decoded whole): a network's name for its access point, else the address
    const from: Record<string, number> = {};
    for (const k of hit) if (k.f[8]) from[this.wifiNames.get(k.f[8]) ?? k.f[8]] = (from[this.wifiNames.get(k.f[8]) ?? k.f[8]] ?? 0) + 1;
    const who = Object.entries(from).sort((a, b) => b[1] - a[1]).slice(0, 2).map(([k]) => k);
    return `Wi-Fi ${ch}: ${hit.length} frame${hit.length > 1 ? 's' : ''} (${kinds})${rate} · SNR ${snr[snr.length >> 1].toFixed(0)} dB${who.length ? ` · from ${who.join(', ')}` : ''}${other ? ` · ${other}` : ''}`;
  }

  // e.g. "Bluetooth 3, narrowband 1 · 2426 MHz · 366 µs"
  private otherText(ms: Mark[]): string | null {
    if (!ms.length) return null;
    const by: Record<string, number> = {};
    for (const m of ms) by[OTHER_NAME[m[3] as OtherSignal[3]]] = (by[OTHER_NAME[m[3] as OtherSignal[3]]] ?? 0) + 1;
    const kinds = Object.entries(by).map(([k, n]) => (k === OTHER_NAME.M || k === OTHER_NAME.C ? k : `${k} ${n}`)).join(', ');
    const bursts = ms.filter((m) => m[3] !== 'M' && m[3] !== 'C');
    const carrier = ms.find((m) => m[3] === 'C' && m[4] > 0) ?? ms.find((m) => m[3] === 'C');
    const comb = carrier?.[4] ?? 0;
    const video = comb > 0 && LINE_RATES.some((r) => Math.abs(comb - r) < 0.02 * r);
    const combText = comb > 0 ? ` · comb ${comb.toFixed(1)} kHz${video ? ': a video line rate, likely a display\'s clock harmonic (HDMI/DP)' : ''}` : '';
    const us = bursts.map((m) => m[6]).sort((a, b) => a - b);
    // BLE advertisers: a name when they send one, else the address
    const ble = [...new Set(ms.filter((m) => m[3] === 'E').map((m) => (m[10] ? `“${m[10]}” ${m[8]}` : m[8])))].slice(0, 2);
    const mhz = carrier ? carrier[1].toFixed(3) : ms.map((m) => m[1]).sort((a, b) => a - b)[ms.length >> 1].toFixed(0);
    return `${kinds} · ${mhz} MHz${us.length ? ` · ${us[us.length >> 1].toFixed(0)} µs` : ''}${ble.length ? ` · ${ble.join(', ')}` : ''}${combText}`;
  }

  private drawSpectrum() {
    this.dirty = false;
    const c = this.spCtx, W = this.sp.width, H = this.sp.height, d = this.dpr, L = this.plotLeft();
    const B = AXIS_H * d, T = TOP * d;
    const ph = H - B;
    c.fillStyle = '#0b0e13';
    c.fillRect(0, 0, W, H);
    const { dbMin, dbMax } = this.opts;
    const yOf = (db: number) => T + (ph - T) * (1 - (db - dbMin) / (dbMax - dbMin));
    c.font = `${11 * d}px system-ui, sans-serif`;
    c.textBaseline = 'middle';
    // dB grid + axis caption
    const step = dbMax - dbMin > 60 ? 10 : 5;
    c.strokeStyle = '#1f2630';
    c.fillStyle = '#7d8896';
    c.lineWidth = 1;
    c.textAlign = 'right';
    for (let db = Math.ceil(dbMin / step) * step; db <= dbMax; db += step) {
      const y = Math.round(yOf(db)) + 0.5;
      c.beginPath();
      c.moveTo(L, y);
      c.lineTo(W, y);
      c.stroke();
      c.fillText(`${db}`, L - 6 * d, y);
    }
    c.textAlign = 'left';
    c.textBaseline = 'top';
    c.fillText('dBFS', 4 * d, 3 * d);
    c.textBaseline = 'middle';
    const s = this.span();
    const stale = this.isStale();
    const w = W - L;
    if (s && w > 0) {
      const ax = this.axis(w);
      const xOf = (f: number) => L + Display.xAt(ax, f);
      for (const q of ax) {
        // frequency grid + labels: one every ~70 px at least (1-2-5 steps), so a wide axis stays readable
        const pxPerMHz = (q.x1 - q.x0) / ((q.f1 - q.f0) / 1e6);
        const fstep = [0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000].find((x) => x * pxPerMHz >= 70 * d) ?? 1000;
        c.textAlign = 'center';
        c.textBaseline = 'middle';
        for (let f = Math.ceil(q.f0 / 1e6 / fstep) * fstep; f <= q.f1 / 1e6; f += fstep) {
          const x = Math.round(xOf(f * 1e6)) + 0.5;
          c.strokeStyle = '#1f2630';
          c.beginPath();
          c.moveTo(x, T);
          c.lineTo(x, ph);
          c.stroke();
          c.fillStyle = '#7d8896';
          // a label that would be cut at the plot's edges is left out
          const text = f.toFixed(fstep < 1 ? 1 : 0), half = c.measureText(text).width / 2;
          if (x - half >= L - 2 * d && x + half <= W) c.fillText(text, x, ph + B / 2);
        }
        // channel markers (the 20 MHz channels, 68-96 included), while they are at least ~30 px apart
        c.fillStyle = '#5c6b7a';
        c.textBaseline = 'top';
        for (const ch of 20 * pxPerMHz >= 30 * d ? channelsIn(q.f0, q.f1) : []) {
          const x = Math.round(xOf(ch.centre)) + 0.5;
          c.strokeStyle = '#2b3a4a';
          c.setLineDash([3 * d, 3 * d]);
          c.beginPath();
          c.moveTo(x, T);
          c.lineTo(x, ph);
          c.stroke();
          c.setLineDash([]);
          const half = c.measureText(`ch ${ch.label}`).width / 2;
          if (x - half >= L && x + half <= W) c.fillText(`ch ${ch.label}`, x, T + 2 * d);
        }
      }
      // breaks between the pieces: how far apart they are
      for (let i = 1; i < ax.length; i++) {
        const x0 = L + ax[i - 1].x1, x1 = L + ax[i].x0;
        c.fillStyle = '#141a22';
        c.fillRect(x0, T, x1 - x0, ph - T);
        c.fillStyle = '#8e99a7';
        c.textAlign = 'center';
        c.textBaseline = 'middle';
        c.font = `${11 * d}px system-ui, sans-serif`;
        const mid = (T + ph) / 2;
        c.fillText('≈', (x0 + x1) / 2, mid - 14 * d);
        c.fillText(`${Math.round((ax[i].f0 - ax[i - 1].f1) / 1e6)}`, (x0 + x1) / 2, mid);
        c.fillText('MHz', (x0 + x1) / 2, mid + 14 * d);
      }
      c.textBaseline = 'middle';
      const st = this.opts.stitch;
      // the views on this axis: one (this display's rows), or each stitched view with its own rows
      const views: { slot: number; src: Source; keys: number[]; lo: number; hi: number }[] = st
        ? st.flatMap((src, slot) => (src.hi > src.lo ? [{ slot, src, keys: [slot * 2, slot * 2 + 1], lo: src.lo, hi: src.hi }] : []))
        : [{
            slot: 0, keys: [0, 1], lo: s.f0, hi: s.f1,
            src: { lo: s.f0, hi: s.f1, colour: this.opts.colour, pathColours: this.opts.pathColours, marker: this.opts.marker, markerLabel: this.opts.markerLabel, chipNotch: this.opts.chipNotch, notchWide: this.opts.notchWide, dim: this.opts.dim, msg: null },
          }];
      // stitched: each pixel belongs to the view whose centre is nearer (as in the waterfall), so
      // overlapping or nested spans split the same way in traces, waterfall and hover
      const ordered = [...views].sort((x, y) => x.lo + x.hi - (y.lo + y.hi));
      const overlap = ordered.length > 1 ? Math.min(ordered[0].hi, ordered[1].hi) - Math.max(ordered[0].lo, ordered[1].lo) : 0;
      const owner = new Int8Array(Math.max(0, w)).fill(-1); // w < 0 while the canvas is hidden (Panorama)
      if (st)
        for (let x = 0; x < w; x++) {
          const at = Display.fAt(ax, x + 0.5);
          if (!at) continue;
          const fm = at.f;
          // a running view's live picture over a stopped one's frozen one, then the nearer centre
          let bd = Infinity;
          for (const v of views) {
            const dist = (v.src.dim ? 1e12 : 0) + Math.abs(fm - (v.lo + v.hi) / 2);
            if (fm >= v.lo && fm <= v.hi && dist < bd) [owner[x], bd] = [v.slot, dist];
          }
        }
      const nowMs = performance.now();
      for (const v of views) {
        // a running view whose rows stopped coming (stitched): dimmed, and said over its part
        const stalled = !!st && !v.src.dim && this.slotAt[v.slot] > 0 && nowMs - this.slotAt[v.slot] > STALE_MS;
        if (stalled && !v.src.msg) v.src = { ...v.src, msg: `${SLOT[v.slot]}: no data from the adapter` };
        const lr = this.last[v.keys[0]] ?? this.last[v.keys[1]];
        // the rows' own span (the radio's centre), else what the view asked for
        const rg = lr ? Display.range(lr) : { lo: v.lo, hi: v.hi };
        // 8822C: its RX DC rejection leaves a notch at the hardware centre (in the raw samples; ~-6 dB
        // at +-1.5 % of fs); gapped runs only (offset-tuned views keep it out of view). 8822B: 40 MHz on
        // 2.4 GHz. Shade it so it is not read as a missing signal.
        const mid0 = (rg.lo + rg.hi) / 2;
        if (((v.src.chipNotch && lr?.gapped && !lr.offset) || (v.src.notchWide && lr)) && Display.onAxis(ax, mid0)) {
          const hw = v.src.notchWide ? 1e6 : (rg.hi - rg.lo) * 0.015;
          const x0 = xOf(mid0 - hw), x1 = xOf(mid0 + hw);
          if (x1 - x0 >= 3 * d) {
            c.fillStyle = 'rgba(160,170,190,0.10)';
            c.fillRect(x0, T, x1 - x0, ph - T);
            c.font = `${10 * d}px system-ui, sans-serif`;
            c.textAlign = 'center';
            c.textBaseline = 'top';
            const label = v.src.notchWide ? 'centre dip (8812BU, 40 MHz on 2.4 GHz)' : 'chip notch: what the adapter itself sees';
            const lw = c.measureText(label).width;
            c.fillStyle = 'rgba(11,14,19,0.85)';
            c.fillRect((x0 + x1) / 2 - lw / 2 - 4 * d, T + 22 * d, lw + 8 * d, 14 * d);
            c.fillStyle = '#a3aebb';
            c.fillText(label, (x0 + x1) / 2, T + 24 * d);
            c.textBaseline = 'middle';
          }
        }
        // most likely the adapter's own 40 MHz crystal (not yet proven with a second dongle): weak steady
        // lines at n x 40 MHz (not at the centre: DC is removed there)
        if (lr) {
          c.strokeStyle = 'rgba(163,174,187,0.55)';
          c.fillStyle = '#8a94a3';
          c.font = `${9 * d}px system-ui, sans-serif`;
          c.textAlign = 'center';
          c.textBaseline = 'bottom';
          c.setLineDash([2 * d, 3 * d]);
          for (let n = Math.ceil(rg.lo / 40e6); n * 40e6 < rg.hi; n++) {
            const hz = n * 40e6;
            if (Math.abs(hz - mid0) < 1e6 || !Display.onAxis(ax, hz)) continue;
            const x = Math.round(xOf(hz)) + 0.5;
            c.beginPath();
            c.moveTo(x, ph - 16 * d);
            c.lineTo(x, ph);
            c.stroke();
            c.fillText('spur', x, ph - 17 * d);
          }
          c.setLineDash([]);
          c.textBaseline = 'middle';
        }
        // requested frequency, when the hardware centre differs (5 MHz synthesizer grid)
        const m = v.src.marker;
        if (m !== null && Display.onAxis(ax, m) && Math.abs(m - mid0) > (rg.hi - rg.lo) / 200) {
          const x = Math.round(xOf(m)) + 0.5;
          c.strokeStyle = '#ffd54f';
          c.setLineDash([6 * d, 4 * d]);
          c.beginPath();
          c.moveTo(x, T);
          c.lineTo(x, ph);
          c.stroke();
          c.setLineDash([]);
          const label = `${st ? `${SLOT[v.slot]} ` : ''}${v.src.markerLabel} ${(m / 1e6).toFixed((m / 1e6) % 1 ? 1 : 0)}`;
          c.font = `${11 * d}px system-ui, sans-serif`;
          const lw = c.measureText(label).width;
          const lx = Math.min(W - lw - 4 * d, Math.max(L + 2 * d, x - lw / 2));
          c.fillStyle = 'rgba(11,14,19,0.85)';
          c.fillRect(lx - 3 * d, ph - 18 * d, lw + 6 * d, 15 * d);
          c.fillStyle = '#ffd54f';
          c.textAlign = 'left';
          c.textBaseline = 'top';
          c.fillText(label, lx, ph - 16 * d);
          c.textBaseline = 'middle';
        }
        c.globalAlpha = stale || this.opts.dim || v.src.dim || stalled ? 0.35 : 1;
        for (const k of v.keys) {
          const r = this.last[k];
          if (!r) continue;
          // drawn where its frequencies are on the axis (all of it, for one view)
          const { lo, hi } = Display.range(r);
          const xa = Math.max(0, Math.floor(Display.xAt(ax, lo))), xb = Math.min(w, Math.ceil(Display.xAt(ax, hi)));
          const draw = (arr: Float32Array, colour: string, lw: number) => {
            c.strokeStyle = colour;
            c.lineWidth = lw * d;
            c.beginPath();
            let pen = false; // stitched: the line breaks where another view owns the pixels
            for (let x = xa; x < xb; x++) {
              const at = Display.fAt(ax, x);
              if (!at || at.f < lo || at.f > hi || (st && owner[x] !== v.slot)) {
                pen = false;
                continue;
              }
              const fa = at.f, fx = at.fx;
              const b0 = Math.max(0, Math.floor((fa - lo) / r.df)), b1 = Math.min(r.bins, Math.max(b0 + 1, Math.floor((fa + fx - lo) / r.df)));
              let val = -1e9;
              for (let b = b0; b < b1; b++) val = Math.max(val, arr[b]);
              const y = Math.max(T, Math.min(ph, yOf(val)));
              if (pen) c.lineTo(L + x, y);
              else c.moveTo(L + x, y);
              pen = true;
            }
            c.stroke();
          };
          const p = k % 2;
          const h = this.hold[k];
          if (h && this.opts.showPeak) draw(h, PEAK[p], 1);
          if (this.opts.showAvg) draw(this.smoothed[k] ?? r.avg, v.src.pathColours?.[p] ?? v.src.colour, 1.3);
        }
        c.globalAlpha = 1;
      }
      if (st && views.length > 1) {
        const [a, b] = ordered;
        c.font = `${11 * d}px system-ui, sans-serif`;
        c.textBaseline = 'top';
        c.textAlign = 'center';
        if (b.lo > a.hi + 1e3 && ax.length === 1) {
          // not covered by either view
          const x0 = xOf(a.hi), x1 = xOf(b.lo);
          c.fillStyle = 'rgba(120,130,150,0.08)';
          c.fillRect(x0, T, x1 - x0, ph - T);
          c.fillStyle = '#8e99a7';
          if (x1 - x0 > 60 * d) c.fillText('not covered', (x0 + x1) / 2, T + 40 * d);
        } else if (b.lo <= a.hi + 1e3) {
          // the seams: wherever one view's part ends and the other's begins (two for nested spans)
          let labelled = false;
          for (let x = 1; x < w; x++) {
            if (owner[x] < 0 || owner[x - 1] < 0 || owner[x] === owner[x - 1]) continue;
            const sx = L + x + 0.5;
            c.strokeStyle = '#6b7889';
            c.setLineDash([4 * d, 4 * d]);
            c.beginPath();
            c.moveTo(sx, T);
            c.lineTo(sx, ph);
            c.stroke();
            c.setLineDash([]);
            const label = `${SLOT[owner[x - 1]]} │ ${SLOT[owner[x]]}${!labelled && overlap > 1e6 ? ` · overlap ${Math.round(overlap / 1e6)} MHz` : ''}`;
            labelled = true;
            const lw = c.measureText(label).width;
            c.fillStyle = 'rgba(11,14,19,0.85)';
            c.fillRect(sx - lw / 2 - 4 * d, T + 18 * d, lw + 8 * d, 15 * d); // under the channel labels
            c.fillStyle = '#d8dee6';
            c.fillText(label, sx, T + 20 * d);
          }
        }
        // a view's own message (stopped, starting) over its part
        c.font = `${13 * d}px system-ui, sans-serif`;
        c.textBaseline = 'middle';
        for (const v of views) {
          if (!v.src.msg) continue;
          const tw = c.measureText(v.src.msg).width + 20 * d;
          const mid = (xOf(Math.max(v.lo, s.f0)) + xOf(Math.min(v.hi, s.f1))) / 2;
          const cx = Math.min(W - tw / 2 - 4 * d, Math.max(L + tw / 2 + 4 * d, mid)), cy = T + 48 * d; // inside the plot
          c.fillStyle = 'rgba(19,24,32,0.92)';
          c.fillRect(cx - tw / 2, cy - 14 * d, tw, 28 * d);
          c.fillStyle = '#d6dde6';
          c.fillText(v.src.msg, cx, cy);
        }
      }
      // the other view (dual mode), when it shows the same axis
      const own = this.first();
      const g = this.ghost, gs = this.ghostSm;
      const ghostOn = !!(g && gs && own && performance.now() - this.ghostAt < 1500 && g.bins === own.bins && Math.abs(g.f0 - own.f0) < own.df / 2 && Math.abs(g.df - own.df) < 1e-6);
      if (ghostOn && g && gs) {
        c.globalAlpha = stale || this.opts.dim ? 0.35 : 0.9;
        c.strokeStyle = this.opts.ghostColour;
        c.lineWidth = 1.1 * d;
        c.beginPath();
        for (let x = 0; x < w; x++) {
          const b0 = Math.floor((x * g.bins) / w), b1 = Math.max(b0 + 1, Math.floor(((x + 1) * g.bins) / w));
          let v = -1e9;
          for (let b = b0; b < b1; b++) v = Math.max(v, gs[b]);
          const y = Math.max(T, Math.min(ph, yOf(v)));
          if (x === 0) c.moveTo(L + x, y);
          else c.lineTo(L + x, y);
        }
        c.stroke();
        c.globalAlpha = 1;
      }
      // hover readout
      if (this.hover !== null && !stale) {
        const f = this.freqAt(this.hover);
        // the trace under the pointer (stitched: the view whose frequencies these are)
        let k = -1, kd = Infinity;
        // stitched: the trace of the view that owns this pixel (as drawn)
        const own = this.opts.stitch ? owner[Math.round(this.hover * d - L)] ?? -1 : -1;
        this.last.forEach((q, i) => {
          if (!q || f === null || (this.opts.stitch && Math.floor(i / 2) !== own)) return;
          const rg = Display.range(q), dist = Math.abs(f - (rg.lo + rg.hi) / 2);
          if (f >= rg.lo && f <= rg.hi && dist < kd) [k, kd] = [i, dist];
        });
        const r = k >= 0 ? this.last[k] : null;
        if (f !== null && r) {
          const x = this.hover * d;
          c.strokeStyle = '#e0e6ee';
          c.globalAlpha = 0.5;
          c.beginPath();
          c.moveTo(x + 0.5, T);
          c.lineTo(x + 0.5, ph);
          c.stroke();
          c.globalAlpha = 1;
          const b = Math.max(0, Math.min(r.bins - 1, Math.round((f - r.f0) / r.df)));
          const sm = this.smoothed[k] ?? r.avg;
          const pk = this.hold[k] ?? r.max;
          // over the waterfall: frequency and age (the levels shown live are not that row's)
          const wy = this.hoverWfY * d;
          const older = this.hoverWfY >= 0 && this.sepRows >= 0 && wy > this.sepRows;
          const beforeStop = this.hoverWfY >= 0 && (this.opts.dim || (this.stopRows >= 0 && wy > this.stopRows));
          const wifi = this.hoverWfY >= 0 && !older && this.wifiKept.length ? this.wifiAt(f, Math.round(wy)) : null;
          // on one of the adapter's own lines (n x 40 MHz, within a few pixels)
          const rr = Display.range(r), n40 = Math.round(f / 40e6);
          const spur = Math.abs(f - n40 * 40e6) <= Math.max(2 * r.df, (rr.hi - rr.lo) / 300) && Math.abs(f - (rr.lo + rr.hi) / 2) > 1e6
            ? ` · likely adapter spur (${n40} × 40 MHz)` : '';
          const txt = older
            ? 'older rows: another frequency/span (see the label at the line)'
            : this.hoverWfY >= 0
              ? `${this.opts.stitch ? `${SLOT[r.slot]} · ` : ''}${(f / 1e6).toFixed(3)} MHz · ${beforeStop ? 'before Stop' : `${(wy / this.rowsPerS).toFixed(1)} s ago`}${wifi ? ` · ${wifi}` : ''}${spur}`
              : `${this.opts.stitch ? `${SLOT[r.slot]} · ` : ''}${(f / 1e6).toFixed(3)} MHz · avg ${sm[b].toFixed(1)} · peak ${pk[b].toFixed(1)} dBFS${spur}`;
          c.font = `${12 * d}px system-ui, sans-serif`;
          const tw = c.measureText(txt).width + 12 * d;
          const tx = Math.min(W - tw, x + 8 * d);
          // over the waterfall: just above the frequency axis (nearest the pointer); else below any overlay
          const ty = this.hoverWfY >= 0 ? ph - 44 * d : T + (this.opts.overlay ? 72 : 30) * d; // clear of the marker label
          c.fillStyle = 'rgba(20,26,34,0.92)';
          c.fillRect(tx, ty, tw, 20 * d);
          c.fillStyle = '#e0e6ee';
          c.textAlign = 'left';
          c.fillText(txt, tx + 6 * d, ty + 10 * d);
        }
      }
    }
    // overlay message: explicit (stopped/starting) or stale data
    const msg = this.opts.overlay ?? (stale ? this.opts.staleText : null);
    if (msg) {
      c.font = `${14 * d}px system-ui, sans-serif`;
      const tw = c.measureText(msg).width + 28 * d;
      const cx = (L + W) / 2, cy = T + 48 * d; // near the top: the traces stay visible below it
      c.fillStyle = 'rgba(19,24,32,0.92)';
      c.fillRect(cx - tw / 2, cy - 16 * d, tw, 32 * d);
      c.fillStyle = '#d6dde6';
      c.textAlign = 'center';
      c.textBaseline = 'middle';
      c.fillText(msg, cx, cy);
    }
  }
}
