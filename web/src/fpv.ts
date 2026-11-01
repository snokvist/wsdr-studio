// SPDX-License-Identifier: GPL-2.0-only
// Analog video (the "A" mark: an FM video signal, as an analog FPV drone camera's transmitter or a
// video sender sends it): its standard from its line rate, and the analog FPV channels near its
// frequency, as transmitters and goggles name them.

// MHz by channel 1..8 (the usual VTX tables: A, B and E (Boscam), F (Fatshark), R (Raceband), L (Lowrace))
export const FPV_BANDS: Record<string, readonly number[]> = {
  A: [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725],
  B: [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
  E: [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945],
  F: [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880],
  R: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
  L: [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621],
};

// The channels within `tol` MHz of `mhz`, nearest first, at most `max` (e.g. ["R5 5806", "A4 5805"]). The
// mark's frequency is the signal's mean, which the picture shifts by a MHz or two: a guide, not a match.
export function fpvChannels(mhz: number, tol = 3, max = 2): string[] {
  const near: [number, string][] = [];
  for (const [band, chans] of Object.entries(FPV_BANDS))
    chans.forEach((f, i) => {
      if (Math.abs(f - mhz) <= tol) near.push([Math.abs(f - mhz), `${band}${i + 1} ${f}`]);
    });
  return near.sort((a, b) => a[0] - b[0] || a[1].localeCompare(b[1])).slice(0, max).map(([, n]) => n);
}

// The standard of a line rate (kHz): 625 lines at 25 frames/s, or 525 at 29.97 (or 30)
export function videoStandard(lineKhz: number): string {
  if (Math.abs(lineKhz - 15.625) < 0.03) return 'PAL/SECAM, 625 lines';
  if (Math.abs(lineKhz - 15.734) < 0.03) return 'NTSC, 525 lines';
  return `${lineKhz.toFixed(3)} kHz lines`;
}

// The hover's text for it: "analog video, FM (NTSC, 525 lines) · ≈5806 MHz, FPV R5 5806 or A4 5805 ·
// deviation 8 MHz p-p" (swing: sync tip to white)
export function videoText(mhz: number, lineKhz: number, swingMhz: number): string {
  const ch = fpvChannels(mhz);
  return `analog video, FM (${videoStandard(lineKhz)}) · ≈${mhz.toFixed(0)} MHz${ch.length ? `, FPV ${ch.join(' or ')}` : ''}` +
    ` · deviation ${swingMhz.toFixed(0)} MHz p-p`;
}
