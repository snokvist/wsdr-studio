// SPDX-License-Identifier: GPL-2.0-only
// A vendor protocol's frame in the frame inspector: what it is, where it went, its payload as hex and
// text (shown as text: nothing in it is markup).
import type { VendorInfo } from '../protocol';
import { BROADCAST, hexDump, vendorLabel } from './index';

export function Payload({ v }: { v: VendorInfo }) {
  const lines = v.hex ? hexDump(v.hex) : [];
  return (
    <>
      <dl className="kv">
        <dt>Protocol</dt>
        <dd>
          {vendorLabel(v)}
          {v.enc ? ' (encrypted)' : ` v${v.version}`}
        </dd>
        <dt>To</dt>
        <dd>{v.to === BROADCAST ? 'broadcast' : v.to}</dd>
        {v.seq >= 0 && (
          <>
            <dt>Sequence</dt>
            <dd>
              {v.seq}
              {v.retry ? ' · retry' : ''}
            </dd>
          </>
        )}
        {!v.enc && (
          <>
            <dt>Payload</dt>
            <dd>
              {v.len} byte{v.len === 1 ? '' : 's'}
            </dd>
          </>
        )}
      </dl>
      {v.enc ? (
        <p className="note">
          Its payload is encrypted (CCMP). It is taken for {vendorLabel(v)} by its addressing alone: an encrypted action frame
          to one station with a broadcast third address.
        </p>
      ) : (
        lines.length > 0 && <pre className="hexdump">{lines.join('\n')}</pre>
      )}
    </>
  );
}
