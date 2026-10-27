// SPDX-License-Identifier: GPL-2.0-only
// Spectral Map: one adapter sweeps a range of channels, listening a while at each step; each sweep
// becomes a row of a heatmap (newest on top) on the band ruler's channel axis, so a column sits under
// its channel. Above it, the newest value of each MHz and the peak-hold over the sweeps kept. Each
// channel is heard only while the sweep is on it: the header says how long, and how often.
import { useEffect, useLayoutEffect, useRef, useState, type MouseEvent } from 'react';
import { RANGES, freqAt, pos, sweepPlan, type Range } from './bands';
import { TURBO } from './colormap';
import type { Hub } from './hub';
import type { Device } from './protocol';
import { centre } from './channels';
import { gappedDuty, mhz, type Prefs, type Scale } from './util';

const MAX_SWEEPS = 150;
const ROW_H = 6;     // CSS px per sweep in the heatmap
const AXIS_W = 46;   // CSS px gutter on the left (dB axis, colour bar, ages), as the other plots
const TOP = 22;
const AXIS_H = 22;
const SETTLE = 2;    // rows of a new run left out (the first ones after a retune carry its transient)
const NOT_HEARD = 0xff42382f; // ABGR: a step not heard yet (or failed): mid grey, apart from quiet blue
const GAP = 0xff221a14;       // between channel groups: nothing allowed there

type Sweep = { data: Float32Array; t: number }; // dBFS per MHz (NaN: not heard), when it ended
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const fresh = (n: number) => new Float32Array(n).fill(NaN);

