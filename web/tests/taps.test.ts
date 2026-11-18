// SPDX-License-Identifier: GPL-2.0-only
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { tapFrame } from '../src/taps/index';
import { decodeVideo, toRgba } from '../src/taps/video';

// A video field as src/video.h lays it out (encode_video): 4 x 2, colour, PAL, view B, watch 3, seq 9;
// Y 100, U 128 (none), V 200 (red)
function field(w: number, h: number, flags: number, slot: number) {
  const ny = w * h, b = new ArrayBuffer(32 + ny + ny / 2), d = new DataView(b), u = new Uint8Array(b);
  d.setUint32(0, 0x46565357, true);
  d.setUint16(4, 1, true);
  d.setUint8(6, slot);
  d.setUint8(7, flags);
  d.setUint32(8, 3, true);
  d.setUint32(28, 9, true);
  d.setUint16(12, w, true);
  d.setUint16(14, h, true);
  d.setFloat64(16, 12.5, true);
  d.setFloat32(24, 0.75, true);
  u.fill(100, 32, 32 + ny);
  u.fill(128, 32 + ny, 32 + ny + ny / 4);
  u.fill(200, 32 + ny + ny / 4);
  return b;
}

test('tap frames: routed by their magic to their kind and view; spectrum rows and unknown magics are not', () => {
  const f = tapFrame(field(4, 2, 3, 1));
  assert.equal(f?.kind, 'video');
  assert.equal(f?.slot, 1);
  assert.equal(f?.watch, 3);
  const row = new ArrayBuffer(64);
  new DataView(row).setUint32(0, 0x57525357, true);
  assert.equal(tapFrame(row), null);
  assert.equal(tapFrame(new ArrayBuffer(4)), null);
});

test('video fields: decoded as encode_video writes them, and to RGB (BT.601)', () => {
  const v = decodeVideo(field(4, 2, 3, 1))!;
  assert.ok(v);
  assert.deepEqual([v.width, v.height, v.colour, v.pal, v.seq, v.t, v.lock], [4, 2, true, true, 9, 12.5, 0.75]);
  const out = new Uint8ClampedArray(4 * 2 * 4);
  toRgba(v, out);
  assert.deepEqual([...out.slice(0, 4)], [Math.round(100 + 1.402 * 72), Math.round(100 - 0.714136 * 72), 100, 255]);
  const grey = decodeVideo(field(4, 2, 0, 0))!;
  toRgba(grey, out);
  assert.deepEqual([...out.slice(4, 8)], [100, 100, 100, 255]); // black and white: the chroma is not used
  assert.equal(decodeVideo(field(4, 2, 3, 1).slice(0, 40)), null); // short
});
