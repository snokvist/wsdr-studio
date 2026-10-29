// SPDX-License-Identifier: GPL-2.0-only
// The window: top bar (mode), sidebar (the selected view's settings, the links, the display), and one
// view (Single) or two (Panorama: side by side in channel order; Compare: stacked, same channel).
// Two-view links: a linked setting changed in either view is applied to the other (the frequency
// keeps the views' distance).
import { useEffect, useRef, useState } from 'react';
import { Hub } from './hub';
import type { Device, RxSettings, WifiFrame } from './protocol';
import { Station, type Link, type StationHandle, type ViewInfo } from './Station';
import { RANGES, planId, sweepPlan, type Range } from './bands';
import { BandRuler } from './BandRuler';
import { OTHER_COLOUR, WIFI_COLOUR } from './display';
import { Panorama } from './Panorama';
import { SpectralMap } from './SpectralMap';
import { survey, type Survey } from './survey';
import { NumField, Seg } from './ui';
import {
  FFTS, LOCKED, SLOT_NAME, bandOf, bandsOf, baseName, bytes, contMsps, devKey, gappedDuty, isDual, loadPrefs, savePrefs, snap, spanLabel, type Locks, type Mode, type Prefs, type Scale,
} from './util';

// [keys, what, the modes it acts in]
const ALL: Mode[] = ['single', 'panorama', 'compare', 'map'];
const VIEWS: Mode[] = ['single', 'panorama', 'compare'];
const TWO: Mode[] = ['panorama', 'compare'];
const KEYS: [string, string, Mode[]][] = [
  ['← →', 'tune 5 MHz (Shift: 20 MHz)', VIEWS],
  ['[ ]', 'narrower / wider span (continuous spans; gapped ones are picked in the Span list)', VIEWS],
  ['0–9', 'type a frequency (Enter applies)', VIEWS],
  ['A B', 'select view A / B', TWO],
  ['Enter', 'go to the frequency field', VIEWS],
  ['Space', 'Start / Stop', VIEWS],
  ['Space', 'start / stop the sweep', ['map']],
  ['Ctrl+R', 'Record / end the recording', VIEWS],
  ['L', 'link the frequencies of A and B', TWO],
  ['O', 'the other view’s overlay', ['compare']],
  ['Ctrl+B', 'sidebar', ALL],
  ['?', 'this list (Esc closes)', ALL],
  ['Double-click', 'on the spectrum: tune there', VIEWS],
  ['Wheel', 'over the band ruler: 5 MHz steps', VIEWS],
];
const MODES: [Mode, string, string][] = [
  ['single', 'Single', 'One view'],
  ['panorama', 'Panorama', 'Two adapters, one picture: B continues where A ends'],
  ['compare', 'Compare', 'Two adapters on the same frequencies, one above the other'],
  ['map', 'Map', 'One adapter sweeps the band: a heatmap of what each channel had'],
];

// afterClose: the view is being closed (stopped before the map): its tuning goes back once it is
// (a set to the closing adapter would be lost, and the close's draft would be the sweep's last step)
type RestoreItem = {
  slot: number; key: string | null; pid: number | null; args?: Partial<RxSettings>; start: boolean; at: number; gappedOk: boolean; afterClose?: boolean;
};

