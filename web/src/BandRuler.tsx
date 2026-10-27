// SPDX-License-Identifier: GPL-2.0-only
// The band ruler: one range at a time at full width (2.4 GHz, a 5 GHz section, or all of 5 GHz; it
// follows the selected view), a cell per channel (click: tune the selected view there), a bracket per
// view at the span it shows (its tab selects the view), and a survey bar: the strongest signal seen on
// each channel this session. 5 GHz channels are equally wide, laid out by channel, not by MHz: the gaps
// between the UNII groups shrink to short breaks, except 68-96 (cells marked outside Wi-Fi).
import { useEffect, useLayoutEffect, useRef, useState, type KeyboardEvent, type MouseEvent, type WheelEvent } from 'react';
import { AXES, RANGES, freqAt, pos, rangeOf, type Range } from './bands';
import { centre } from './channels';
import { level, type Survey } from './survey';
import { SLOT_NAME, VIEW_COLOUR, bandOf, mhz } from './util';

const pc = (x: number) => `${(x * 100).toFixed(3)}%`;
const bar = (lv: number) => ({ opacity: (0.25 + 0.75 * lv).toFixed(2), width: `${Math.round(30 + 70 * lv)}%` });
const WHEEL_PX = 50; // wheel distance per 5 MHz step (a notch is ~50-120 px; trackpads send many small ones)
const WHEEL_MAX = 4; // steps per burst at most
const LABEL_PX = 26; // room a channel number needs
const FREQ_PX = 60;  // room for the channel's MHz under it too

export type RulerView = { slot: number; lo: number; hi: number; locked: boolean; gapped: boolean };

