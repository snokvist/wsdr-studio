// SPDX-License-Identifier: GPL-2.0-only
// Vendor protocols in Wi-Fi management frames (the backend's src/vendor_frames.h): the name each is
// sent by, the label it is shown with. A new protocol (Remote ID, say) adds its entry here; the frame
// inspector's payload panel (Payload) shows any of them.
import type { VendorInfo } from '../protocol';

const LABEL: Record<string, string> = {
  espnow: 'ESP-NOW',
};

export const vendorLabel = (v: VendorInfo) => LABEL[v.name] ?? v.name;

export const BROADCAST = 'ff:ff:ff:ff:ff:ff';

// The payload as hex and text, 16 bytes a line: "0000  57 53 44 52 ...  WSDR...."
export function hexDump(hex: string): string[] {
  const lines: string[] = [];
  for (let o = 0; o < hex.length / 2; o += 16) {
    const bytes: number[] = [];
    for (let k = o; k < Math.min(o + 16, hex.length / 2); k++) bytes.push(parseInt(hex.slice(2 * k, 2 * k + 2), 16));
    const h = bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ').padEnd(47, ' ');
    const t = bytes.map((b) => (b >= 0x20 && b <= 0x7e ? String.fromCharCode(b) : '.')).join('');
    lines.push(`${o.toString(16).padStart(4, '0')}  ${h}  ${t}`);
  }
  return lines;
}
