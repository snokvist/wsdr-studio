// SPDX-License-Identifier: GPL-2.0-only
// Taps (src/tap.h): decoders in the backend that follow one signal and send what they make as binary
// frames. Each kind has its magic here (the first 4 bytes of its frames; byte 6 is the view slot, bytes
// 8-11 the watch they are of) and its panel (TapPanel). A new kind (an FM/AM audio receiver, say) adds
// its magic and a panel.

export type TapKindName = 'video';

export type TapFrame = { kind: TapKindName; slot: number; watch: number; buf: ArrayBuffer };

const MAGIC: Record<number, TapKindName> = {
  0x46565357: 'video', // 'WSVF' (src/video.h)
};

export function tapFrame(buf: ArrayBuffer): TapFrame | null {
  if (buf.byteLength < 12) return null;
  const v = new DataView(buf);
  const kind = MAGIC[v.getUint32(0, true)];
  return kind ? { kind, slot: v.getUint8(6), watch: v.getUint32(8, true), buf } : null;
}

// The backend's status of a tap ({"type":"tap"}, src/tap.h)
export type TapStatus = {
  kind: TapKindName;
  on: boolean;
  watch?: number; // which watch it is of (the 'tap' command's reply names it)
  freq?: number;
  params?: Record<string, unknown>;
  state?: string; // 'span' | 'outside' | the kind's ('searching', 'locked', ...)
  dropped?: number;
  [k: string]: unknown; // the kind's own fields
};

// What the user asked a view to follow
export type TapWatch = { kind: TapKindName; freq: number; params?: Record<string, unknown> };

// What a kind's panel (TapPanel) gets: the watch it shows, the backend's latest status of it, the view's
// handoff of the tap's frames (the panel gives it its sink), and close (it stops the watch)
export type TapPanelProps = {
  watch: TapWatch;
  status: TapStatus | null;
  frames: (sink: ((f: TapFrame) => void) | null) => void;
  onClose: () => void;
};
