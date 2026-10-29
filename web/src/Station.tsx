// SPDX-License-Identifier: GPL-2.0-only
// One adapter's view (slot 0 = A, 1 = B): its header (frequency, Start/Stop, Record), its spectrum and
// waterfall, and, while it is the selected view, its sidebar sections (adapter, tuning, gain).
import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { channelAt } from './channels';
import { Display, type Source } from './display';
import type { Hub } from './hub';
import { Constellation } from './Constellation';
import type { Caps, Device, DspSettings, FramePoints, Rate, RxSettings, Row, Status, WifiFrame } from './protocol';
import { Seg } from './ui';
import {
  ANT_COLOUR, LOCKED, SLOT_NAME, UDEV_CMD, isDual, VIEW_COLOUR, bandOf, baseName, bytes, chanShort, contMsps, devKey, devLabel, devTitle, fmtClock,
  fmtDur, gappedDuty, hears, jaguar3, loadPrefs, mhz, ratesFor, snap, spanLabel, type Mode, type Prefs, type Scale, type Tuning,
} from './util';

type Banner = { id: number; text: string; kind: 'info' | 'error' };
type Saved = { file: string; bytes: number; seconds: number; reason?: string; error?: string };

// What the views share (display preferences, the scale) and how a view reaches the other (locks).
export type Shared = { prefs: Prefs; updatePrefs: (p: Partial<Prefs>) => void; scale: Scale; setScale: (s: Scale) => void };
export type StationHandle = {
  linkedSet: (args: Partial<RxSettings>) => void; // a change from the other view (no further forwarding)
  tune: (f: number) => void;                      // a user tune (the band ruler, keys): forwarded like one made here
  req: (cmd: string, args?: Record<string, unknown>) => Promise<unknown>;
  freq: () => number;
  rateOk: (rate_sel: number) => boolean;          // a continuous span on this adapter (a lock never makes one gapped)
  fs: () => number;                               // the span it shows (Hz)
  suggest: (wfMode?: 'max' | 'avg') => Scale | null; // Auto scale from this view's picture
  busy: () => string | null; // why a linked change must not touch this view now (recording, starting)
  notice: (text: string) => void;
  // keyboard shortcuts
  toggleRun: () => void;
  startWith: (args: Partial<RxSettings>) => void; // open with these settings over the draft (the map, a restore)
  running: () => boolean; // open or opening, as the events have said (not waiting for a render)
  toggleRec: () => void;
  spanStep: (dir: 1 | -1) => void;   // the next wider / narrower continuous span
  typeFreq: (digit: string) => void; // start typing a frequency in this view's header
};
// What a view tells the shell (sidebar summary, links, band ruler).
export type ViewInfo = {
  freq: number; fsNow: number; rate_sel: number; fft: number; gapped: boolean; gain_db: number; agc: boolean;
  allowGapped: boolean; // its span may be gapped (the user picked a gapped one)
  centre: number; // where the shown span is centred (the radio's centre while running, else the request)
  src: Source;    // its part of the stitched Panorama plot
  open: boolean; recording: boolean; locked: boolean; selKey: string | null;
  lost: boolean;  // unplugged, waiting for it
  ready: boolean; // connected, the adapters known, and a running view's settings in (alignment waits for it)
};
export type Link = {
  userSet: (slot: number, args: Partial<RxSettings>) => void;  // a user change: locked fields go to the other view
  register: (slot: number, h: StationHandle | null) => void;
  report: (slot: number, v: ViewInfo) => void;
  autoFit: () => void;                                         // refit the shared scale (both views' pictures)
  survey: (r: Row) => void;                                    // the band ruler's per-channel survey
  wifi: (frames: WifiFrame[]) => void;                         // networks heard (the band ruler names them)
};