export function SpectralMap(p: {
  hub: Hub; slot: number; range: Range; dev: Device | undefined; open: boolean; opening: boolean; running: boolean;
  dwell: number; gappedOk: boolean; prefs: Prefs; updatePrefs: (x: Partial<Prefs>) => void; scale: Scale; setScale: (s: Scale) => void;
  onStart: () => void; onStop: () => void; onPick: (f: number) => void; onStep: (centre: number | null, fs: number) => void;
  onFail: (why: string) => void; // the sweep gave up (the adapter stopped answering)
  note: string | null;            // why the sweep stopped by itself
}) {
  const plan = sweepPlan(p.range, p.dev, p.gappedOk);
  const spRef = useRef<HTMLCanvasElement>(null);
  const hmRef = useRef<HTMLCanvasElement>(null);
  // the sweep's data and the row callback's view of it (outside React state: rows arrive 25 times a
  // second); a new range, plan or listen time mounts a new map (the shell keys it)
  const st = useRef({
    gen: -1, inGen: [0, 0], rowCentre: 0, collect: null as null | { c: number; rows: number },
    sweeps: [] as Sweep[], partial: plan ? fresh(plan.n) : null, latest: plan ? fresh(plan.n) : null, step: -1, raf: 0,
  });
  const [info, setInfo] = useState({ sweeps: 0, last: 0 }); // sweeps done, the last one's seconds
  const [hover, setHover] = useState<{ x: number; y: number; text: string; left: boolean } | null>(null);
  // what the sweep and the drawing read (the sweep runs on across renders)
  const current = () => ({ scale: p.scale, prefs: p.prefs, setScale: p.setScale, range: p.range, plan, onStep: p.onStep, onFail: p.onFail });
  const opts = useRef(current());
  useEffect(() => {
    opts.current = current();
  });

  // rows of the sweeping adapter: the run byte (a retune starts a new run) and, while listening, the
  // values within the step (either antenna: the peak of what the adapter hears)
  useEffect(
    () =>
      p.hub.subscribe({
        slot: p.slot,
        row: (r) => {
          const s = st.current;
          if (r.gen !== s.gen) s.inGen = [0, 0];
          s.inGen[r.path & 1]++; // rows of the run per antenna (both when both are shown)
          s.gen = r.gen;
          s.rowCentre = r.f0 + r.df * (r.bins / 2 - 0.5);
          const col = s.collect, pl = opts.current.plan, into = s.partial; // (Clear replaces the partial row: the step goes on into the new one)
          if (!col || !pl || !into || Math.abs(s.rowCentre - col.c) > 2.5e6) return; // only rows of this step
          col.rows++;
          const src = opts.current.prefs.wfMode === 'avg' ? r.avg : r.max;
          for (let i = 0; i < r.bins; i++) {
            const f = r.f0 + i * r.df;
            if (Math.abs(f - col.c) > pl.step / 2 + 2.5e6) continue; // its share (on a 5 MHz grid a centre sits up to 2.5 MHz off)
            const k = Math.floor((f - pl.lo) / 1e6);
            if (k < 0 || k >= pl.n) continue;
            const v = src[i];
            if (!(into[k] >= v)) into[k] = v;
            if (s.latest) s.latest[k] = into[k];
          }
        },
      }),
    [p.hub, p.slot],
  );

  // the sweep: retune, wait for the new run to settle, listen, next; a row of the map per pass
  const active = p.running && p.open && !!plan;
  useEffect(() => {
    if (!active || !plan) return;
    const s = st.current;
    const me = { stop: false };
    (async () => {
      let failed = 0;
      while (!me.stop) {
        const t0 = performance.now();
        let heard = 0;
        for (let i = 0; i < plan.centres.length && !me.stop; i++) {
          const c = plan.centres[i];
          s.step = i;
          opts.current.onStep(c, plan.fs);
          const gen0 = s.gen;
          try {
            await p.hub.request('set', { slot: p.slot, freq: c, rate_sel: plan.rate_sel, wide: true, allow_gapped: plan.gapped }); // gapped: the user's switch
            failed = 0;
          } catch {
            if (me.stop) break; // stopped or left meanwhile: nothing to report
            if (++failed >= 5) return opts.current.onFail('the adapter did not take the sweep’s steps'); // the shell stops the map
            await sleep(500);
            continue;
          }
          const w0 = performance.now();
          // the new run on this step, past its first rows (or the radio was on this step already)
          while (!me.stop && performance.now() - w0 < 3000) {
            const here = Math.abs(s.rowCentre - c) < 2.5e6, rows = Math.max(...s.inGen);
            const settled = here && s.gen !== gen0 && rows >= SETTLE;
            const already = here && performance.now() - w0 > 400 && rows >= SETTLE;
            if (settled || already) break;
            await sleep(10);
          }
          if (me.stop) break;
          const col = { c, rows: 0 };
          s.collect = col;
          await sleep(p.dwell);
          if (s.collect === col) s.collect = null;
          if (col.rows) heard++;
          requestDraw();
        }
        if (me.stop) break;
        const done = s.partial;
        if (heard && done) {
          s.sweeps.unshift({ data: done, t: Date.now() });
          if (s.sweeps.length > MAX_SWEEPS) s.sweeps.pop();
        }
        s.partial = fresh(plan.n);
        setInfo({ sweeps: s.sweeps.length, last: (performance.now() - t0) / 1000 });
        fit();
        requestDraw();
      }
    })();
    return () => {
      me.stop = true;
      s.collect = null;
      s.step = -1;
      opts.current.onStep(null, 0);
      requestDraw();
    };
  }, [active]); // eslint-disable-line react-hooks/exhaustive-deps

  // Auto scale: from the noise of the newest sweep to over its peak
  function fit() {
    if (opts.current.prefs.scale !== null) return;
    const newest = st.current.sweeps[0]?.data;
    if (!newest) return;
    const vals = Array.from(newest).filter(Number.isFinite).sort((a, b) => a - b);
    if (vals.length < 10) return;
    const floor = vals[Math.floor(vals.length * 0.2)], peak = vals[vals.length - 1];
    const dbMin = Math.floor((floor - 8) / 5) * 5, dbMax = Math.max(dbMin + 30, Math.ceil((peak + 5) / 5) * 5);
    opts.current.setScale({ dbMin, dbMax, wfFloor: Math.min(dbMax - 10, Math.round(floor + 3)) });
  }

  // one draw per frame at most, whatever asked for it
  function requestDraw() {
    const s = st.current;
    if (s.raf) return;
    s.raf = requestAnimationFrame(() => {
      s.raf = 0;
      draw();
    });
  }

  // Drawing: both canvases on the channel axis (x: 0..1 of the plot width)
  function draw() {
    const sp = spRef.current, hm = hmRef.current, s = st.current, { scale, range, plan: pl, prefs } = opts.current;
    if (!sp || !hm || !pl) return;
    const d = window.devicePixelRatio || 1;
    for (const c of [sp, hm]) {
      const w = Math.max(1, Math.round(c.clientWidth * d)), h = Math.max(1, Math.round(c.clientHeight * d));
      if (c.width !== w || c.height !== h) [c.width, c.height] = [w, h];
    }
    const L = Math.round(AXIS_W * d), w = Math.floor(sp.width - L); // whole pixels: the image rows below index by w
    if (w <= 0) return;
    // the MHz bin under each pixel column (-1: a gap between channel groups, or a range edge)
    const groups = RANGES[range].groups;
    const col = new Int32Array(w);
    for (let x = 0; x < w; x++) {
      const f = freqAt(range, (x + 0.5) / w);
      const inGroup = groups.some((g) => f >= centre(g.chans[0]) - g.half && f <= centre(g.chans[g.chans.length - 1]) + g.half);
      const k = Math.floor((f - pl.lo) / 1e6);
      col[x] = inGroup && k >= 0 && k < pl.n ? k : -1;
    }
    const lo = Math.min(scale.dbMax - 10, Math.max(scale.dbMin, scale.wfFloor)), hi = scale.dbMax;
    const colour = (v: number) => TURBO[Math.max(0, Math.min(255, Math.round(30 + ((v - lo) * 225) / Math.max(1, hi - lo))))];

    // spectrum: the newest value of each MHz, and the peak-hold over the sweeps kept
    const c = sp.getContext('2d')!, H = sp.height, T = TOP * d, ph = H - AXIS_H * d;
    c.fillStyle = '#0b0e13';
    c.fillRect(0, 0, sp.width, H);
    c.fillStyle = '#141a22'; // outside the channel groups
    for (let x = 0; x < w; x++) if (col[x] < 0) c.fillRect(L + x, T, 1, ph - T);
    const yOf = (db: number) => T + (ph - T) * (1 - (db - scale.dbMin) / (scale.dbMax - scale.dbMin));
    c.font = `${11 * d}px system-ui, sans-serif`;
    c.textBaseline = 'middle';
    c.textAlign = 'right';
    const step = scale.dbMax - scale.dbMin > 60 ? 10 : 5;
    for (let db = Math.ceil(scale.dbMin / step) * step; db <= scale.dbMax; db += step) {
      const y = Math.round(yOf(db)) + 0.5;
      c.strokeStyle = '#1f2630';
      c.beginPath();
      c.moveTo(L, y);
      c.lineTo(sp.width, y);
      c.stroke();
      c.fillStyle = '#7d8896';
      c.fillText(String(db), L - 6 * d, y);
    }
    c.textAlign = 'left';
    c.textBaseline = 'top';
    c.fillText('dBFS', 4 * d, 3 * d);
    const hold = fresh(pl.n);
    for (const arr of [s.partial, ...s.sweeps.map((x) => x.data)])
      if (arr) for (let k = 0; k < pl.n; k++) if (!(hold[k] >= arr[k])) hold[k] = arr[k];
    const line = (arr: Float32Array | null, colourOf: string, lw: number) => {
      if (!arr) return;
      c.strokeStyle = colourOf;
      c.lineWidth = lw * d;
      c.beginPath();
      let pen = false;
      for (let x = 0; x < w; x++) {
        const v = col[x] >= 0 ? arr[col[x]] : NaN;
        if (!Number.isFinite(v)) {
          pen = false;
          continue;
        }
        const y = Math.max(T, Math.min(ph, yOf(v)));
        if (pen) c.lineTo(L + x, y);
        else c.moveTo(L + x, y);
        pen = true;
      }
      c.stroke();
    };
    if (prefs.showPeak) line(hold, '#8d99a8', 1);
    if (prefs.showAvg) line(s.latest, '#e6ebf1', 1.3);
    // MHz under every other channel of each group
    c.textAlign = 'center';
    c.textBaseline = 'middle';
    c.fillStyle = '#7d8896';
    for (const g of groups) for (const ch of g.chans.filter((_, i) => i % 2 === 0)) c.fillText(mhz(centre(ch), 0), L + pos(range, centre(ch)) * w, ph + (AXIS_H * d) / 2);
    // where the sweep is
    if (s.step >= 0 && s.step < pl.centres.length) {
      const x = L + pos(range, pl.centres[s.step]) * w;
      c.strokeStyle = 'rgba(255,255,255,0.7)';
      c.setLineDash([3 * d, 3 * d]);
      c.beginPath();
      c.moveTo(x, T);
      c.lineTo(x, ph);
      c.stroke();
      c.setLineDash([]);
    }

    // heatmap: the sweep in progress on top, then the sweeps kept, newest first (one image)
    const m = hm.getContext('2d')!, rh = Math.max(2, Math.round(ROW_H * d));
    m.fillStyle = '#0b0e13';
    m.fillRect(0, 0, hm.width, hm.height);
    const rows = [s.partial, ...s.sweeps.map((x) => x.data)];
    const n = Math.min(rows.length, Math.floor(hm.height / rh));
    if (n > 0) {
      const img = m.createImageData(w, n * rh);
      const px = new Uint32Array(img.data.buffer);
      for (let r = 0; r < n; r++) {
        const arr = rows[r];
        for (let x = 0; x < w; x++) {
          const k = col[x];
          const v = k < 0 ? GAP : arr && Number.isFinite(arr[k]) ? colour(arr[k]) : NOT_HEARD;
          for (let y = 0; y < rh - 1; y++) px[(r * rh + y) * w + x] = v; // a 1 px line between sweeps
          px[(r * rh + rh - 1) * w + x] = 0xff130e0b;
        }
      }
      m.putImageData(img, L, 0);
    }
    // the gutter: the sweeps' ages on top, the colour bar at the bottom
    m.fillStyle = '#0b0e13';
    m.fillRect(0, 0, L, hm.height);
    m.fillStyle = '#7d8896';
    m.font = `${10 * d}px system-ui, sans-serif`;
    m.textAlign = 'right';
    m.textBaseline = 'middle';
    const barH = Math.min(hm.height * 0.4, 120 * d), barY = hm.height - barH - 8 * d;
    m.fillText('now', L - 7 * d, rh / 2);
    const now = Date.now(), every = s.sweeps.length > 40 ? 10 : 5;
    for (let r = every; r <= s.sweeps.length && r * rh < barY - 14 * d; r += every) {
      const y = r * rh + rh / 2;
      m.fillRect(L - 5 * d, y, 4 * d, 1);
      m.fillText(`−${Math.round((now - s.sweeps[r - 1].t) / 1000)} s`, L - 7 * d, y);
    }
    if (barH > 20 * d) {
      const bar = m.createImageData(Math.round(6 * d), Math.round(barH));
      const bp = new Uint32Array(bar.data.buffer);
      for (let y = 0; y < bar.height; y++) for (let x = 0; x < bar.width; x++) bp[y * bar.width + x] = TURBO[Math.round(255 - (225 * y) / Math.max(1, bar.height - 1))];
      m.putImageData(bar, Math.round(4 * d), Math.round(barY));
      m.textAlign = 'left';
      m.textBaseline = 'top';
      m.fillText(String(Math.round(hi)), 13 * d, barY);
      m.textBaseline = 'bottom';
      m.fillText(String(Math.round(lo)), 13 * d, barY + barH);
    }
  }
  useLayoutEffect(() => {
    const ro = new ResizeObserver(() => requestDraw());
    if (spRef.current) ro.observe(spRef.current);
    if (hmRef.current) ro.observe(hmRef.current);
    const s = st.current;
    return () => {
      ro.disconnect();
      cancelAnimationFrame(s.raf);
      s.raf = 0;
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => requestDraw(), [p.scale, p.range, p.prefs.showAvg, p.prefs.showPeak]); // eslint-disable-line react-hooks/exhaustive-deps

  // hover: the channel, the newest value and the peak-hold, or that sweep's value and age; click: leave
  // the map onto that channel
  const at = (e: MouseEvent<HTMLCanvasElement>, heat: boolean) => {
    const r = e.currentTarget.getBoundingClientRect(), x = e.clientX - r.left - AXIS_W;
    const w = r.width - AXIS_W, pl = plan;
    if (x < 0 || x > w || !pl) return null;
    const f = freqAt(p.range, x / w);
    const g = RANGES[p.range].groups.find((q) => f >= centre(q.chans[0]) - q.half && f <= centre(q.chans[q.chans.length - 1]) + q.half);
    if (!g) return null;
    const ch = g.chans.reduce((a, b) => (Math.abs(centre(b) - f) < Math.abs(centre(a) - f) ? b : a));
    const k = Math.floor((f - pl.lo) / 1e6), s = st.current;
    const d = window.devicePixelRatio || 1, rh = Math.max(2, Math.round(ROW_H * d));
    const db = (v: number | undefined) => (v !== undefined && Number.isFinite(v) ? `${v.toFixed(0)} dBFS` : 'not heard');
    let what: string;
    if (heat) {
      const row = Math.floor(((e.clientY - r.top) * d) / rh);
      const sw = row > 0 ? s.sweeps[row - 1] : null;
      what = row === 0 ? `this sweep: ${db(s.partial?.[k])}` : sw ? `${db(sw.data[k])} · ${Math.round((Date.now() - sw.t) / 1000)} s ago` : '';
    } else {
      let peak = NaN; // as the grey line: the sweep in progress counts too
      for (const arr of [s.partial, ...s.sweeps.map((x) => x.data)]) if (arr && !(peak >= arr[k])) peak = arr[k];
      what = `newest ${db(s.latest?.[k])} · peak ${db(peak)}`;
    }
    return { ch, text: `ch ${ch} · ${mhz(f, 0)} MHz${what ? ` · ${what}` : ''} · click: open this channel` };
  };
  const onMove = (e: MouseEvent<HTMLCanvasElement>, heat: boolean) => {
    const a = at(e, heat);
    const box = e.currentTarget.parentElement!.getBoundingClientRect();
    const x = e.clientX - box.left;
    setHover(a ? { x, y: e.clientY - box.top, text: a.text, left: x > box.width - 380 } : null); // near the right edge: to the left
  };
  const onClick = (e: MouseEvent<HTMLCanvasElement>, heat: boolean) => {
    const a = at(e, heat);
    if (a) p.onPick(centre(a.ch));
  };

  const sweepS = info.last || (plan ? (plan.centres.length * (p.dwell + 150)) / 1000 : 0);
  const dwellS = p.dwell >= 1000 ? `${p.dwell / 1000} s` : `${p.dwell} ms`;
  // a gapped step samples only part of its listen
  const duty = plan?.gapped ? gappedDuty(plan.fs, (p.dev?.speed_mbps ?? 0) >= 5000) : 1;
  return (
    <main className="smap" aria-label="Spectral map">
      <header className="vhead maphead">
        <span className="mtitle">Spectral Map</span>
        <span className="muted chlabel">
          {RANGES[p.range].title.replace(/^Channels [^:]*: /, '')} ·{' '}
          {plan ? `${plan.centres.length} steps of ${plan.fs / 1e6} MHz${plan.gapped ? ' (gapped)' : ''}` : 'no adapter'}
          {info.sweeps ? ` · last sweep ${info.last.toFixed(1)} s · ${info.sweeps} sweep${info.sweeps > 1 ? 's' : ''}` : ''}
        </span>
        {plan && (
          <span className="amber" title="A sweep visits one step at a time: what happens on a channel while the sweep is elsewhere is not seen. A map cell holds the peak (or mean) of that listen.">
            each channel heard {dwellS}
            {duty < 1 ? ` (~${Math.round(duty * 100)} % sampled)` : ''} of every {sweepS.toFixed(1)} s (
            {Math.round(((duty * p.dwell) / 1000 / Math.max(0.1, sweepS)) * 100)} %)
            <span className="long">: bursts between visits are missed</span>
          </span>
        )}
        {p.note && <span className="warn-note">{p.note}</span>}
        <span className="spacer" />
        <span className="state">
          <span className={active ? 'dot ok' : p.opening ? 'dot warn' : 'dot'} />
          <span className="stext">{active ? 'sweeping' : p.opening ? 'starting…' : 'stopped'}</span>
        </span>
        <button
          onClick={() => {
            const s = st.current;
            s.sweeps = [];
            if (plan) {
              s.partial = fresh(plan.n); // the sweep in progress goes on into the new one
              s.latest = fresh(plan.n);
            }
            setInfo({ sweeps: 0, last: 0 });
            requestDraw();
          }}
          title="Start the map afresh"
        >
          Clear map
        </button>
        {p.running ? (
          <button onClick={p.onStop} title="Stop the sweep and the adapter (Space)">
            ■ Stop
          </button>
        ) : (
          <button disabled={!p.dev || !p.dev.can_open || p.opening} onClick={p.onStart} title="Start the adapter and sweep (Space)">
            ▶ Start
          </button>
        )}
        <button className="rec" disabled title="Recording needs a continuous span: a sweep jumps between frequencies">
          ● Rec
        </button>
      </header>
      <div className="smap-plots">
        <canvas ref={spRef} className="spectrum" onMouseMove={(e) => onMove(e, false)} onMouseLeave={() => setHover(null)} onClick={(e) => onClick(e, false)} />
        <canvas ref={hmRef} className="waterfall" onMouseMove={(e) => onMove(e, true)} onMouseLeave={() => setHover(null)} onClick={(e) => onClick(e, true)} />
        <div className="legend pano-legend">
          {(
            [
              ['newest', '#e6ebf1', p.prefs.showAvg, () => p.updatePrefs({ showAvg: !p.prefs.showAvg })],
              [`peak-hold (${info.sweeps} sweeps)`, '#8d99a8', p.prefs.showPeak, () => p.updatePrefs({ showPeak: !p.prefs.showPeak })],
            ] as [string, string, boolean, () => void][]
          ).map(([label, colour, on, fn]) => (
            <button key={label.split(' ')[0]} className={on ? 'on' : 'off'} aria-pressed={on} onClick={fn} title={`${on ? 'Hide' : 'Show'} the ${label.split(' ')[0]} line`}>
              <span className="swatch" style={{ background: colour }} />
              {label}
            </button>
          ))}
          <span className="mapkey">
            <i style={{ background: '#2f3842' }} />
            not heard yet
          </span>
        </div>
        {hover && (
          <div className="smap-hover" style={hover.left ? { right: `calc(100% - ${hover.x - 12}px)`, top: hover.y + 12 } : { left: hover.x + 12, top: hover.y + 12 }}>
            {hover.text}
          </div>
        )}
        {!p.dev && <div className="vcard">No adapter for the map. Plug in an RTL8812BU or RTL8812CU.</div>}
        {p.dev && !p.running && !info.sweeps && (
          <div className="vcard">
            <h2>Map the {RANGES[p.range].label === '2.4 GHz' ? '2.4 GHz band' : `5 GHz ${p.range === '5' ? 'band' : `channels ${RANGES[p.range].label}`}`}</h2>
            <p>
              ▶ Start sweeps it with the {p.dev.model}. Each sweep adds a row, newest on top; a click on a channel opens it in the
              views.
            </p>
          </div>
        )}
      </div>
    </main>
  );
}
