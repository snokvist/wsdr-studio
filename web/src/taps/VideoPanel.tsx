// SPDX-License-Identifier: GPL-2.0-only
// The video tap's panel: the analog video transmitter's picture (a clicked analog video mark), with what
// the decoder found (its standard, colour, how many lines hold their sync).
import { useEffect, useRef, useState } from 'react';
import type { TapPanelProps } from './index';
import { decodeVideo, toRgba, type VideoField } from './video';

const STATE: Record<string, string> = {
  span: 'The picture needs a continuous 20 or 40 MS/s span (an 8812BU on USB 3). Pick one of those spans.',
  outside: 'The transmitter is outside the span: tune closer to it.',
  searching: 'Looking for the picture’s sync…',
};

export function VideoPanel(p: TapPanelProps) {
  const ref = useRef<HTMLCanvasElement>(null);
  const [big, setBig] = useState(false);

  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || e.defaultPrevented) return;
      e.preventDefault(); // (one Esc, one panel: the other view's stays)
      p.onClose();
    };
    window.addEventListener('keydown', key);
    return () => window.removeEventListener('keydown', key);
  }, [p.onClose]); // eslint-disable-line react-hooks/exhaustive-deps

  // the newest field, drawn once per screen refresh (fields that arrive in between are skipped)
  useEffect(() => {
    let next: VideoField | null = null, img: ImageData | null = null, raf = 0;
    p.frames((f) => {
      const v = decodeVideo(f.buf);
      if (v) next = v;
    });
    const draw = () => {
      raf = requestAnimationFrame(draw);
      const cv = ref.current, f = next;
      if (!cv || !f) return;
      next = null;
      if (cv.width !== f.width || cv.height !== f.height) {
        cv.width = f.width;
        cv.height = f.height;
      }
      const c = cv.getContext('2d');
      if (!c) return;
      if (!img || img.width !== f.width || img.height !== f.height) img = c.createImageData(f.width, f.height);
      toRgba(f, img.data);
      c.putImageData(img, 0, 0);
    };
    raf = requestAnimationFrame(draw);
    return () => {
      cancelAnimationFrame(raf);
      p.frames(null);
    };
  }, [p.frames]); // eslint-disable-line react-hooks/exhaustive-deps

  const s = p.status;
  const state = s?.on ? s.state ?? 'searching' : null;
  const locked = state === 'locked';
  const lock = typeof s?.lock === 'number' ? Math.round(s.lock * 100) : 0;
  const fields = typeof s?.fields === 'number' ? s.fields : 0;
  return (
    <aside className={`inspector video${big ? ' big' : ''}`} aria-label="Analog video">
      <header>
        <b>Analog video · {(p.watch.freq / 1e6).toFixed(0)} MHz</b>
        <span className="spacer" />
        <button className="x" onClick={() => setBig(!big)} title={big ? 'Smaller' : 'Larger (over the plots)'} aria-label={big ? 'Smaller' : 'Larger'}>
          {big ? '⤡' : '⤢'}
        </button>
        <button className="x" onClick={p.onClose} title="Close (Esc)" aria-label="Close">
          ×
        </button>
      </header>
      <canvas ref={ref} className={locked ? 'pic' : 'pic idle'} style={{ aspectRatio: '4 / 3' }} />
      {locked ? (
        <p className="note">
          {String(s?.std || '')} · {s?.colour ? 'colour' : 'black and white'} · {lock} % of lines locked · {fields.toFixed(0)} fields/s
          {s?.dropped ? ` · ${s.dropped} banks skipped (the decoder fell behind)` : ''}
        </p>
      ) : s?.state === 'error' ? (
        <p className="note">The decoder stopped: {String(s.error ?? 'error')}</p>
      ) : (
        <p className="note">{state ? STATE[state] ?? state : 'Starting…'}</p>
      )}
    </aside>
  );
}
