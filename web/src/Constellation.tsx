// SPDX-License-Identifier: GPL-2.0-only
// The frame inspector: one Wi-Fi frame's equalised subcarriers as a constellation (L-SIG, the HT/VHT
// signal fields, a legacy frame's first payload symbols), with its EVM, SNR and carrier offset.
import { useEffect, useRef, useState } from 'react';
import type { FramePoints } from './protocol';

const FORMAT: Record<string, string> = { L: 'Legacy OFDM', H: '802.11n (HT)', V: '802.11ac (VHT)', B: '802.11b' };
const MOD: Record<number, string> = { 1: 'BPSK', 2: 'QPSK', 4: '16-QAM', 6: '64-QAM' };
const R = 1.6; // the plot spans -R..R on both axes (unit mean power: 64-QAM's outer levels sit at 1.08)
type Group = 'lsig' | 'sig2' | 'data';
const COLOUR: Record<Group, string> = { lsig: '#e0b46a', sig2: '#f48fb1', data: '#dfe6ee' };

// The ideal points of a constellation at unit mean power (bits per subcarrier; 1 BPSK)
function ideal(bits: number): [number, number][] {
  if (bits === 1) return [[-1, 0], [1, 0]];
  const top = bits === 2 ? 1 : bits === 4 ? 3 : 7, k = 1 / Math.sqrt(bits === 2 ? 2 : bits === 4 ? 10 : 42);
  const out: [number, number][] = [];
  for (let i = -top; i <= top; i += 2) for (let q = -top; q <= top; q += 2) out.push([i * k, q * k]);
  return out;
}

// (which side the panel sits on, for this window: it covers marks on its side)
let side: 'right' | 'left' = 'right';