export default function App() {
  const [prefs, setPrefs] = useState<Prefs>(loadPrefs);
  const updatePrefs = (p: Partial<Prefs>) =>
    setPrefs((old) => {
      const n = { ...old, ...p };
      savePrefs(n);
      return n;
    });
  const [scale, setScale] = useState<Scale>(() => loadPrefs().scale ?? { dbMin: -110, dbMax: -20, wfFloor: -100 });
  const [hub] = useState(() => new Hub());
  const [devices, setDevices] = useState<Device[]>([]);
  const [openKey, setOpenKey] = useState<(string | null)[]>([null, null]); // the adapter open in each slot
  const [views, setViews] = useState<(ViewInfo | null)[]>([null, null]);
  const [selSlot, setSelSlot] = useState(0);
  const [confirm, setConfirm] = useState<{ to: Mode; slot: number } | null>(null); // stop that view first?
  const [mapSlot, setMapSlot] = useState(0);   // Map: the view whose adapter sweeps
  const [mapRun, setMapRun] = useState(false); // Map: sweeping
  const [mapStep, setMapStep] = useState<{ c: number; fs: number } | null>(null); // where the sweep is
  // Map: what to put back on leaving it (the sweeping view's tuning, whether it ran; the view stopped for
  // it, with its adapter)
  const mapReturn = useRef<{
    mode: Mode; freq: number; rate_sel: number; gapped: boolean; wasOpen: boolean; other: { slot: number; key: string | null } | null;
  } | null>(null);
  const [mapOther, setMapOther] = useState<number | null>(null); // that view stays mounted (hidden): it keeps its adapter
  // Views to put back once they have settled (not opening; a Stop's close done): tuning, and a start
  // when they ran. key: the adapter it is for (a view whose adapter changed meanwhile is left alone).
  // pid: its model too (a port can get another adapter); at: when queued (it lapses after 15 s);
  // gappedOk: its span was gapped before the map (else a span the adapter now shows only gapped is
  // not asked for: e.g. replugged at USB 2 meanwhile)
  const restore = useRef<RestoreItem[]>([]);
  // bumped by every Stop, leave and entry: a Start or a restore queued behind a close does nothing
  // if anything happened since
  const mapSeq = useRef(0);
  const [restoreKick, setRestoreKick] = useState(0);
  const mapClose = useRef<Promise<unknown> | null>(null); // a Stop's close in flight
  const backTo = useRef<Mode | null>(null); // the mode the map was entered from (no new alignment going back to it)
  const [mapNote, setMapNote] = useState<string | null>(null); // why the sweep stopped by itself
  const [help, setHelp] = useState(false); // the keyboard sheet
  const [narrow, setNarrow] = useState(() => matchMedia('(max-width: 1279px)').matches);
  const [drawer, setDrawer] = useState(false);
  const [sbEl, setSbEl] = useState<HTMLDivElement | null>(null);
  const handles = useRef<(StationHandle | null)[]>([null, null]);
  const leaving = useRef(0); // "Stop B" sent (and a few seconds after): B's recording note goes to A
  const [surveyMap] = useState<Survey>(() => new Map()); // grows in place; the ruler redraws it itself
  // networks heard this session (beacons decoded from the samples): BSSID -> name and channel; the
  // ruler's names per channel follow at most once a second
  const netsRef = useRef(new Map<string, { ssid: string; ch: number }>());
  const [nets, setNets] = useState<Map<number, string[]>>(() => new Map());
  const netsDirty = useRef(false);
  const { mode, locks } = prefs;
  const dual = isDual(mode);
  const inMap = mode === 'map';
  const sel = inMap ? mapSlot : dual ? selSlot : 0;
  const now = useRef({ mode, locks, views });

  useEffect(() => {
    hub.start();
    const unsubscribe = hub.subscribe({
      slot: null,
      event: (e) => {
        if (e.type === 'devices') setDevices(e.devices);
        else if (e.type === 'caps' && e.slot !== undefined)
          setOpenKey((k) => k.map((x, i) => (i === e.slot ? (e.open && e.bus >= 0 ? `${e.bus}-${e.port}` : null) : x)));
        else if (e.type === 'recorded' && e.slot !== undefined && (!handles.current[e.slot] || leaving.current > 0))
          // a view that is not shown (it was stopped on leaving the two-view modes): its note goes to one that is
          handles.current.find((h, i) => h && i !== e.slot)?.notice(`View ${SLOT_NAME[e.slot]}'s recording saved: ${baseName(e.file)} · ${bytes(e.bytes)}`);
      },
    });
    return () => {
      unsubscribe();
      hub.stop();
    };
  }, [hub]);
  useEffect(() => {
    const mq = matchMedia('(max-width: 1279px)');
    const on = () => setNarrow(mq.matches);
    mq.addEventListener('change', on);
    return () => mq.removeEventListener('change', on);
  }, []);
  const sidebarShown = narrow ? drawer : prefs.sidebar;
  const toggleSidebar = () => (narrow ? setDrawer((v) => !v) : updatePrefs({ sidebar: !prefs.sidebar }));
  // The drawer (narrow windows) is a dialog: focus moves into it, Escape closes it, focus returns.
  const toggleRef = useRef<HTMLButtonElement>(null);
  const asideRef = useRef<HTMLElement>(null);
  const drawerOpen = narrow && drawer;
  useEffect(() => {
    if (!drawerOpen) return;
    asideRef.current?.querySelector<HTMLElement>('button, select, input')?.focus();
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && setDrawer(false);
    window.addEventListener('keydown', onKey);
    const toggle = toggleRef.current;
    return () => {
      window.removeEventListener('keydown', onKey);
      toggle?.focus();
    };
  }, [drawerOpen]);

  // The other view takes the linked fields of a change: the frequency at the same distance (snapped to
  // the tuning grid, in its band), a span only when its adapter shows it continuously, gain and AGC.
  const forward = (slot: number, args: Partial<RxSettings>, use: Locks) => {
    const other = handles.current[slot ^ 1], me = handles.current[slot];
    if (!isDual(now.current.mode) || !other || !me) return;
    const name = SLOT_NAME[slot ^ 1];
    const a: Partial<RxSettings> = {};
    if (use.freq && args.freq !== undefined) {
      const want = args.freq + (other.freq() - me.freq());
      const f = snap(want);
      if (Math.abs(f - want) > 2.5e6 || bandOf(want) !== bandOf(other.freq()))
        me.notice(`View ${name} cannot follow: ${(want / 1e6).toFixed(0)} MHz is outside its band`);
      else a.freq = f;
    }
    if (use.span && args.rate_sel !== undefined) {
      if (other.rateOk(args.rate_sel)) {
        a.rate_sel = args.rate_sel;
        a.wide = args.wide ?? true;
      } else me.notice(`View ${name} keeps its span: its adapter cannot show ${spanLabel(80e6 / (1 << args.rate_sel))} continuously`);
    }
    if (use.gain && args.gain_db !== undefined) a.gain_db = args.gain_db;
    if (use.gain && args.agc !== undefined) a.agc = args.agc;
    if (!Object.keys(a).length) return;
    const why = other.busy();
    if (why) me.notice(`View ${name} did not follow: ${why}`); // a recording must not be ended by a link
    else other.linkedSet(a);
  };
  const fwd = useRef(forward);
  // One scale for both views, fitted to both pictures.
  const fit = (wfMode?: 'max' | 'avg') => {
    if (now.current.mode === 'map') return; // the map fits its own
    const s = handles.current
      .filter((_, i) => i === 0 || isDual(now.current.mode))
      .map((h) => h?.suggest(wfMode) ?? null)
      .filter((x): x is Scale => x !== null);
    if (s.length)
      setScale({ dbMin: Math.min(...s.map((x) => x.dbMin)), dbMax: Math.max(...s.map((x) => x.dbMax)), wfFloor: Math.min(...s.map((x) => x.wfFloor)) });
  };
  const autoFit = useRef(fit);
  // The views call these from callbacks made at mount: they read the current state through refs.
  const [link] = useState<Link>(() => ({
    register: (slot, h) => {
      handles.current[slot] = h;
      if (!h) setViews((old) => old.map((x, i) => (i === slot ? null : x))); // a view gone: its report too
    },
    report: (slot, v) => setViews((old) => old.map((x, i) => (i === slot ? v : x))),
    userSet: (slot, args) => fwd.current(slot, args, now.current.locks),
    autoFit: () => autoFit.current(),
    survey: (r) => {
      survey(surveyMap, r);
    },
    wifi: (frames: WifiFrame[]) => {
      for (const f of frames) {
        if (!f[9] || f[10] == null) continue; // not a beacon
        // its primary channel: from the beacon, else the 20 MHz channel it was heard on
        const ch = f[11] || (f[2] === 20 && f[1] < 3000 && f[1] !== 2484 ? Math.round((f[1] - 2407) / 5) : f[2] === 20 && f[1] > 5000 ? Math.round((f[1] - 5000) / 5) : 0);
        if (!ch) continue;
        const old = netsRef.current.get(f[9]);
        const ssid = f[10] || old?.ssid || ''; // a hidden network's beacon must not hide a name a probe response gave
        if (old && old.ssid === ssid && old.ch === ch) continue;
        if (!old && netsRef.current.size >= 300) netsRef.current.delete(netsRef.current.keys().next().value!); // (a flood: the oldest goes)
        netsRef.current.set(f[9], { ssid, ch });
        netsDirty.current = true;
      }
    },
  }));
  useEffect(() => {
    const t = window.setInterval(() => {
      if (!netsDirty.current) return;
      netsDirty.current = false;
      const sets = new Map<number, Set<string>>(); // one name once per channel (an access point may have several BSSIDs)
      for (const n of netsRef.current.values()) {
        if (!sets.has(n.ch)) sets.set(n.ch, new Set());
        sets.get(n.ch)!.add(n.ssid || '(hidden)');
      }
      setNets(new Map([...sets].map(([ch, s]) => [ch, [...s]])));
    }, 1000);
    return () => window.clearInterval(t);
  }, []);
  // the latest state and functions for those callbacks, after every render
  useEffect(() => {
    now.current = { mode, locks, views };
    fwd.current = forward;
    autoFit.current = fit;
  });

  const setLock = (k: keyof Locks) => {
    const on = !locks[k];
    updatePrefs({ locks: { ...locks, [k]: on } });
    // engaging a span or gain link aligns the other view to the selected one now
    const v = views[sel];
    if (on && v && k !== 'freq')
      forward(sel, k === 'span' ? { rate_sel: v.rate_sel, wide: true } : { gain_db: v.gain_db, agc: v.agc }, { freq: false, span: k === 'span', gain: k === 'gain' });
  };
  // Entering Panorama puts B one span above A; Compare puts it on A's frequency. Pending until both
  // views have appeared and reported.
  const prevMode = useRef(mode);
  const alignPending = useRef<Mode | null>(isDual(mode) ? mode : null); // also at a start in a two-view mode
  useEffect(() => {
    if (prevMode.current !== mode) {
      // (back from the map: the views go back where they were, no new alignment)
      alignPending.current = isDual(mode) && !(prevMode.current === 'map' && backTo.current === mode) ? mode : null;
      prevMode.current = mode;
    }
    const a = handles.current[0], b = handles.current[1], va = views[0];
    if (!alignPending.current || !a || !b || !va?.ready || !views[1]?.ready) return; // what A runs, B's rates known
    alignPending.current = null;
    const why = b.busy();
    if (why) {
      a.notice(`View B did not move: ${why}`);
      return;
    }
    const args: Partial<RxSettings> = {};
    const sameSpan = b.rateOk(va.rate_sel);
    if (sameSpan) {
      args.rate_sel = va.rate_sel;
      args.wide = true;
    }
    if (mode === 'compare') {
      // comparing one channel: the same gain too, or the quieter view just looks empty
      args.gain_db = va.gain_db;
      args.agc = va.agc;
    }
    // Panorama: B's span starts where A's ends (each half of its own span from its centre)
    // (from where the radio centres A: at 40 MHz on 2.4 GHz that may not be the asked frequency)
    const want = (mode === 'panorama' ? va.centre : va.freq) + (mode === 'panorama' ? (va.fsNow + (sameSpan ? va.fsNow : b.fs())) / 2 : 0);
    // (and in B's own band: an 8812EU in B cannot follow A to 2.4 GHz)
    if (Math.abs(snap(want) - want) <= 2.5e6 && bandOf(want) === bandOf(va.freq) && bandOf(want) === bandOf(b.freq())) args.freq = snap(want);
    else a.notice(`View B stays: ${(want / 1e6).toFixed(0)} MHz is outside the band`);
    if (Object.keys(args).length) b.linkedSet(args);
  }, [mode, views]); // eslint-disable-line react-hooks/exhaustive-deps
  // Switching modes. Single and Map show one view: a running other view is stopped first (asked).
  const setMode = (m: Mode) => {
    setConfirm(null);
    if (m === mode) return;
    if (m === 'map') {
      const slot = dual ? selSlot : 0;
      if (views[slot]?.recording) return handles.current[slot]?.notice('End the recording first: the map retunes this adapter');
    }
    if (dual && !isDual(m)) {
      const other = m === 'single' ? 1 : selSlot ^ 1;
      if (views[other]?.locked || openKey[other]) {
        setConfirm({ to: m, slot: other }); // that view is running: ask first
        return;
      }
    }
    enter(m, null);
  };
  // stopped: the view stopped for it (Map starts it again on leaving); the map sweeps with the other one
  const enter = (m: Mode, stopped: number | null) => {
    if (m === 'map') {
      const slot = stopped !== null ? stopped ^ 1 : dual ? selSlot : 0;
      setMapSlot(slot);
      setMapOther(stopped);
      setMapRun(false);
      const vs = now.current.views; // as they are now (this may run after a close, seconds after the click)
      const v = vs[slot];
      restore.current = [];
      mapSeq.current++;
      setMapNote(null);
      mapReturn.current = v
        ? { mode, freq: v.freq, rate_sel: v.rate_sel, gapped: v.gapped && v.allowGapped, wasOpen: v.open, other: stopped !== null ? { slot: stopped, key: vs[stopped]?.selKey ?? null } : null }
        : null;
    } else if (mode === 'map') leaveMap(m, null);
    updatePrefs({ mode: m });
  };
  // Leaving the map: the sweeping view back where it was (or onto the channel picked in the map); in
  // Single only view A is shown, so a sweeping B stops; a view stopped for the map starts again.
  // The span goes back too (the sweep's may be one the user never picked, gapped on USB 2).
  const leaveMap = (to: Mode, f: number | null) => {
    const r = mapReturn.current, slot = mapSlot;
    mapReturn.current = null;
    const seq = ++mapSeq.current;
    backTo.current = r?.mode ?? null;
    setMapRun(false);
    setMapStep(null);
    setMapOther(null);
    const dev = devices.find((d) => devKey(d) === views[slot]?.selKey);
    const rate_sel = r?.rate_sel ?? (dev ? Math.max(1, Math.ceil(Math.log2(80 / contMsps(dev)))) : 4); // else the widest continuous
    const args = { freq: f !== null ? snap(f) : r ? r.freq : views[slot]?.freq ?? 5180e6, rate_sel, wide: true };
    const shownAfter = (i: number) => (isDual(to) ? true : i === 0);
    const key = views[slot]?.selKey ?? null;
    const pidOf = (k: string | null) => devices.find((d) => devKey(d) === k)?.pid ?? null;
    const queue = () => {
      if (seq !== mapSeq.current) return; // entered the map again meanwhile
      const at = Date.now();
      const items: RestoreItem[] = [];
      const gappedOk = !!r?.gapped; // (a span the user picked gapped: see enter)
      // shown afterwards, it goes back as it was: a view stopped before the map (and started by its
      // Start) stops again, unless a channel was picked in the map
      const stopAgain = !!r && !r.wasOpen && f === null && !!handles.current[slot]?.running();
      if ((to === 'single' && slot === 1) || stopAgain)
        hub.request('close', { slot }).catch((e: Error) => {
          if (seq === mapSeq.current) restore.current = restore.current.filter((x) => x.slot !== slot); // it keeps running as it is
          (handles.current[slot] ?? handles.current[0])?.notice(`Could not stop view ${SLOT_NAME[slot]}: ${e.message}`); // (B is gone in Single)
        });
      if (!(to === 'single' && slot === 1))
        items.push({ slot, key, pid: pidOf(key), args, start: !!r?.wasOpen && !!key, at, gappedOk, afterClose: stopAgain });
      if (r?.other && shownAfter(r.other.slot) && r.other.key)
        items.push({ slot: r.other.slot, key: r.other.key, pid: pidOf(r.other.key), start: true, at, gappedOk: false });
      restore.current = [...restore.current.filter((x) => !items.some((i) => i.slot === x.slot)), ...items];
      setRestoreKick((k) => k + 1);
    };
    if (mapClose.current) void mapClose.current.finally(queue); // a Stop still closing: after it
    else queue();
  };
  const pickFromMap = (f: number) => {
    const to = mapReturn.current?.mode ?? 'single';
    leaveMap(to === 'map' ? 'single' : to, f);
    updatePrefs({ mode: to === 'map' ? 'single' : to });
  };
  useEffect(() => {
    restore.current = restore.current.filter((x) => {
      const v = views[x.slot], h = handles.current[x.slot];
      if (Date.now() - x.at > 15000) return false; // never settled: let it go
      if (!h || !v?.ready || v.locked || (x.afterClose && (v.open || h.running()))) return true; // not settled yet: keep it
      // its adapter (port and model) changed meanwhile: leave the view as it is
      const pid = devices.find((d) => devKey(d) === v.selKey)?.pid ?? null;
      if (x.key && (v.selKey !== x.key || (x.pid !== null && pid !== x.pid))) return false;
      const args = { ...x.args };
      // the span as it was: gapped only if it was before the map (else one the adapter now shows only
      // gapped, e.g. replugged at USB 2 meanwhile, is not asked for); said explicitly either way, so no
      // draft of a gapped sweep step carries over
      if (args.rate_sel !== undefined && !x.gappedOk && !h.rateOk(args.rate_sel)) delete args.rate_sel;
      if (x.args?.rate_sel !== undefined) args.allow_gapped = args.rate_sel !== undefined && x.gappedOk; // (an item without a span leaves it alone)
      if (Object.keys(args).length) h.linkedSet(args); // running: retuned; stopped: its next start
      if (x.start && !h.running()) h.startWith(args);
      return false;
    });
  }, [views, mode, restoreKick, devices]);
  // the close waits for a Start still running (the engine's queue is serial); on failure the view stays
  // (its recording note may come after the reply: it goes to a shown view for a few seconds more)
  const stopAndEnter = (to: Mode, slot: number) => {
    setConfirm(null);
    leaving.current++;
    const from = mode;
    hub
      .request('close', { slot })
      .then(() => now.current.mode === from && enter(to, slot)) // unless the user picked a mode meanwhile
      .catch((e: Error) => handles.current[slot]?.notice(`Could not stop view ${SLOT_NAME[slot]}: ${e.message}`))
      .finally(() => window.setTimeout(() => leaving.current--, 5000));
  };
  // n steps of the selected view along the tuning grid (5 MHz; at the 2.4 GHz edges and channel 14,
  // the next centre)
  const stepSel = (n: number) => {
    const h = handles.current[sel];
    if (!h) return;
    if (views[sel]?.locked) return h.notice(LOCKED);
    let f = h.freq();
    for (let i = 0; i < Math.abs(n); i++) {
      let next = f;
      for (let d = 5e6; next === f && d <= 15e6; d += 5e6) next = snap(f + Math.sign(n) * d);
      f = next;
    }
    h.tune(f);
  };
  const setUserScale = (sc: Scale) => {
    const dbMin = Math.min(sc.dbMin, sc.dbMax - 10);
    const n = { dbMin, dbMax: Math.max(sc.dbMax, dbMin + 10), wfFloor: sc.wfFloor };
    setScale(n);
    updatePrefs({ scale: n }); // a scale the user chose survives restarts (no automatic refit)
  };

  // Map: the sweeping view's adapter; Start opens it (when stopped) and sweeps, Stop ends both
  const mapDev = inMap ? devices.find((d) => devKey(d) === views[mapSlot]?.selKey) : undefined;
  // a remembered 2.4 GHz sweep on an adapter without that band (8812EU): all of 5 GHz instead
  const mapRange: Range = prefs.mapRange === '2.4' && !bandsOf(mapDev).includes('2.4') ? '5' : prefs.mapRange;
  const mapPlan = inMap ? sweepPlan(mapRange, mapDev, prefs.mapGapped) : null;
  const selDev = devices.find((d) => devKey(d) === views[sel]?.selKey); // the band ruler's bands
  // Start opens the adapter on the sweep's first step (not on a draft of an earlier sweep), after a
  // Stop's close if one is still in flight; Stop closes it, opening or open.
  const mapStart = () => {
    const v = views[mapSlot];
    if (v?.recording) return handles.current[mapSlot]?.notice('End the recording first: the map retunes this adapter');
    setMapRun(true);
    setMapNote(null);
    const seq = mapSeq.current;
    const first = mapPlan ? { freq: mapPlan.centres[0], rate_sel: mapPlan.rate_sel, wide: true, allow_gapped: mapPlan.gapped } : {};
    const open = () => {
      if (seq !== mapSeq.current) return; // stopped or left meanwhile (each bumps mapSeq)
      const h = handles.current[mapSlot];
      if (h && !h.running()) h.startWith(first);
    };
    if (mapClose.current) void mapClose.current.finally(open);
    else open();
  };
  const mapStop = () => {
    setMapRun(false);
    mapSeq.current++;
    if (handles.current[mapSlot]?.running()) {
      const p: Promise<unknown> = hub
        .request('close', { slot: mapSlot })
        .catch(() => {})
        .finally(() => {
          if (mapClose.current === p) mapClose.current = null;
        });
      mapClose.current = p;
    }
  };
  const mapToggle = () => (mapRun ? mapStop() : mapDev?.can_open && mapStart());
  // keyboard shortcuts (see KEYS)
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.defaultPrevented || e.altKey || e.metaKey) return; // e.g. the focused ruler took the arrows
      const t = e.target as HTMLElement;
      const h = handles.current[sel];
      const k = e.key;
      const typing = !!t.closest('input, select, textarea, [contenteditable="true"]');
      // held keys repeat only tuning and span steps (not Start/Stop, Record, the toggles)
      if (e.repeat && !['ArrowLeft', 'ArrowRight', '[', ']'].includes(k)) return;
      if (confirm && k === 'Escape') return setConfirm(null);
      if (help) {
        // the sheet is modal: it takes only its own keys
        if (k === 'Escape' || k === '?') {
          e.preventDefault();
          setHelp(false);
        }
        return;
      }
      if (e.ctrlKey) {
        if (k.toLowerCase() === 'b' && !e.shiftKey) toggleSidebar();
        else if (k.toLowerCase() === 'r' && !e.shiftKey && !typing && !inMap) h?.toggleRec(); // no recording in a sweep
        else return;
        e.preventDefault();
        return;
      }
      if (typing) return; // typing there
      const onButton = !!t.closest('button');
      if (inMap) {
        // the sweep tunes: Space starts or stops it
        if (k === ' ' && !onButton) mapToggle();
        else if (k === '?') setHelp((v) => !v);
        else return;
        e.preventDefault();
        return;
      }
      if (k === 'ArrowLeft' || k === 'ArrowRight') stepSel((k === 'ArrowRight' ? 1 : -1) * (e.shiftKey ? 4 : 1));
      else if (k === '[' || k === ']') h?.spanStep(k === ']' ? 1 : -1);
      else if (dual && (k === 'a' || k === 'b' || k === 'A' || k === 'B')) setSelSlot(k.toLowerCase() === 'a' ? 0 : 1);
      else if (/^[0-9]$/.test(k)) h?.typeFreq(k);
      else if (k === 'Enter' && !onButton) h?.typeFreq('');
      else if (k === ' ' && !onButton) h?.toggleRun();
      else if ((k === 'l' || k === 'L') && dual) setLock('freq');
      else if ((k === 'o' || k === 'O') && mode === 'compare') updatePrefs({ ghost: !prefs.ghost });
      else if (k === '?') setHelp((v) => !v);
      else return;
      e.preventDefault();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  });
  const vs = views[sel];
  const fsNow = vs?.fsNow ?? 5e6;
  const gapped = vs?.gapped ?? false;
  const fft = vs?.fft ?? prefs.fft;
  const dist = views[0] && views[1] ? Math.round((views[1].freq - views[0].freq) / 1e6) : null;
  const distLabel = dist === null ? '' : dist === 0 ? ' · B = A' : dist > 0 ? ` · B = A + ${dist}` : ` · B = A − ${-dist}`;
  const anyLock = locks.freq || locks.span || locks.gain;
  // the links, as switches: what each keeps the same between A and B
  const spans = views[0] && views[1] ? [views[0].fsNow, views[1].fsNow] : null;
  const links: [keyof Locks, string, string, string][] = [
    ['freq', 'Frequency', distLabel ? distLabel.slice(3) : 'keeps the distance', 'Retuning either view retunes the other by the same amount, keeping their distance'],
    ['span', 'Span', spans && spans[0] === spans[1] ? `both ${spanLabel(spans[0])}` : 'the same span', 'Both views show the same span (where both adapters show it continuously)'],
    ['gain', 'Gain', 'gain and AGC together', "Gain and AGC change together (two dongles' gains differ slightly)"],
  ];
  const linksBox = dual && (
    <section className="sbgroup links" aria-label="Links between A and B">
      <h3>
        Links<small>{anyLock ? 'moving either view moves both' : 'the views move independently'}</small>
      </h3>
      {links.map(([k, label, what, title]) => (
        <button key={k} className={locks[k] ? 'toggle on' : 'toggle'} role="switch" aria-checked={locks[k]} onClick={() => setLock(k)} title={title}>
          <span className="tlabel">
            {label}
            <small>{locks[k] ? what : 'not linked'}</small>
          </span>
          <span className="switch" aria-hidden="true">
            <i />
          </span>
        </button>
      ))}
    </section>
  );
  const auto = prefs.scale === null;
  // Panorama: views far apart (more than two spans)
  const [va, vb] = views;
  const gap = va && vb ? Math.max(va.src.lo, vb.src.lo) - Math.min(va.src.hi, vb.src.hi) : 0;
  const far = !!va && !!vb && gap > 2 * Math.max(va.fsNow, vb.fsNow);
  // a stopped view far from a running one stays off the axis (it would squash the running one's part)
  const offAxis = far ? views.findIndex((v, i) => v && !v.open && views[i ^ 1]?.open) : -1;
  const panoSources = views.map((v, i) => (i === offAxis ? null : v?.src ?? null));
  // (views far apart both running: the plot breaks its axis between them)
  const panoNote = views.some((v) => v?.open) ? null : 'Stopped. Press ▶ Start in a view’s header.';

  return (
    <div className={`app mode-${mode}`}>
      <header className="topbar">
        <button ref={toggleRef} className="icon" aria-label={sidebarShown ? 'Hide the sidebar (Ctrl+B)' : 'Show the sidebar (Ctrl+B)'} title="Sidebar (Ctrl+B)" onClick={toggleSidebar}>
          <svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
            <rect x="3" y="4" width="18" height="16" rx="2" />
            <line x1="9" y1="4" x2="9" y2="20" />
          </svg>
        </button>
        <span className="brand">wsdr-studio</span>
        <div className="seg modes" role="group" aria-label="Mode">
          {MODES.map(([m, label, hint]) => {
            const off = m !== mode && (isDual(m) ? devices.length < 2 : m === 'map' && !devices.length);
            return (
              <button key={m} className={m === mode ? 'on' : ''} aria-pressed={m === mode} disabled={off} title={off ? (m === 'map' ? 'Needs an adapter' : 'Needs a second adapter') : hint} onClick={() => setMode(m)}>
                {label}
              </button>
            );
          })}
        </div>
        <span className="hint">{MODES.find((x) => x[0] === mode)![2]}</span>
        <span className="spacer" />
        <button className="icon" aria-label="Keyboard shortcuts (?)" title="Keyboard shortcuts (?)" aria-expanded={help} onClick={() => setHelp((v) => !v)}>
          ?
        </button>
        {confirm && (
          <div className="confirm" role="alertdialog" aria-label="Leave the two-view mode">
            View {SLOT_NAME[confirm.slot]} is {views[confirm.slot]?.recording ? 'recording' : views[confirm.slot]?.open ? 'running' : 'starting'}.{' '}
            {confirm.to === 'map' ? 'Map stops it (it starts again when you leave Map)' : 'Single stops it'}
            {views[confirm.slot]?.recording ? ' and saves the recording' : ''}.
            <button onClick={() => stopAndEnter(confirm.to, confirm.slot)}>Stop {SLOT_NAME[confirm.slot]}</button>
            <button autoFocus onClick={() => setConfirm(null)}>
              Cancel
            </button>
          </div>
        )}
      </header>
      {help && (
        <div className="modal-bg" onClick={() => setHelp(false)}>
          <div className="sheet" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts" onClick={(e) => e.stopPropagation()}>
            <h2>Keyboard</h2>
            <dl>
              {KEYS.filter(([, , modes]) => modes.includes(mode)).map(([keys, what]) => (
                <div key={keys + what}>
                  <dt>{keys}</dt>
                  <dd>{what}</dd>
                </div>
              ))}
            </dl>
            <p className="note">{inMap ? 'Keys act on the map' : `Keys act on the selected view${dual ? ' (the coloured header)' : ''}`}; not while typing in a field.</p>
            <button autoFocus onClick={() => setHelp(false)}>
              Close
            </button>
          </div>
        </div>
      )}
      <div className="body">
        {sidebarShown && (
          <>
            {narrow && <div className="backdrop" onClick={() => setDrawer(false)} />}
            <aside
              ref={asideRef}
              className={narrow ? 'sidebar drawer' : 'sidebar'}
              {...(narrow ? { role: 'dialog', 'aria-modal': true, 'aria-label': 'Settings' } : { 'aria-label': 'Settings' })}
            >
              {dual && (
                <div className="editing">
                  <span>Editing</span>
                  <div className="seg wide" role="group" aria-label="Editing view">
                    {[0, 1].map((i) => (
                      <button key={i} className={`s${i}${sel === i ? ' on' : ''}`} aria-pressed={sel === i} onClick={() => setSelSlot(i)}>
                        {SLOT_NAME[i]}
                      </button>
                    ))}
                  </div>
                </div>
              )}
              {inMap && (
                <section className="sbgroup" aria-label="Sweep">
                  <h3>Sweep</h3>
                  <div className="rbands" role="group" aria-label="Channels swept">
                    {(['2.4', '5', 'u12', 'u2e', 'u3'] as Range[]).filter((r) => r !== '2.4' || bandsOf(mapDev).includes('2.4')).map((r) => (
                      <button key={r} className={r === mapRange ? 'on' : ''} aria-pressed={r === mapRange} onClick={() => updatePrefs({ mapRange: r })} title={RANGES[r].title}>
                        {RANGES[r].label}
                      </button>
                    ))}
                  </div>
                  <div className="kv">
                    <span>Listen</span>
                    <Seg
                      wide
                      value={String(prefs.mapDwell)}
                      options={[['120', '120 ms'], ['250', '250 ms'], ['1000', '1 s']]}
                      title="How long each step is heard: Wi-Fi beacons come every ~102 ms"
                      onChange={(v) => updatePrefs({ mapDwell: Number(v) })}
                    />
                  </div>
                  {mapDev && (mapDev.gapped_msps ?? 0) > Math.min(40, mapDev.max_msps) && (
                    <button
                      className={prefs.mapGapped ? 'toggle on' : 'toggle'}
                      role="switch"
                      aria-checked={prefs.mapGapped}
                      onClick={() => updatePrefs({ mapGapped: !prefs.mapGapped })}
                      title={`This adapter shows ${mapDev.gapped_msps} MHz only in short slices (gapped): fewer steps, but only part of the time is sampled`}
                    >
                      <span className="tlabel">
                        Gapped {mapDev.gapped_msps} MHz steps
                        <small>
                          {prefs.mapGapped
                            ? `faster, ~${Math.round(gappedDuty((mapDev.gapped_msps ?? 0) * 1e6, mapDev.speed_mbps >= 5000) * 100)} % sampled`
                            : `continuous ${Math.min(40, mapDev.max_msps)} MHz steps`}
                        </small>
                      </span>
                      <span className="switch" aria-hidden="true">
                        <i />
                      </span>
                    </button>
                  )}
                  {mapPlan && (
                    <span className="note">
                      {mapPlan.centres.length} steps of {mapPlan.fs / 1e6} MHz. Each: retune ~120 ms, then listen{' '}
                      {prefs.mapDwell >= 1000 ? '1 s' : `${prefs.mapDwell} ms`}. Leaving Map puts view {SLOT_NAME[mapSlot]} back where it was.
                    </span>
                  )}
                </section>
              )}
              <div className="sbviews" ref={setSbEl} />
              <section className="sbgroup" aria-label="Display">
                <h3>
                  Display{dual && <small>both views</small>}
                </h3>
                <button
                  className={auto ? 'toggle on' : 'toggle'}
                  role="switch"
                  aria-checked={auto}
                  onClick={() => {
                    if (auto) updatePrefs({ scale }); // keep the scale it has now
                    else {
                      fit();
                      updatePrefs({ scale: null });
                    }
                  }}
                  title="On: the scale refits after Start, retune, span, gain or FFT changes. Typing a value turns it off."
                >
                  <span className="tlabel">
                    Auto scale
                    <small>{auto ? 'fits the signal' : 'fixed (type values below)'}</small>
                  </span>
                  <span className="switch" aria-hidden="true">
                    <i />
                  </span>
                </button>
                <div className="kv">
                  <span>Scale</span>
                  <span className="row">
                    <NumField label="Scale bottom, dBFS" value={scale.dbMin} title="Bottom of the scale (dBFS); Enter to apply" onCommit={(v) => setUserScale({ ...scale, dbMin: v })} />
                    <span className="muted">to</span>
                    <NumField label="Scale top, dBFS" value={scale.dbMax} title="Top of the scale (dBFS); Enter to apply" onCommit={(v) => setUserScale({ ...scale, dbMax: v })} />
                    <span className="muted">dBFS</span>
                  </span>
                  <span>Waterfall from</span>
                  <span className="row">
                    <NumField label="Waterfall floor, dBFS" value={scale.wfFloor} title="The waterfall's colours start here (dBFS)" onCommit={(v) => setUserScale({ ...scale, wfFloor: v })} />
                    <span className="muted">dBFS</span>
                  </span>
                </div>
                {!inMap && mode !== 'panorama' && (
                  <button
                    className={prefs.wifiMarks ? 'toggle on' : 'toggle'}
                    role="switch"
                    aria-checked={prefs.wifiMarks}
                    onClick={() => updatePrefs({ wifiMarks: !prefs.wifiMarks })}
                    title="Finds Wi-Fi frames (802.11a/g/n/ac, and 802.11b at 2.4 GHz) and other signals (Bluetooth, other narrowband bursts, a microwave oven) in the samples themselves (continuous 20 and 40 MHz spans) and marks each on the waterfall; hover the waterfall for what they were."
                  >
                    <span className="tlabel">
                      Mark signals
                      <small>
                        {views[sel] && (views[sel]!.gapped || (views[sel]!.fsNow !== 20e6 && views[sel]!.fsNow !== 40e6)) ? (
                          'needs a continuous 20 or 40 MHz span'
                        ) : (
                          <>
                            <i className="wkey" style={{ background: WIFI_COLOUR.L }} /> legacy <i className="wkey" style={{ background: WIFI_COLOUR.H }} /> 11n{' '}
                            <i className="wkey" style={{ background: WIFI_COLOUR.V }} /> 11ac{' '}
                            <i className="wkey" style={{ background: WIFI_COLOUR.X }} /> 11ax/be{' '}
                            <i className="wkey" style={{ background: WIFI_COLOUR.B }} /> 11b{' '}
                            <i className="wkey" style={{ background: OTHER_COLOUR }} /> other
                          </>
                        )}
                      </small>
                    </span>
                    <span className="switch" aria-hidden="true">
                      <i />
                    </span>
                  </button>
                )}
                <div className="kv">
                  <span>{inMap ? 'Map shows' : 'Waterfall'}</span>
                  <Seg
                    wide
                    value={prefs.wfMode}
                    options={[['max', 'Peaks'], ['avg', 'Mean']]}
                    title="What the waterfall's colours show: the peaks of each row (short frames stand out) or its mean (what stays)"
                    onChange={(m) => {
                      updatePrefs({ wfMode: m });
                      if (auto) fit(m); // the floor depends on the mode
                    }}
                  />
                  {!inMap && ( // the map keeps peaks per listen; nothing to smooth
                    <>
                      <span>Smoothing</span>
                      <Seg
                        wide
                        value={String(prefs.smooth)}
                        options={[['0', 'Off'], ['0.5', 'Low'], ['0.7', 'Mid'], ['0.9', 'High']]}
                        title="Averaging of the average trace across rows"
                        onChange={(v) => updatePrefs({ smooth: Number(v) })}
                      />
                    </>
                  )}
                  <span>FFT size</span>
                  <select
                    aria-label="FFT size"
                    title="Bins per row; the kHz is the width of a bin (above 4096 the noise gets smoother, the bins no narrower)"
                    value={fft}
                    onChange={(e) => updatePrefs({ fft: Number(e.target.value) })}>
                    {FFTS.map((n) => (
                      <option key={n} value={n} disabled={gapped && n > 8192}>
                        {n <= 4096 ? `${n} · ${(fsNow / n / 1e3).toFixed(1)} kHz` : `${n} · smoother`}
                        {gapped && n > 8192 ? ' (not gapped)' : ''}
                      </option>
                    ))}
                  </select>
                </div>
              </section>
            </aside>
          </>
        )}
        <main className="workspace">
          <BandRuler
            views={
              inMap
                ? mapStep
                  ? [{ slot: mapSlot, lo: mapStep.c - mapStep.fs / 2, hi: mapStep.c + mapStep.fs / 2, locked: false, gapped: !!mapPlan?.gapped }]
                  : []
                : views.flatMap((v, i) =>
                    v && (i === 0 || dual) ? [{ slot: i, lo: v.centre - v.fsNow / 2, hi: v.centre + v.fsNow / 2, locked: v.locked, gapped: v.gapped }] : [],
                  )
            }
            range={inMap ? mapRange : undefined}
            bands={bandsOf(inMap ? mapDev : selDev)}
            sel={sel}
            compact={mode === 'compare'}
            survey={surveyMap}
            nets={nets}
            linkLabel={dual && locks.freq && distLabel ? distLabel.slice(3) : null}
            otherBusy={dual && views[sel ^ 1]?.recording ? `${SLOT_NAME[sel ^ 1]} paused: recording` : null}
            onTune={(f) => (inMap ? pickFromMap(f) : handles.current[sel]?.tune(f))}
            onStep={inMap ? () => {} : stepSel}
            onSelect={setSelSlot}
          />
          <div className={`views ${mode}`}>
            {[0, 1]
              .filter((i) => (inMap ? i === mapSlot || i === mapOther : i === 0 || dual))
              .map((i) => (
                <Station
                  key={i}
                  slot={i}
                  mode={mode}
                  hub={hub}
                  shared={{ prefs, updatePrefs, scale, setScale }}
                  link={link}
                  selected={sel === i}
                  onSelect={() => setSelSlot(i)}
                  sidebar={sbEl}
                  linksBox={linksBox}
                  otherSel={dual ? views[i ^ 1]?.selKey ?? null : null}
                  otherOpen={openKey[i ^ 1]}
                  otherWaiting={i === 1 && !!(views[0]?.lost || views[0]?.locked)}
                />
              ))}
            {inMap && (
              <SpectralMap
                key={`${mapSlot}:${mapRange}:${prefs.mapDwell}:${planId(mapPlan)}`}
                hub={hub}
                slot={mapSlot}
                range={mapRange}
                dev={mapDev}
                open={!!views[mapSlot]?.open}
                opening={!!views[mapSlot]?.locked && !views[mapSlot]?.open}
                running={mapRun}
                dwell={prefs.mapDwell}
                gappedOk={prefs.mapGapped}
                prefs={prefs}
                updatePrefs={updatePrefs}
                scale={scale}
                setScale={setScale}
                onStart={mapStart}
                onStop={mapStop}
                onPick={pickFromMap}
                onStep={(c, fs) => setMapStep(c === null ? null : { c, fs })}
                onFail={(why) => {
                  setMapNote(`The sweep stopped: ${why}`);
                  mapStop();
                }}
                note={mapNote}
                lost={!!views[mapSlot]?.lost}
              />
            )}
            {mode === 'panorama' && (
              <Panorama
                hub={hub}
                prefs={prefs}
                updatePrefs={updatePrefs}
                scale={scale}
                sources={panoSources}
                offAxis={offAxis >= 0 ? `${SLOT_NAME[offAxis]} is stopped at ${Math.round((views[offAxis]?.freq ?? 0) / 1e6)} MHz, off this axis` : null}
                overlay={panoNote}
                onRetune={(f) => {
                  // the view whose part this is (or the nearer one) moves its centre there
                  // as the plot assigns pixels: a running view's part over a stopped one's, then the nearer centre
                  const d = views.map((v) =>
                    v
                      ? (f >= v.src.lo && f <= v.src.hi ? 0 : 1e13 + Math.min(Math.abs(f - v.src.lo), Math.abs(f - v.src.hi))) +
                        (v.open ? 0 : 1e12) +
                        Math.abs(f - (v.src.lo + v.src.hi) / 2)
                      : Infinity,
                  );
                  const slot = d[1] < d[0] ? 1 : 0;
                  if (views[slot]?.locked) handles.current[slot]?.notice('End the recording to change tuning');
                  else handles.current[slot]?.tune(f);
                }}
              />
            )}
          </div>
        </main>
      </div>
    </div>
  );
}