export function BandRuler(p: {
  views: RulerView[]; sel: number; compact: boolean; survey: Survey; linkLabel: string | null; otherBusy: string | null;
  nets: Map<number, string[]>; // network names heard on each channel (from beacons)
  onTune: (f: number) => void; onStep: (steps: number) => void; onSelect: (slot: number) => void;
  range?: Range; // Map: the range swept (its buttons are in the sidebar; the cells line up with the map)
}) {
  const wheel = useRef({ px: 0, t: 0 });
  const track = useRef<HTMLDivElement>(null);
  const [trackPx, setTrackPx] = useState(800);
  // the survey grows in place: redraw it once a second (not the whole window)
  const [, setTick] = useState(0);
  useEffect(() => {
    const t = window.setInterval(() => setTick((x) => x + 1), 1000);
    const w = wheel.current;
    return () => {
      window.clearInterval(t);
      window.clearTimeout(w.t);
    };
  }, []);
  useLayoutEffect(() => {
    const el = track.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setTrackPx(el.clientWidth));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  const selView = p.views.find((v) => v.slot === p.sel);
  const selLocked = p.range ? false : selView?.locked ?? true; // Map: a cell click leaves the map (always allowed)
  // the range shown: the selected view's section, unless the user picked another (until that view
  // moves to another section)
  const selRange = selView ? rangeOf((selView.lo + selView.hi) / 2) : 'u12';
  const [picked, setPicked] = useState<Range | null>(null);
  const inRange = (r: Range, v: RulerView) => {
    const c = (v.lo + v.hi) / 2;
    return r === '5' ? bandOf(c) === '5' : rangeOf(c) === r;
  };
  // forgotten when another view is selected, or when the selected view moves where the pick does not
  // show it (the 5 GHz overview stays while it steps across sections); state adjusted in render
  const [seen, setSeen] = useState(`${p.sel}:${selRange}`);
  if (seen !== `${p.sel}:${selRange}`) {
    setSeen(`${p.sel}:${selRange}`);
    if (!seen.startsWith(`${p.sel}:`) || (picked && selView && !inRange(picked, selView))) setPicked(null);
  }
  const band: Range = p.range ?? picked ?? selRange;
  const showBand = (b: Range) => setPicked(b === selRange ? null : b);
  const shows = (v: RulerView) => inRange(band, v);

  const onWheel = (e: WheelEvent) => {
    if (selLocked || !e.deltaY) return;
    const w = wheel.current;
    w.px += e.deltaY * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1);
    window.clearTimeout(w.t);
    w.t = window.setTimeout(() => {
      // one retune for a burst (down: lower); a short flick is still one step
      const n = Math.max(-WHEEL_MAX, Math.min(WHEEL_MAX, -Math.trunc(w.px / WHEEL_PX) || -Math.sign(w.px)));
      w.px = 0;
      p.onStep(n);
    }, 150);
  };
  const onKey = (e: KeyboardEvent) => {
    if (selLocked || (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight')) return;
    e.preventDefault();
    p.onStep((e.key === 'ArrowRight' ? 1 : -1) * (e.shiftKey ? 4 : 1));
  };
  const at = (e: MouseEvent<HTMLDivElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    return freqAt(band, (e.clientX - r.left) / r.width);
  };
  const { units } = AXES[band];
  const cellPx = trackPx / units;
  // label every channel when they fit, else every 2nd or 4th (2.4 GHz: 1, 6, 11 when tight)
  const every = band === '2.4' ? (cellPx >= LABEL_PX * 0.8 ? 1 : 5) : cellPx >= LABEL_PX ? 1 : cellPx * 2 >= LABEL_PX ? 2 : 4;
  const others = p.views.filter((v) => !shows(v));

  return (
    <div
      className={p.range ? 'ruler map' : p.compact ? 'ruler compact' : 'ruler'}
      onWheel={onWheel}
      onKeyDown={onKey}
      tabIndex={0}
      role="group"
      aria-label="Band ruler. Left and right arrows tune the selected view by 5 MHz (Shift: 20 MHz)"
    >
      {p.range ? (
        <div className="rgutter" />
      ) : (
        <div className="rbands" role="group" aria-label="Channels shown">
          {(['2.4', '5', 'u12', 'u2e', 'u3'] as Range[]).map((b) => (
            <button key={b} className={b === band ? 'on' : ''} aria-pressed={b === band} onClick={() => showBand(b)} title={RANGES[b].title}>
              {RANGES[b].label}
            </button>
          ))}
        </div>
      )}
      <div className="rtrack" ref={track} onClick={(e) => !selLocked && p.onTune(at(e))} title={selLocked ? undefined : 'Click to tune the selected view here'}>
        {RANGES[band].groups
          .filter((g) => g.name)
          .map((g) => {
            const left = pos(band, centre(g.chans[0]) - g.half);
            // a view's tab starting here (A's tabs sit at their bracket's left end): the name steps aside
            const tabHere = p.views.some((v) => v.slot === 0 && shows(v) && Math.abs(pos(band, v.lo) - left) * trackPx < 24);
            return (
              <span key={g.name} className="rgroup" style={{ left: pc(left), paddingLeft: tabHere ? 26 : undefined }}>
                {g.name}
              </span>
            );
          })}
        {RANGES[band].groups.flatMap((g) =>
          g.chans.map((c) => {
            const db = p.survey.get(c);
            const names = p.nets.get(c);
            const a = pos(band, centre(c) - g.half), b = pos(band, centre(c) + g.half);
            // which channels are numbered when there is no room for all: 1, 6, 11 / every 2nd or 4th from 36
            const shown = band === '2.4' ? every === 1 || [1, 6, 11].includes(c) : ((c - (c > 144 ? 149 : 36)) / 4) % every === 0;
            return (
              <button
                key={c}
                className={g.out ? 'rcell out' : 'rcell'}
                tabIndex={-1}
                disabled={selLocked}
                style={{ left: pc(a), width: pc(b - a) }}
                aria-label={`Tune to channel ${c}${g.out ? ', outside the Wi-Fi channels' : ''}`}
                title={`Channel ${c} · ${mhz(centre(c), 0)} MHz${g.out ? ' · outside the Wi-Fi channels' : ''}${db !== undefined ? ` · strongest seen: ${db.toFixed(0)} dB over the noise` : ''}${
                  names ? ` · networks: ${names.join(', ')}` : ''
                }`}
                onClick={(e) => {
                  e.stopPropagation();
                  p.onTune(centre(c));
                }}
              >
                <span>
                  {shown ? c : ''}
                  {cellPx >= FREQ_PX &&
                    (names ? (
                      <small className="rnet">{names.length > 1 ? `${names[0]} +${names.length - 1}` : names[0]}</small>
                    ) : (
                      <small>{mhz(centre(c), 0)}</small>
                    ))}
                </span>
                {db !== undefined && <i className="rbar" style={bar(level(db))} />}
              </button>
            );
          }),
        )}
        {p.views
          .filter(shows)
          .map((v) => {
            const a = pos(band, v.lo), b = pos(band, v.hi);
            const inset = p.compact && v.slot === 1 && p.views.length > 1; // Compare: B inside A
            const name = SLOT_NAME[v.slot];
            const where = `View ${name}: ${mhz(v.lo, 0)}–${mhz(v.hi, 0)} MHz${v.gapped ? ' (gapped)' : ''}`;
            return (
              <span key={v.slot}>
                <span
                  className={`rview s${v.slot}${v.gapped ? ' gapped' : ''}${v.slot === p.sel ? ' sel' : ''}${inset ? ' inset' : ''}`}
                  style={{ left: pc(a), width: `max(6px, ${pc(b - a)})`, borderColor: VIEW_COLOUR[v.slot] }}
                  title={where}
                />
                <button
                  className={`rtab s${v.slot}${v.slot === p.sel ? ' sel' : ''}`}
                  // A's tab at its bracket's left end, B's at its right end: they never sit on each other
                  style={v.slot === 1 ? { right: pc(1 - b) } : { left: pc(a) }}
                  aria-label={`${where}. Edit view ${name}`}
                  aria-pressed={v.slot === p.sel}
                  title={`${where} · click to edit view ${name}`}
                  onClick={(e) => {
                    e.stopPropagation();
                    p.onSelect(v.slot);
                  }}
                >
                  {name}
                </button>
              </span>
            );
          })}
      </div>
      {/* a view on the other band: a tab that shows that band */}
      {others.length > 0 && (
        <div className="rothers">
          {others.map((v) => (
            <button
              key={v.slot}
              className={`rtab s${v.slot}${v.slot === p.sel ? ' sel' : ''}`}
              title={`View ${SLOT_NAME[v.slot]} is at ${mhz((v.lo + v.hi) / 2, 0)} MHz: show those channels`}
              onClick={() => showBand(rangeOf((v.lo + v.hi) / 2))}
            >
              {SLOT_NAME[v.slot]} · {RANGES[rangeOf((v.lo + v.hi) / 2)].label} ▸
            </button>
          ))}
        </div>
      )}
      {!p.compact && (
        <div className="rfoot">
          {p.linkLabel && (
            <span className={p.otherBusy ? 'rlink paused' : 'rlink'}>
              {p.linkLabel}
              {p.otherBusy ? ` · ${p.otherBusy}` : ''}
            </span>
          )}
          <span className="spacer" />
          <span className="rkey" aria-hidden="true">
            quiet
            {[0, 0.5, 1].map((lv) => (
              <b key={lv}>
                <i className="rbar" style={bar(lv)} />
              </b>
            ))}
            strong
          </span>
          <span>strongest signal seen this session · wheel / arrow keys: 5 MHz</span>
        </div>
      )}
    </div>
  );
}