export function Constellation(p: { data: FramePoints | null; error: string | null; onClose: () => void }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const [hide, setHide] = useState<Record<Group, boolean>>({ lsig: false, sig2: false, data: false });
  const [left, setLeft] = useState(side === 'left');
  const d = p.data;

  useEffect(() => {
    const key = (e: KeyboardEvent) => e.key === 'Escape' && !e.defaultPrevented && p.onClose(); // (a dialog took it)
    window.addEventListener('keydown', key);
    return () => window.removeEventListener('keydown', key);
  }, [p.onClose]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const cv = ref.current;
    if (!cv) return;
    const dpr = window.devicePixelRatio || 1, css = cv.clientWidth;
    cv.width = cv.height = Math.round(css * dpr);
    const c = cv.getContext('2d')!;
    const n = cv.width, px = (v: number) => ((v + R) / (2 * R)) * n, py = (v: number) => ((R - v) / (2 * R)) * n;
    c.fillStyle = '#0b0e13';
    c.fillRect(0, 0, n, n);
    c.strokeStyle = '#233041';
    c.lineWidth = dpr;
    c.beginPath();
    c.moveTo(px(-R), py(0));
    c.lineTo(px(R), py(0));
    c.moveTo(px(0), py(-R));
    c.lineTo(px(0), py(R));
    c.stroke();
    if (!d) return;
    // where the points belong: the payload's constellation, else BPSK (and QBPSK for the HT/VHT fields)
    const targets = d.bits ? ideal(d.bits) : [...ideal(1), ...(d.n_sig2 ? ([[0, -1], [0, 1]] as [number, number][]) : [])];
    c.strokeStyle = '#5a6b80';
    for (const [i, q] of targets) {
      c.beginPath();
      c.arc(px(i), py(q), 3.5 * dpr, 0, 2 * Math.PI);
      c.stroke();
    }
    const groups: [Group, number, number][] = [
      ['lsig', 0, d.n_lsig],
      ['sig2', d.n_lsig, d.n_lsig + d.n_sig2],
      ['data', d.n_lsig + d.n_sig2, d.z.length / 2],
    ];
    const r = Math.max(1, 1.2 * dpr);
    for (const [g, a, b] of groups) {
      if (hide[g]) continue;
      c.fillStyle = COLOUR[g];
      c.globalAlpha = g === 'data' && b - a > 600 ? 0.6 : 0.85;
      for (let k = a; k < b; k++) {
        const i = d.z[2 * k] / d.scale, q = d.z[2 * k + 1] / d.scale;
        if (Math.abs(i) <= R && Math.abs(q) <= R) c.fillRect(px(i) - r, py(q) - r, 2 * r, 2 * r);
      }
    }
    c.globalAlpha = 1;
  }, [d, hide]);

  const nData = d ? d.z.length / 2 - d.n_lsig - d.n_sig2 : 0;
  const chip = (g: Group, label: string, count: number) =>
    count > 0 && (
      <button
        className={hide[g] ? 'mini off' : 'mini on'}
        aria-pressed={!hide[g]}
        onClick={() => setHide((h) => ({ ...h, [g]: !h[g] }))}
        title={`Show or hide the ${label} points`}
      >
        <i style={{ background: COLOUR[g] }} /> {label} · {count}
      </button>
    );
  return (
    <aside className={left ? 'inspector left' : 'inspector'} aria-label="Frame inspector">
      <header>
        <b>Frame constellation</b>
        <span className="spacer" />
        <button
          className="x"
          onClick={() => {
            side = left ? 'right' : 'left';
            setLeft(!left);
          }}
          title={`Move to the ${left ? 'right' : 'left'} (to reach the marks under it)`}
          aria-label={left ? 'Move the panel right' : 'Move the panel left'}
        >
          {left ? '⇥' : '⇤'}
        </button>
        <button className="x" onClick={p.onClose} title="Close (Esc)" aria-label="Close">
          ×
        </button>
      </header>
      {p.error ? (
        <p className="note">{p.error}</p>
      ) : !d ? (
        <p className="note">Loading…</p>
      ) : d.fmt === 'B' ? (
        <p className="note">An 802.11b frame ({d.mhz.toFixed(0)} MHz): DSSS carries no OFDM subcarriers to show.</p>
      ) : (
        <>
          <canvas ref={ref} className="iq" />
          <div className="legend">
            {chip('lsig', 'L-SIG', d.n_lsig)}
            {chip('sig2', d.fmt === 'V' ? 'VHT-SIG-A' : 'HT-SIG', d.n_sig2)}
            {chip('data', `payload${d.bits ? ` ${MOD[d.bits]}` : ''}`, nData)}
          </div>
          <dl className="kv">
            <dt>Frame</dt>
            <dd>
              {FORMAT[d.fmt] ?? d.fmt} · {d.mhz.toFixed(0)} MHz{d.bw > 20 ? ` (${d.bw} MHz)` : ''} · {d.len} bytes
              {d.fmt === 'L' ? ` at ${d.rate} Mb/s` : ''}
            </dd>
            {d.ta && (
              <>
                <dt>From</dt>
                <dd>
                  {d.ta}
                  {d.ssid !== undefined ? ` · ${d.ssid || '(hidden)'}` : ''}
                </dd>
              </>
            )}
            <dt>EVM</dt>
            <dd>
              {d.evm_db !== null && <span className="evm">{d.evm_db.toFixed(1)} dB payload</span>}
              <span className="evm">{d.evm_lsig_db.toFixed(1)} dB L-SIG</span>
            </dd>
            <dt>SNR</dt>
            <dd>{d.snr.toFixed(1)} dB (training fields)</dd>
            <dt>Carrier</dt>
            <dd title="The sender's carrier against this adapter's tuning: both crystals' errors together">
              {d.cfo_hz >= 0 ? '+' : '−'}
              {(Math.abs(d.cfo_hz) / 1e3).toFixed(1)} kHz ({d.cfo_ppm >= 0 ? '+' : '−'}
              {Math.abs(d.cfo_ppm).toFixed(1)} ppm)
            </dd>
          </dl>
          <p className="note">
            {d.fmt === 'L'
              ? `Payload: its first ${nData / 48} OFDM symbol${nData === 48 ? '' : 's'}. `
              : 'The HT/VHT payload itself is not shown yet. '}
            EVM is against the nearest ideal point and counts everything on the way: the sender, the path and this
            receiver. Strong frames reading worse than weaker ones means the gain is too high for them.
          </p>
        </>
      )}
    </aside>
  );
}