export function Station(p: {
  slot: number; mode: Mode; hub: Hub; shared: Shared; link: Link; selected: boolean; onSelect: () => void;
  sidebar: HTMLElement | null; linksBox: ReactNode;
  otherSel: string | null; otherOpen: string | null; // the other view's adapter (picked, open)
  otherWaiting: boolean; // view A is restarting after an unplug: B waits its turn (they may want the same model)
}) {
  const { slot, mode, hub, shared, link, selected } = p;
  const dual = isDual(mode);
  const name = SLOT_NAME[slot];
  const spRef = useRef<HTMLCanvasElement>(null);
  const wfRef = useRef<HTMLCanvasElement>(null);
  const display = useRef<Display | null>(null);
  const freqFocused = useRef(false);
  const lastSentFreq = useRef<number | null>(null);
  const gainTimer = useRef<number | undefined>(undefined);
  const rate = useRef({ banks: 0, t: 0 });
  const drops = useRef({ ring: 0, client: 0, at: 0 });
  const bannerId = useRef(1);
  // for callbacks that outlive a render; want: a frequency sent and not yet acknowledged (a link reads it,
  // so quick changes in a row keep the views' distance)
  // rates: the open adapter's (caps), set as events arrive, so a handle used in the same commit is current
  const live = useRef({
    open: false, recording: false, opening: false, freq: 5180e6, rateSel: 4, want: null as number | null, wantId: 0, rates: null as Rate[] | null,
    wifi: false, // the backend finds Wi-Fi frames for this view
  });
  const autoOn = useRef(loadPrefs().scale === null); // Auto scale on (mirrors prefs.scale === null; storage may be blocked)
  const view = useRef({ ghost: false, other: p.otherSel, map: mode === 'map' }); // for the socket callbacks
  const [home, setHome] = useState('');
  const setRef = useRef<(a: Partial<RxSettings>) => void>(() => {});

  const { prefs, updatePrefs, scale } = shared;
  const [connected, setConnected] = useState(false);
  const [fatal, setFatal] = useState<string | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [selKey, setSelKey] = useState<string | null>(null);
  const [caps, setCaps] = useState<Caps>({ open: false });
  const [rx, setRx] = useState<RxSettings | null>(null); // live settings (adapter open)
  // the last tuning (remembered across launches) is the first draft
  const [draft, setDraft] = useState<RxSettings>(() => ({
    freq: 5180e6, rate_sel: 4, agc: false, gain_db: 30, ...(slot ? loadPrefs().tuningB : loadPrefs().tuning), wide: true,
  }));
  const [dsp, setDsp] = useState<DspSettings>({ fft: 8192, rows_per_s: 25, paths: 'a', dc_remove: true });
  const [status, setStatus] = useState<Status | null>(null);
  const [banners, setBanners] = useState<Banner[]>([]);
  const [saved, setSaved] = useState<Saved | null>(null);
  const [starting, setStarting] = useState(false);
  const startingRef = useRef(false); // the same, at once (a handle may be called before the next render)
  const [freqText, setFreqText] = useState(() => mhz(draft.freq, 3));
  const [gainUi, setGainUi] = useState<number | null>(null);
  const [msps, setMsps] = useState(0);
  const [dropWarn, setDropWarn] = useState<string[]>([]);
  // stream faults in the last minute (the core restarts the stream by itself; a fault long gone is
  // not shown, its count since Start would stay forever)
  const [recentFaults, setRecentFaults] = useState(0);
  const [wifiSkipped, setWifiSkipped] = useState(0); // when the Wi-Fi finder last fell behind (ms)
  // the radio's centre in status lags a retune (status comes every 0.5 s, and only once the new run
  // streams): until a status of the new run arrives, the request is shown as the centre
  const [tunedStale, setTunedStale] = useState(false);
  // the frame inspector: a clicked Wi-Fi mark's frame and, once the backend answered, its points
  const [inspect, setInspect] = useState<{ id: number; data: FramePoints | null; error: string | null } | null>(null);
  const closeInspect = useCallback(() => setInspect(null), []);
  const wantWifiRef = useRef(false); // (for the event handler: this window wants Wi-Fi frames found)
  const [hadPicture, setHadPicture] = useState(false);
  // unplugged while running: the adapter (pid, picker key) to start again when it is back
  // gone: a scan has shown it missing; before: the adapters there were when it was lost; tries: restarts
  // present: scans in a row that still listed it (a quick replug on the same port may never show it gone)
  // tune: what it ran when it was lost (a restart asks for that: the draft may since have been clamped
  // to what a USB 2 adapter carries, before Start switches it back to USB 3)
  const [lost, setLost] = useState<{
    pid: number; key: string; gone: boolean; present: number; before: string[]; tries: number; tune: Partial<RxSettings>;
  } | null>(null);
  const [gotDevices, setGotDevices] = useState(false);
  const scanSeen = useRef<Device[] | null>(null);
  const known = useRef<string[]>([]); // the adapter list's keys, for the callbacks
  const freqInput = useRef<HTMLInputElement>(null);

  const rememberTuning = (t: Tuning) => updatePrefs(slot ? { tuningB: t } : { tuning: t });
  const addBanner = (text: string, kind: Banner['kind'] = 'info') => {
    const id = bannerId.current++;
    setBanners((b) => [...b.filter((x) => x.text !== text), { id, text, kind }]);
    if (kind === 'info') window.setTimeout(() => setBanners((b) => b.filter((x) => x.id !== id)), 6000);
  };
  const dropBanner = (id: number) => setBanners((b) => b.filter((x) => x.id !== id));
  const req = (cmd: string, args: Record<string, unknown> = {}) =>
    hub.request(cmd, { ...args, slot }).catch((e: Error & { kind?: string }) => {
      addBanner(e.message, e.kind === 'busy' ? 'info' : 'error'); // "try again in a moment" is not a fault
      throw e;
    });

  // socket + display wiring (the effect owns the renderer and the subscription)
  useEffect(() => {
    const d = new Display(spRef.current!, wfRef.current!);
    display.current = d;
    const ro = new ResizeObserver(() => d.resize());
    ro.observe(spRef.current!);
    ro.observe(wfRef.current!);
    d.resize();
    // Auto refit: after Start; after an rx change once 25 rows of the NEW run arrived (rows of the old
    // run may still be in flight); after a display change (FFT) once 25 more rows arrived.
    // autoAt = the row count to refit at (-1: nothing pending); autoRun = the run count to wait for.
    let autoAt = autoOn.current ? 25 : -1;
    let autoRun = -1;
    const autoAgain = (rx: boolean) => {
      if (!autoOn.current) return;
      if (rx) {
        autoRun = d.runCount() + 1;
        autoAt = -1; // rows still counting toward an earlier trigger may be of the old run
      } else {
        d.resetPeaks();
        autoAt = d.rowsSeen() + 25;
      }
    };
    d.onHint = (t) => addBanner(t);
    d.onFrame = (id) => {
      setInspect({ id, data: null, error: null });
      hub
        .request('points', { slot, frame: id })
        .then((r: FramePoints) => setInspect((s) => (s?.id === id ? { id, data: r, error: null } : s)))
        .catch((e: Error) => setInspect((s) => (s?.id === id ? { id, data: null, error: e.message } : s)));
    };
    let wasOpen = false;
    let opened: { pid: number; key: string } | null = null; // the adapter of the current run
    let lastRx: RxSettings | null = null;
    let lastRestarts = 0;
    let wifiDropped = -1; // the backend's count is since it started: the first event is the baseline
    let firstDsp = true;
    let faultTimes: number[] = [];
    let lastPaths = 'a';
    let lastFft = 0;
    d.onRetune = (f) => {
      if (live.current.recording || live.current.opening) {
        if (live.current.recording) addBanner(LOCKED);
        return;
      }
      const t = snap(f); // the request stays; at a 40 MHz span the radio centres as near as it can
      if (Math.abs(t - live.current.freq) < 1) addBanner(`Already at the nearest step (${mhz(t, 0)} MHz). The radio tunes in 5 MHz steps.`);
      else setRef.current({ freq: t }); // live retune, or the draft for the next Start when stopped
      updatePrefs({ retuned: true });
    };
    const unsubscribe = hub.subscribe({
      slot,
      row: (r) => {
        d.push(r);
        if (r.seq % 5 === 0) link.survey(r);
        const runDone = autoRun >= 0 && d.runCount() >= autoRun && d.runRows() >= 25;
        const rowsDone = autoAt >= 0 && d.rowsSeen() >= autoAt;
        if (runDone || rowsDone) {
          if (runDone) autoRun = -1; // clear only the trigger that fired
          if (rowsDone) autoAt = -1;
          if (autoOn.current) link.autoFit(); // unless the user fixed a scale meanwhile
        }
      },
      other: (r) => view.current.ghost && d.setGhost(r),
      connection: (up) => {
        setConnected(up);
        if (!up) wasOpen = false; // the radio may be stopped and restarted elsewhere meanwhile: the next caps is a fresh Start
      },
      giveUp: (why) => setFatal(why),
      event: (e) => {
        switch (e.type) {
          case 'devices': {
            setDevices(e.devices);
            setGotDevices(true);
            known.current = e.devices.map(devKey);
            // keep the pick; else the first adapter the other view has not picked (nothing picked yet,
            // at a start in a two-view mode: B takes the second)
            // (view B picks nothing rather than A's adapter: a second Start there would only say "busy")
            const other = view.current.other;
            const free = other !== null ? e.devices.find((x: Device) => devKey(x) !== other) ?? (slot ? undefined : e.devices[0]) : e.devices[slot] ?? (slot ? undefined : e.devices[0]);
            setSelKey((k) => (k !== null && e.devices.some((x: Device) => devKey(x) === k) ? k : free ? devKey(free) : null));
            break;
          }
          case 'caps':
            // the picker shows the adapter that is open, whoever opened it
            if (e.open && e.bus !== undefined && e.bus >= 0) {
              setSelKey(`${e.bus}-${e.port}`);
              opened = { pid: e.pid, key: `${e.bus}-${e.port}` };
              setLost(null);
            }
            if (e.open && !wasOpen) {
              lastRx = null; // the new session's first settings are not a change (Auto refits on Start)
              setSaved(null); // also when another window or script pressed Start
              d.newStream();
              autoAt = autoOn.current ? 25 : -1;
              autoRun = -1;
            } else if (!e.open && wasOpen) {
              setHadPicture(d.hasPicture());
              d.freeze();
              if (lastRx) setDraft(lastRx); // keep the last settings for the next Start
              setRx(null);
            }
            wasOpen = e.open;
            live.current.open = e.open;
            live.current.rates = e.open ? e.rates ?? null : null;
            setCaps(e);
            break;
          case 'settings':
            if (e.rx) {
              if (lastRx && (lastRx.freq !== e.rx.freq || lastRx.rate_sel !== e.rx.rate_sel || lastRx.gain_db !== e.rx.gain_db || lastRx.agc !== e.rx.agc))
                autoAgain(true);
              // (the map's sweep steps are not the user's tuning)
              if (!view.current.map) {
                if (lastRx && lastRx.freq !== e.rx.freq) updatePrefs({ retuned: true });
                rememberTuning({ freq: e.rx.freq, rate_sel: e.rx.rate_sel, gain_db: e.rx.gain_db, agc: e.rx.agc });
              }
              if (lastRx && lastRx.freq !== e.rx.freq) setTunedStale(true);
              lastRx = e.rx;
              live.current.freq = e.rx.freq;
              live.current.rateSel = e.rx.rate_sel;
              setRx(e.rx);
              lastSentFreq.current = null;
              if (!freqFocused.current) setFreqText(mhz(e.rx.freq, 3));
              setGainUi(null);
            }
            if (e.dsp) {
              if (lastFft && e.dsp.fft !== lastFft && wasOpen) autoAgain(false); // stopped: keep the frozen picture
              lastFft = e.dsp.fft;
              if (e.dsp.paths !== lastPaths) {
                if (!e.dsp.paths.includes('a')) d.dropPath(0);
                if (!e.dsp.paths.includes('b')) d.dropPath(1);
                lastPaths = e.dsp.paths;
              }
              live.current.wifi = !!e.dsp.wifi;
              // the first settings after connecting: an open view may come up searching while this
              // window does not want it (or the other way round)
              if (firstDsp) {
                firstDsp = false;
                if (live.current.open && live.current.wifi !== wantWifiRef.current) req('dsp', { wifi: wantWifiRef.current }).catch(() => {});
              }
              setDsp(e.dsp);
            }
            break;
          case 'status': {
            setStatus(e);
            if (e.rx?.state === 'streaming' && !e.rx.settling) setTunedStale(false); // the new run's centre (settling: the old run, about to end)
            {
              const n = e.rx?.restarts ?? 0, now = Date.now();
              for (let k = lastRestarts; k < n; k++) faultTimes.push(now); // a new open starts again at 0
              lastRestarts = n;
              faultTimes = faultTimes.filter((t) => now - t < 60000);
              setRecentFaults(faultTimes.length);
            }
            if (e.record?.active && !live.current.recording) {
              window.clearTimeout(gainTimer.current); // controls lock now
              setSaved((s) => (s && s.file === e.record.file ? s : null)); // a new recording (from any window) replaces the old note
            }
            live.current.recording = e.record?.active ?? false;
            live.current.opening = e.state === 'opening';
            const banks = e.rx?.banks ?? 0;
            const r = rate.current;
            if (r.t && e.t > r.t && banks >= r.banks) setMsps(((banks - r.banks) * 8192) / (e.t - r.t) / 1e6);
            r.banks = banks;
            r.t = e.t;
            // drop counters are cumulative: warn only while they are still growing (last 5 s)
            const dr = drops.current;
            const ring = e.ring_dropped ?? 0, cl = e.client_drops ?? 0;
            if (ring > dr.ring || cl > dr.client) dr.at = e.t;
            const w: string[] = [];
            if (e.t - dr.at < 5 && dr.at) {
              if (ring > dr.ring) w.push('banks dropped (DSP behind)');
              if (cl > dr.client) w.push('display skipped rows (window too slow)');
            }
            if (w.length || e.t - dr.at >= 5) setDropWarn(w);
            dr.ring = ring;
            dr.client = cl;
            break;
          }
          case 'hello':
            setHome(e.home ?? '');
            break;
          case 'notice': // backend news for the user (e.g. the USB 3 switch)
            addBanner(String(e.text ?? ''));
            break;
          case 'wifi':
            d.addWifi(e.frames, e.other ?? [], e.id0, e.kept0);
            link.wifi(e.frames);
            if (wifiDropped >= 0 && e.dropped > wifiDropped) setWifiSkipped(Date.now()); // the backend fell behind: some samples not searched
            wifiDropped = e.dropped;
            break;
          case 'recorded':
            setSaved({ file: e.file, bytes: e.bytes, seconds: e.seconds, reason: e.reason, error: e.error });
            break;
          case 'alert':
            if (e.kind === 'lost' && opened)
              setLost({
                ...opened, gone: false, present: 0, before: known.current, tries: 0,
                tune: lastRx ? { freq: lastRx.freq, rate_sel: lastRx.rate_sel, wide: lastRx.wide } : {},
              }); // the header says it; no banner
            else addBanner(e.msg, 'error');
            break;
          case 'error':
            addBanner(e.msg ?? 'error', 'error');
            break;
        }
      },
    });
    return () => {
      unsubscribe();
      ro.disconnect();
      d.destroy();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Before Start the draft shows what Start opens: a span this adapter carries continuously. A
  // remembered span (a gapped one, or 40 MHz on an 8812CU) never opens gapped by itself; one the user
  // picked gapped in this session (allow_gapped) stays. (Every wider centre is also a narrower step.)
  const sel = devices.find((d) => devKey(d) === selKey);
  const selMax = sel ? contMsps(sel) : 40;
  const selGapped = sel?.gapped_msps ?? 0;
  useEffect(() => {
    const widest = Math.max(1, Math.ceil(Math.log2(80 / selMax))); // rate_sel of the widest continuous span
    const widestGapped = selGapped > 0 ? Math.max(0, Math.ceil(Math.log2(80 / selGapped))) : widest; // (80 MHz gapped: 8812BU only)
    setDraft((dr) => {
      const lo = dr.allow_gapped ? widestGapped : widest;
      return dr.rate_sel < lo ? { ...dr, rate_sel: lo } : dr;
    });
  }, [selMax, selGapped]);

  const open = caps.open;
  const opening = status?.state === 'opening' || starting;
  const cur = rx ?? draft; // what the controls show
  const rxState = open ? status?.rx?.state ?? 'starting' : 'stopped';
  const recording = status?.record.active ?? false;
  const locked = (open && recording) || opening; // recording: a change would end the file; opening: it would be lost
  const lockedWhy = opening && !recording ? 'Starting… wait a moment' : LOCKED;

  const fsNow = 80e6 / (1 << cur.rate_sel);
  const chan = channelAt(cur.freq, 20); // by centre frequency only (the span does not pick a channel)
  const rates: Rate[] = caps.rates ?? ratesFor(sel, prefs.offsetTuning);
  const usb3 = caps.superspeed ?? (sel?.speed_mbps ?? 0) >= 5000; // a gapped bank ships ~7x faster
  const contMax = Math.max(0, ...rates.filter((r) => !r.gapped).map((r) => r.fs)); // widest continuous span
  // the selected span (follows a change at once; the badge shows the run's measured share)
  const gapped = rates.find((r) => r.rate_sel === cur.rate_sel)?.gapped ?? false;
  const run = status?.rx;
  const hwFs = run?.hw_fs ?? run?.fs ?? 0;
  const gappedPct = run?.gapped && msps > 0 && hwFs ? Math.round(((msps * 1e6) / hwFs) * 100) : null;
  const tuned = tunedStale || run?.state !== 'streaming' ? cur.freq : run?.centre ?? 0;
  const chip = caps.chip ?? sel?.chip ?? '';
  const twoPaths = (caps.paths ?? 1) === 2;
  const ghost = mode === 'compare' && prefs.ghost; // the other view's average over this one
  useEffect(() => {
    live.current.opening = opening;
    live.current.freq = cur.freq;
    live.current.rateSel = cur.rate_sel;
  }, [opening, cur.freq, cur.rate_sel]);
  // stopped on an adapter the other view has just opened: take a free one
  useEffect(() => {
    if (open || opening || !p.otherOpen || selKey !== p.otherOpen) return;
    const free = devices.find((d) => devKey(d) !== p.otherOpen);
    if (free) setSelKey(devKey(free));
  }, [p.otherOpen, selKey, open, opening, devices]);
  useEffect(() => () => window.clearTimeout(gainTimer.current), []); // a gain change still waiting dies with the view
  useEffect(() => {
    autoOn.current = prefs.scale === null;
  }, [prefs.scale]);
  // Closing the window while recording: the shell asks first (Electron 'will-prevent-unload').
  useEffect(() => {
    if (!recording) return;
    const h = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener('beforeunload', h);
    return () => window.removeEventListener('beforeunload', h);
  }, [recording]);
  const offGrid = open && tuned > 0 && Math.abs(tuned - cur.freq) > 1e3;

  const overlay = fatal
    ? fatal
    : !connected
      ? 'Connecting to wsdr-studio…'
      : opening
        ? 'Starting the adapter… (about 5 s)'
        : open
          ? null
          : !devices.length || lost || (sel && !sel.can_open)
            ? null // a card says it
            : hadPicture
              ? 'Stopped. Double-click the spectrum to pick a frequency; ▶ Start to resume.'
              : 'Stopped. Press ▶ Start to begin.';
  const marker = open || hadPicture ? cur.freq : null;
  const markerLabel = open ? 'requested' : 'next Start';
  const chipNotch = jaguar3(chip); // 8822C and 8822E notch their centre in the gapped wide spans
  // the 8822B dips at its centre at 40 MHz on 2.4 GHz (measured: ~8-10 dB over +-1 MHz)
  const notchWide = open && (caps.chip ?? '').includes('8822B') && hwFs >= 40e6 && tuned > 0 && tuned < 3.7e9;
  useEffect(
    () =>
      display.current?.setOpts({
        ...scale, wfMode: prefs.wfMode, smooth: prefs.smooth, overlay, dim: !open,
        wfPath: dsp.paths === 'b' ? 1 : 0, marker, markerLabel, chipNotch, notchWide,
        colour: VIEW_COLOUR[slot], pathColours: twoPaths ? ANT_COLOUR : null, ghostColour: VIEW_COLOUR[slot ^ 1],
        showAvg: prefs.showAvg, showPeak: prefs.showPeak, wifi: prefs.wifiMarks,
        staleText: rxState === 'starting' ? 'Applying settings…' : 'No data from the adapter',
      }),
    [scale, prefs.wfMode, prefs.smooth, prefs.showAvg, prefs.showPeak, prefs.wifiMarks, overlay, open, dsp.paths, marker, markerLabel, chipNotch, notchWide, rxState, slot, twoPaths],
  );
  // Wi-Fi frames are found in the backend only while this view shows them (its own waterfall: not in
  // Panorama's stitched plot or the map)
  const wantWifi = prefs.wifiMarks && mode !== 'panorama' && mode !== 'map';
  wantWifiRef.current = wantWifi;
  // (sent when this window's wish changes, not when the setting does: two windows that disagree must
  // not keep switching it back and forth)
  useEffect(() => {
    if (connected && open && !!live.current.wifi !== wantWifi) req('dsp', { wifi: wantWifi }).catch(() => {});
  }, [wantWifi, connected, open]); // eslint-disable-line react-hooks/exhaustive-deps

  // A settings change: sent now when streaming, kept as a draft until Start otherwise.
  const set = (a: Partial<RxSettings>) => {
    // a span change is a continuous one unless it says otherwise (only the user's pick of a gapped span does)
    const args = a.rate_sel !== undefined && a.allow_gapped === undefined ? { ...a, allow_gapped: false } : a;
    if (!live.current.open) {
      const n = { ...draft, ...args };
      live.current.freq = n.freq;
      live.current.rateSel = n.rate_sel;
      setDraft(n);
      // waiting for an unplugged adapter: it starts again with this tuning, not the one it had
      const t: Partial<RxSettings> = {};
      if (args.freq !== undefined) t.freq = args.freq;
      if (args.rate_sel !== undefined) t.rate_sel = args.rate_sel;
      if (args.wide !== undefined) t.wide = args.wide;
      if (Object.keys(t).length) setLost((l) => l && { ...l, tune: { ...l.tune, ...t } });
      if (!view.current.map) rememberTuning({ freq: n.freq, rate_sel: n.rate_sel, gain_db: n.gain_db, agc: n.agc }); // remembered too
      if (args.freq !== undefined) setFreqText(mhz(args.freq, 3));
      return;
    }
    if (args.freq === undefined) {
      req('set', args).catch(() => {});
      return;
    }
    const id = ++live.current.wantId;
    live.current.want = args.freq;
    req('set', args)
      .catch(() => {})
      .finally(() => id === live.current.wantId && (live.current.want = null));
  };
  // A change the user made here: offered to the other view (it takes the linked fields; first, while
  // this view's frequency is still the old one, so the distance is kept), then applied.
  const userSet = (args: Partial<RxSettings>) => {
    if (args.freq !== undefined && !hears(sel, args.freq)) { // the 8812EU: no 2.4 GHz front end
      addBanner(`${sel?.model ?? 'This adapter'} receives 5 GHz only`);
      return;
    }
    link.userSet(slot, args);
    set(args);
  };
  const tuneTo = (f: number) => { // the ‹ › buttons, the ruler: same snapping as a typed frequency
    if (locked) return;
    const t = snap(f);
    setFreqText(mhz(t, 3));
    if (Math.abs(t - cur.freq) < 1) return;
    lastSentFreq.current = t;
    updatePrefs({ retuned: true });
    userSet({ freq: t });
  };
  useEffect(() => {
    setRef.current = userSet;
    view.current = { ghost, other: p.otherSel, map: mode === 'map' };
    if (!ghost) display.current?.setGhost(null);
    link.register(slot, {
      linkedSet: set, tune: tuneTo, req, freq: () => live.current.want ?? live.current.freq,
      rateOk: (r) => (live.current.rates ?? rates).some((x) => x.rate_sel === r && !x.gapped),
      fs: () => 80e6 / (1 << live.current.rateSel),
      suggest: (wfMode) => {
        if (wfMode) display.current?.setOpts({ wfMode });
        return display.current?.suggestRange() ?? null;
      },
      busy: () => (live.current.recording ? 'it is recording' : live.current.opening || startingRef.current ? 'it is starting' : null),
      notice: (text) => addBanner(text),
      running: () => live.current.open || live.current.opening || startingRef.current,
      startWith: (args) => {
        if (sel && sel.can_open && connected && !live.current.open && !live.current.opening && !startingRef.current) void doStart(sel, false, args);
      },
      toggleRun: () => {
        if (open) req('close').catch(() => {});
        else if (sel && sel.can_open && !opening && connected) void doStart();
      },
      toggleRec: () => {
        if (recording || (open && rxState === 'streaming' && !gapped && !runGapped)) void toggleRecord();
        else addBanner(gapped || runGapped ? 'Recording needs a continuous span' : 'Recording starts while streaming');
      },
      spanStep: (dir) => {
        if (locked) return addBanner(lockedWhy);
        // continuous spans only: a gapped one is picked in the Span list, on purpose (from a gapped
        // span, narrower is the widest continuous one)
        const ok = rates.filter((r) => !r.gapped).sort((a, b) => a.fs - b.fs);
        const i = ok.findIndex((r) => r.rate_sel === cur.rate_sel);
        const next = i < 0 ? (dir < 0 ? ok[ok.length - 1] : undefined) : ok[Math.max(0, Math.min(ok.length - 1, i + dir))];
        if (next && next.rate_sel !== cur.rate_sel) userSet({ rate_sel: next.rate_sel, wide: true });
      },
      typeFreq: (digit) => {
        if (locked) return addBanner(lockedWhy);
        if (digit) setFreqText(digit);
        freqInput.current?.focus();
        if (!digit) freqInput.current?.select(); // Enter: the field, its text selected
      },
    });
  });
  useEffect(() => () => link.register(slot, null), []); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    const centre = open && tuned > 0 ? tuned : cur.freq;
    const msg = fatal || !connected ? null : opening ? `${name}: starting…` : !open ? `${name} stopped` : rxState === 'starting' ? `${name}: applying settings…` : null;
    link.report(slot, {
      freq: cur.freq, fsNow, rate_sel: cur.rate_sel, fft: dsp.fft, gapped, allowGapped: !!cur.allow_gapped, gain_db: cur.gain_db, agc: cur.agc,
      centre, open, recording, locked, selKey, lost: !!lost,
      ready: connected && gotDevices && !opening && (!open || rx !== null),
      src: { lo: centre - fsNow / 2, hi: centre + fsNow / 2, colour: VIEW_COLOUR[slot], pathColours: twoPaths ? ANT_COLOUR : null, marker, markerLabel, chipNotch, notchWide, dim: !open, msg },
    });
  }, [cur.freq, fsNow, cur.rate_sel, dsp.fft, gapped, cur.allow_gapped, cur.gain_db, cur.agc, open, tuned, recording, locked, selKey, marker, markerLabel, chipNotch, notchWide, opening, rxState, fatal, connected, twoPaths, lost, gotDevices, rx]); // eslint-disable-line react-hooks/exhaustive-deps
  // the shared preferences reach this adapter: offset tuning when running, the FFT size at once
  useEffect(() => {
    if (caps.open) req('set', { offset_tuning: prefs.offsetTuning }).catch(() => {});
  }, [prefs.offsetTuning]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (connected && dsp.fft !== prefs.fft) req('dsp', { fft: prefs.fft }).catch(() => {});
  }, [prefs.fft, connected]); // eslint-disable-line react-hooks/exhaustive-deps

  // auto: starting again after an unplug; it waits on if this fails. It asks for the span it had, but
  // never gapped: a replugged 8812BU comes back at USB 2, Start switches it to USB 3 (40 MHz again),
  // and should it stay at USB 2 the receiver opens the widest continuous span instead.
  const doStart = async (dev = sel, auto = false, extra: Partial<RxSettings> = {}) => {
    if (!dev) return;
    if (!auto) setLost(null);
    startingRef.current = true;
    setStarting(true);
    setSaved(null);
    window.clearTimeout(gainTimer.current); // a gain change still waiting goes with the open
    const gain = gainUi ?? draft.gain_db;
    // an adapter without a 2.4 GHz front end (8812EU) starts on 5 GHz channel 36 instead of a 2.4 GHz draft
    const band = hears(dev, extra.freq ?? draft.freq) ? {} : { freq: 5180e6 };
    if (band.freq) addBanner(`${dev.model} receives 5 GHz only: starting on channel 36`);
    try {
      await req('open', {
        pid: dev.pid, bus: dev.bus, port: dev.port, ...draft, ...extra, ...band, gain_db: gain, offset_tuning: prefs.offsetTuning, ...(auto ? { allow_gapped: false } : {}),
      });
    } catch {
      /* shown */
    } finally {
      startingRef.current = false;
      setStarting(false);
    }
  };
  // Waiting for an adapter (none found: view A looks; or this one unplugged): look again every 3 s
  // (no hotplug events). The unplugged adapter starts again when it is back: on its port once a scan
  // showed it gone, or as an adapter of that model that was not there before (a replug can change the
  // bus); never one the other view has.
  const waiting = connected && !open && !opening && ((!devices.length && slot === 0) || !!lost);
  useEffect(() => {
    if (!waiting) return;
    const t = window.setInterval(() => hub.request('scan', { slot }).catch(() => {}), 3000);
    return () => window.clearInterval(t);
  }, [waiting, hub, slot]);
  // (tries are paced by the scans: a failed try waits for the next adapter list)
  useEffect(() => {
    if (!lost || open || opening || starting || !connected || p.otherWaiting) return;
    const fresh = scanSeen.current !== devices;
    scanSeen.current = devices;
    const keys = devices.map(devKey);
    if (!lost.gone && !keys.includes(lost.key)) return setLost({ ...lost, gone: true });
    const present = !lost.gone && fresh ? lost.present + 1 : lost.present;
    const sameBack = lost.gone || present >= 2; // its own port: once seen gone, or listed by two scans since
    if (lost.tries >= 3) return; // the card says to press Start
    const d = devices.find((x) => {
      const k = devKey(x);
      return x.pid === lost.pid && x.can_open && k !== p.otherOpen && k !== p.otherSel && (k === lost.key ? sameBack : !lost.before.includes(k));
    });
    if (!d) {
      if (present !== lost.present) setLost({ ...lost, present });
      return;
    }
    setSelKey(devKey(d));
    setLost({ ...lost, present, tries: lost.tries + 1 });
    void doStart(d, true, lost.tune);
  }, [devices, lost, p.otherWaiting]); // eslint-disable-line react-hooks/exhaustive-deps
  const commitFreq = () => {
    const f = Math.round(parseFloat(freqText) * 1e6);
    if (!Number.isFinite(f) || f < 2.3e9 || f > 6.2e9) {
      setFreqText(mhz(cur.freq, 3));
      if (freqText.trim()) addBanner('Frequency must be between 2300 and 6200 MHz'); // (a slip: not a lasting error)
      return;
    }
    const t = snap(f); // the radio tunes in 5 MHz steps
    if (t !== f) addBanner(`Tuned to ${mhz(t, 3)} MHz (the radio tunes in 5 MHz steps)`);
    setFreqText(mhz(t, 3));
    if (t === lastSentFreq.current || Math.abs(t - cur.freq) < 1) return; // Enter then blur: send once
    lastSentFreq.current = t;
    updatePrefs({ retuned: true });
    userSet({ freq: t });
  };
  const onGain = (v: number) => {
    setGainUi(v);
    window.clearTimeout(gainTimer.current);
    gainTimer.current = window.setTimeout(() => userSet({ gain_db: v }), 200);
  };
  const toggleRecord = async () => {
    try {
      await req('record', { action: recording ? 'stop' : 'start' });
      if (!recording) setSaved(null);
    } catch {
      /* shown */
    }
  };
  const setPaths = (paths: string) => paths !== dsp.paths && req('dsp', { paths }).catch(() => {});
  const copy = (text: string) => navigator.clipboard?.writeText(text).then(() => addBanner('Copied')).catch(() => {});

  // diagnostics: only when they are abnormal
  const st = status?.rx;
  const warn: string[] = [...dropWarn];
  if (open && st) {
    if (recentFaults > 0) warn.push(`stream restarted after a fault (${recentFaults}× in the last minute)`);
    if (Date.now() - wifiSkipped < 60000) warn.push('Wi-Fi marks: some samples were not searched (the frame finder could not keep up)');
    if (status!.dsp_load > 0.8) warn.push(`DSP ${(status!.dsp_load * 100).toFixed(0)} %`);
    if (rxState === 'streaming' && st.fs && !st.gapped && msps > 0 && msps < 0.95 * (st.fs / 1e6)) warn.push(`input ${msps.toFixed(2)} MS/s`);
  }
  if (st?.error) warn.push(st.error);
  // (unplugged: the card says it restarts by itself, not "press Start"; a failed Start is in its strip)
  if (!open && !lost && status?.error && !banners.some((b) => b.kind === 'error' && b.text === status.error)) warn.push(status.error);
  const dotClass =
    fatal || !connected ? 'dot bad'
    : opening || (rxState === 'starting' && !st?.error) ? 'dot warn' // a routine restart (retune, span) is not a fault
    : !open ? 'dot'
    : rxState === 'streaming' ? 'dot ok'
    : 'dot bad';
  const stateText = fatal ? 'disconnected' : !connected ? 'connecting…' : opening ? 'starting…' : open ? rxState : lost ? 'unplugged' : 'stopped';
  const recRate = fsNow * 4; // bytes/s (int16 I + Q)
  const diskFree = status?.disk_free ?? 0;
  const secsLeft = diskFree / recRate;
  const lockTitle = locked ? lockedWhy : undefined;
  const runGapped = !!run?.gapped;
  const recTitle = !recording && gapped
    ? `Recording needs a continuous span (${spanLabel(contMax)} or less on this adapter); gapped spans sample only part of the time`
    : !recording && rxState !== 'streaming'
      ? 'Available while streaming'
      : recording && status
        ? `Recording to ${status.record.file} (${bytes(recRate)}/s${diskFree ? `; ${bytes(diskFree)} free, ~${fmtDur(secsLeft)}` : ''}). Tuning and gain are locked meanwhile.`
        : `Record raw IQ (SigMF), about ${bytes(recRate)}/s at this span${diskFree ? `; ${bytes(diskFree)} free (~${fmtDur(secsLeft)})` : ''}`;

  // a setting that is on or off: a switch, with what it does under its name
  const toggle = (label: string, what: string, on: boolean, onChange: (v: boolean) => void, title: string, disabled = false) => (
    <button className={on ? 'toggle on' : 'toggle'} role="switch" aria-checked={on} disabled={disabled} onClick={() => onChange(!on)} title={title}>
      <span className="tlabel">
        {label}
        <small>{what}</small>
      </span>
      <span className="switch" aria-hidden="true">
        <i />
      </span>
    </button>
  );
  // The selected view's settings in the sidebar (frequency, channel, Start/Stop and Record are in its
  // header and the band ruler).
  const sidebar = (
    <>
      <section className="sbgroup" aria-label={`Adapter of view ${name}`}>
        <h3>Adapter</h3>
        <div className="row">
          <select
            aria-label={`Adapter of view ${name}`}
            value={selKey ?? ''}
            disabled={open || opening || !devices.length}
            onChange={(e) => {
              setSelKey(e.target.value);
              setLost(null); // another adapter: no longer waiting for the old one
            }}
            title={sel ? devTitle(sel) : 'Adapter'}
          >
            {!devices.length && <option value="">no adapter found</option>}
            {devices.map((d) => {
              const inOther = devKey(d) === p.otherOpen; // also in Single: view B may still be running
              return (
                <option key={devKey(d)} value={devKey(d)} title={devTitle(d)} disabled={inOther}>
                  {devLabel(d)}
                  {inOther ? ` (in view ${SLOT_NAME[slot ^ 1]})` : ''}
                </option>
              );
            })}
          </select>
          {!open && (
            <button className="icon" aria-label="Look for adapters again" disabled={opening || !connected} onClick={() => req('scan').catch(() => {})} title="Look for adapters again">
              ⟳
            </button>
          )}
        </div>
        {twoPaths && <Seg wide value={dsp.paths} disabled={locked} options={[['a', 'Ant 1'], ['b', 'Ant 2'], ['ab', 'Both']]} onChange={setPaths} title="Antennas" />}
        {sel && sel.driver && sel.driver !== 'usbfs' && !open && (
          <span className="note">The kernel driver <code>{sel.driver}</code> is using this adapter; ▶ Start takes it over.</span>
        )}
        {sel && sel.pid === 0xb812 && !open && sel.speed_mbps < 5000 && (
          <span className="note">Connected at USB 2: ▶ Start switches it to USB 3 when the port allows (continuous 10–40 MHz need USB 3).</span>
        )}
        {sel && sel.pid === 0xb812 && open && caps.superspeed === false && (
          <span className="note">At USB 2: 10–40 MHz only gapped. Use a USB 3 port and cable, then replug.</span>
        )}
        {(open || sel) && mode !== 'map' && (
          <span className="note" title={diskFree ? `${bytes(diskFree)} free where recordings go` : undefined}>
            {gapped
              ? `Recording needs a continuous span (${spanLabel(contMax)} or less here)`
              : `Recording: ${bytes(recRate)}/s${diskFree ? ` · room for ~${fmtDur(secsLeft)}` : ''}`}
          </span>
        )}
      </section>
      {/* Map: the sweep picks its own steps */}
      {mode !== 'map' && (
        <section className="sbgroup" title={lockTitle}>
          <h3>Span</h3>
          <select
            aria-label={`Span of view ${name}`}
            disabled={locked}
            value={cur.rate_sel}
            // the centre stays as asked; a 40 MHz span centres within 2422-2462 / 5090-6155 MHz, so the
            // radio may sit beside it (the plot marks the requested frequency)
            onChange={(e) => {
              const r = Number(e.target.value);
              userSet({ rate_sel: r, wide: true, allow_gapped: rates.some((x) => x.rate_sel === r && x.gapped) }); // picked: may be gapped
            }}
            title={`Bandwidth shown = sample rate (complex IQ). This adapter: up to ${spanLabel(Math.max(...rates.map((r) => r.fs)))}${
              rates.some((r) => r.gapped)
                ? '. "gapped": wider than the USB link can carry continuously; the adapter captures short slices (spectrum and waterfall, no recording)'
                : ''
            }`}
          >
            {rates.map((r) => (
              <option key={r.rate_sel} value={r.rate_sel}>
                {r.gapped ? `${spanLabel(r.fs)} · gapped ~${Math.round(gappedDuty(r.hw_fs ?? r.fs, usb3) * 100)} %` : `${spanLabel(r.fs)} · ${r.fs / 1e6} MS/s`}
              </option>
            ))}
          </select>
          {gapped && (
            <span className="note">
              Gapped: the adapter captures short slices of this span and ships each over USB, so only part of the time is
              seen. Spectrum and waterfall, no recording.
            </span>
          )}
          {fsNow >= 40e6 && bandOf(cur.freq) === '2.4' && (
            <span className="note">
              {fsNow >= 80e6 ? 'An 80 MHz span centres between 2442 and 2462 MHz' : 'A 40 MHz span centres between 2422 and 2462 MHz'}: the
              requested frequency is marked on the plot.
            </span>
          )}
          {jaguar3(chip) &&
            toggle(
              'Avoid the chip’s centre notch',
              prefs.offsetTuning ? 'tunes beside the channel' : 'shows what the chip sees',
              prefs.offsetTuning,
              (v) => updatePrefs({ offsetTuning: v }),
              "Off: the view is what the adapter's own receiver sees, its centre notch included. On: the 10 and 20 MHz views are cut from a 40 MHz capture tuned beside the channel, so the notch stays out of view (less of the time is sampled).",
            )}
        </section>
      )}
      <section className="sbgroup" title={lockTitle}>
        <h3>Gain</h3>
        <div className="row">
          <input
            type="range"
            aria-label={`Gain of view ${name}`}
            disabled={cur.agc || locked}
            min={0}
            max={caps.gain_max ?? 49}
            step={caps.gain_step ?? 1}
            value={gainUi ?? cur.gain_db}
            onChange={(e) => onGain(Number(e.target.value))}
          />
          <span className="val">{cur.agc ? 'auto' : `${(gainUi ?? cur.gain_db).toFixed(0)} dB`}</span>
          <button
            className={cur.agc ? 'mini on' : 'mini'}
            role="switch"
            aria-checked={cur.agc}
            disabled={locked}
            onClick={() => userSet({ agc: !cur.agc })}
            title="AGC: the chip sets the gain (it steps down during frames)"
          >
            AGC
            <span className="switch" aria-hidden="true">
              <i />
            </span>
          </button>
        </div>
      </section>
      {p.linksBox}
    </>
  );

  const legend: [string, string, boolean, () => void][] = [
    ['average', twoPaths ? '#a3aebb' : VIEW_COLOUR[slot], prefs.showAvg, () => updatePrefs({ showAvg: !prefs.showAvg })],
    ['peak', '#8d99a8', prefs.showPeak, () => updatePrefs({ showPeak: !prefs.showPeak })],
  ];
  if (mode === 'compare') legend.push([`${SLOT_NAME[slot ^ 1]} overlay`, VIEW_COLOUR[slot ^ 1], prefs.ghost, () => updatePrefs({ ghost: !prefs.ghost })]);
  if (twoPaths) {
    const toggle = (path: 'a' | 'b') => {
      const on = new Set(dsp.paths.split(''));
      if (on.has(path)) on.delete(path);
      else on.add(path);
      if (on.size) setPaths(on.has('a') && on.has('b') ? 'ab' : [...on][0]);
    };
    legend.push(['Ant 1', '#ffab70', dsp.paths.includes('a'), () => toggle('a')]);
    legend.push(['Ant 2', '#8fd694', dsp.paths.includes('b'), () => toggle('b')]);
  }

  return (
    <section className={`view s${slot}${dual && selected ? ' sel' : ''}${lost ? ' lost' : ''}`} onMouseDown={p.onSelect} onFocusCapture={p.onSelect} aria-label={`View ${name}`}>
      <header className="vhead">
        {dual && <span className={`slotchip s${slot}`}>{name}</span>}
        <div className="freqbox" title={lockTitle}>
          <button aria-label="Tune down 5 MHz (Shift: 20 MHz)" disabled={locked} onClick={(e) => tuneTo(cur.freq - (e.shiftKey ? 20e6 : 5e6))}>
            ‹
          </button>
          <input
            ref={freqInput}
            className="freq"
            aria-label={`Centre frequency of view ${name}, MHz`}
            disabled={locked}
            value={freqText}
            onFocus={() => {
              freqFocused.current = true;
              lastSentFreq.current = null;
            }}
            onChange={(e) => setFreqText(e.target.value)}
            onBlur={() => {
              freqFocused.current = false;
              commitFreq();
            }}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitFreq();
              else if (e.key === 'Escape') {
                setFreqText(mhz(cur.freq, 3)); // as it was; the blur then changes nothing
                freqInput.current?.blur();
              }
            }}
            title="Centre frequency in MHz (Enter to apply). The radio tunes in 5 MHz steps."
          />
          <button aria-label="Tune up 5 MHz (Shift: 20 MHz)" disabled={locked} onClick={(e) => tuneTo(cur.freq + (e.shiftKey ? 20e6 : 5e6))}>
            ›
          </button>
        </div>
        <span className="muted">MHz</span>
        <span className="muted chlabel">
          · {chan ? `${chanShort(chan)} · ` : ''}
          {spanLabel(fsNow)} span
        </span>
        {offGrid && (
          <span className="amber" title="At a 40 MHz span the radio centres within 2422–2462 / 5090–6155 MHz, or on its 5 MHz grid; the requested frequency is marked on the plot">
            <span className="long">requested {mhz(cur.freq, 0)} · </span>tuned {mhz(tuned, 0)}
            <span className="long"> ({Math.abs(tuned - cur.freq) >= 4.99e6 ? '40 MHz centres' : '5 MHz steps'})</span>
          </span>
        )}
        {runGapped && (
          <span
            className="badge"
            title={`Each slice is 8192 samples; the gaps between them are the time USB 2 needs to ship one${
              st?.offset && st.hw_centre ? `. Radio at ${mhz(st.hw_centre)} MHz (keeps the chip notch out of view)` : ''
            }`}
          >
            GAPPED<span className="long"> · {gappedPct !== null ? `~${gappedPct} %` : 'part of the time'} sampled</span>
          </span>
        )}
        <span className="spacer" />
        {/* Panorama: one legend on the stitched plot instead */}
        {mode !== 'panorama' && <div className="legend">
          {legend.map(([label, colour, on, fn]) => (
            <button key={label} className={on ? 'on' : 'off'} aria-pressed={on} aria-label={label} onClick={fn} title={`${on ? 'Hide' : 'Show'} ${label}${dual && !label.startsWith('Ant') ? ' (both views)' : ''}${
              label === 'peak' ? '. The strongest FFT frame of each row: a burst shorter than a frame reads lower when it straddles two' : ''
            }`}>
              <span className="swatch" style={{ background: colour }} />
              <span className="ltext">{label}</span>
            </button>
          ))}
        </div>}
        <span className="state" title={stateText}>
          <span className={dotClass} />
          <span className="stext">{stateText}</span>
        </span>
        {!open ? (
          <button disabled={!sel || opening || !connected || !sel.can_open} onClick={() => void doStart()}>
            {opening ? 'Starting…' : '▶ Start'}
          </button>
        ) : (
          <button onClick={() => req('close').catch(() => {})} title={recording ? 'Stop the radio (also ends and saves the recording)' : 'Stop the radio'}>
            ■ Stop
          </button>
        )}
        <button
          className={recording ? 'rec on' : 'rec'}
          disabled={!recording && (!open || rxState !== 'streaming' || gapped || runGapped)}
          onClick={toggleRecord}
          title={recTitle}
        >
          {recording && status
            ? (
                <>
                  ■ End rec {fmtClock(status.record.seconds)}
                  <span className="long"> · {bytes(status.record.bytes)}</span>
                </>
              )
            : gapped
              ? (
                  <>
                    ● Rec<span className="long"> · ≤ {spanLabel(contMax)}</span>
                  </>
                )
              : '● Rec'}
        </button>
      </header>

      <main className="plots">
        <div className="strips">
          {!recording && saved && (
            <div className={saved.error ? 'strip error' : 'strip'}>
              <span className="one-line" title={saved.file}>
                {saved.error ? `Recording stopped: ${saved.error}. ` : saved.reason ? `Recording stopped (${saved.reason}). ` : ''}
                Saved <code>{baseName(saved.file)}</code> · {bytes(saved.bytes)} · {saved.seconds.toFixed(1)} s · in{' '}
                {home && saved.file.startsWith(home + '/')
                  ? '~' + saved.file.slice(home.length, saved.file.lastIndexOf('/') + 1)
                  : saved.file.slice(0, saved.file.lastIndexOf('/') + 1)}
              </span>
              <button onClick={() => copy(saved.file)}>Copy path</button>
              <button className="x" onClick={() => setSaved(null)} title="Dismiss">
                ×
              </button>
            </div>
          )}
          {banners
            .filter((b) => b.kind === 'error')
            .map((b) => (
              <div key={b.id} className="strip error">
                {b.text}
                <button className="x" onClick={() => dropBanner(b.id)} title="Dismiss">
                  ×
                </button>
              </div>
            ))}
        </div>
        {connected && !fatal && !open && !opening && (lost ? (
          <div className="vcard lost" role="status">
            <h2>{lost.tries >= 3 ? 'Back, but it did not start' : 'Unplugged · waiting for it to come back'}</h2>
            <p>
              {lost.tries >= 3
                ? `Three tries to start view ${name} again failed. Press ▶ Start to try once more.`
                : `View ${name} starts again by itself when the adapter is back${saved ? ' (the recording was saved)' : ''}. The last picture stays, dimmed.`}
            </p>
            <button onClick={() => setLost(null)}>Stop waiting</button>
          </div>
        ) : !devices.length ? (
          <div className="vcard" role="status">
            <svg aria-hidden="true" width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#8e99a7" strokeWidth="1.6">
              <rect x="6" y="3" width="12" height="11" rx="2" />
              <path d="M9 14v4h6v-4" />
              <line x1="10" y1="21" x2="14" y2="21" />
            </svg>
            <h2>No adapter found</h2>
            <p>Plug in an RTL8812BU, RTL8812CU or RTL8812EU. It appears here by itself; Rescan looks again now.</p>
            <button onClick={() => req('scan').catch(() => {})}>⟳ Rescan</button>
          </div>
        ) : sel && !sel.can_open ? (
          <div className="vcard" role="status">
            <svg aria-hidden="true" width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#e0b46a" strokeWidth="1.6">
              <rect x="5" y="11" width="14" height="10" rx="2" />
              <path d="M8 11V8a4 4 0 0 1 8 0v3" />
            </svg>
            <h2>{sel.model} found, but this user cannot open it</h2>
            <p>Run this once in a terminal (it asks for your password), then unplug and replug the adapter:</p>
            <code className="cmd">{UDEV_CMD}</code>
            <button onClick={() => copy(UDEV_CMD)}>Copy command</button>
          </div>
        ) : null)}
        <div className="toasts">
          {banners
            .filter((b) => b.kind === 'info')
            .map((b) => (
              <div key={b.id} className="toast" onClick={() => dropBanner(b.id)}>
                {b.text}
              </div>
            ))}
        </div>
        <canvas ref={spRef} className="spectrum" />
        <canvas ref={wfRef} className="waterfall" />
        {inspect && <Constellation data={inspect.data} error={inspect.error} onClose={closeInspect} />}
      </main>

      {(warn.length > 0 || (!prefs.retuned && open && !recording) || (recording && secsLeft < 60)) && (
        <footer className="vfoot">
          {warn.map((w) => (
            <span
              key={w}
              className="warn"
              title={w.startsWith('input') ? 'The adapter delivers fewer samples than this span needs (USB could not keep up): the picture has gaps' : undefined}
            >
              {w}
            </span>
          ))}
          {recording && secsLeft < 60 && <span className="warn">disk nearly full: {fmtDur(secsLeft)} left</span>}
          {!prefs.retuned && open && !recording && <span className="hint">tip: double-click the spectrum to tune there</span>}
        </footer>
      )}
      {selected && p.sidebar && createPortal(sidebar, p.sidebar)}
    </section>
  );
}
