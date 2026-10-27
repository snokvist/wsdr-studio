// SPDX-License-Identifier: GPL-2.0-only
// Channel tables for the header and the spectrum markers: the Wi-Fi channels, plus 68-96 between
// UNII-2 and UNII-2e (no Wi-Fi there, but the radio receives and the band ruler has cells for them).

export type Channel = { label: string; ch: number; width: 20 | 40; centre: number; band: '2.4' | '5' };

// a channel's centre frequency (Hz)
export const centre = (ch: number) => (ch <= 14 ? (ch === 14 ? 2484 : 2407 + 5 * ch) : 5000 + 5 * ch) * 1e6;
const mhz = centre;

const five20 = [
  ...[36, 40, 44, 48, 52, 56, 60, 64],
  ...[68, 72, 76, 80, 84, 88, 92, 96],
  ...[100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144],
  ...[149, 153, 157, 161, 165, 169, 173, 177],
];
const five40 = [36, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 132, 140, 149, 157, 165, 173];

export const CHANNELS: Channel[] = [
  ...Array.from({ length: 13 }, (_, i) => ({ label: `${i + 1}`, ch: i + 1, width: 20 as const, centre: mhz(i + 1), band: '2.4' as const })),
  ...five20.map((c) => ({ label: `${c}`, ch: c, width: 20 as const, centre: mhz(c), band: '5' as const })),
  ...five40.map((c) => ({ label: `${c}+${c + 4}`, ch: c, width: 40 as const, centre: mhz(c) + 10e6, band: '5' as const })),
];

// 20 MHz channels whose centre lies in [lo, hi] (for markers).
export function channelsIn(lo: number, hi: number): Channel[] {
  return CHANNELS.filter((c) => c.width === 20 && c.centre >= lo && c.centre <= hi);
}

// The picker entry for a centre frequency, preferring the given width (a 20 MHz channel can be
// viewed with a 40 MHz span and the other way round).
export function channelAt(freq: number, width: number): Channel | undefined {
  const at = CHANNELS.filter((c) => Math.abs(c.centre - freq) < 1e3);
  return at.find((c) => c.width === width) ?? at[0];
}

