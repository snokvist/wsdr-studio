// SPDX-License-Identifier: GPL-2.0-only
// The video tap's frames (src/video.h): one field of analog video, Y then U and V at half size each way.

export type VideoField = {
  width: number; height: number; colour: boolean; pal: boolean; seq: number; t: number; lock: number;
  y: Uint8Array; u: Uint8Array; v: Uint8Array;
};

const HEADER = 32;

export function decodeVideo(buf: ArrayBuffer): VideoField | null {
  if (buf.byteLength < HEADER) return null;
  const d = new DataView(buf);
  if (d.getUint16(4, true) !== 1) return null;
  const width = d.getUint16(12, true), height = d.getUint16(14, true), ny = width * height;
  if (buf.byteLength !== HEADER + ny + ny / 2) return null;
  const fl = d.getUint8(7);
  return {
    width, height, colour: (fl & 1) !== 0, pal: (fl & 2) !== 0, seq: d.getUint32(28, true), t: d.getFloat64(16, true),
    lock: d.getFloat32(24, true),
    y: new Uint8Array(buf, HEADER, ny), u: new Uint8Array(buf, HEADER + ny, ny / 4), v: new Uint8Array(buf, HEADER + ny + ny / 4, ny / 4),
  };
}

// BT.601 full range to RGBA, into `out` (width * height * 4)
export function toRgba(f: VideoField, out: Uint8ClampedArray) {
  const { width: w, height: h, y, u, v } = f, cw = w >> 1;
  if (!f.colour) {
    for (let i = 0, o = 0; i < w * h; i++, o += 4) {
      out[o] = out[o + 1] = out[o + 2] = y[i];
      out[o + 3] = 255;
    }
    return;
  }
  for (let r = 0; r < h; r++) {
    const cr = (r >> 1) * cw;
    let o = r * w * 4, yi = r * w;
    for (let x = 0; x < w; x++, o += 4, yi++) {
      const Y = y[yi], c = cr + (x >> 1), U = u[c] - 128, V = v[c] - 128;
      out[o] = Y + 1.402 * V;
      out[o + 1] = Y - 0.344136 * U - 0.714136 * V;
      out[o + 2] = Y + 1.772 * U;
      out[o + 3] = 255;
    }
  }
}
