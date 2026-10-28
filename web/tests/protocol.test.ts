// SPDX-License-Identifier: GPL-2.0-only
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { decodeRow } from '../src/protocol';

// The same frame tests/test_core.cpp encodes (test_row_golden): path B of view B, gapped and offset,
// run 7, seq 258, 4 bins. The two decoders read one wire format.
const GOLDEN =
  '57535257' + '0100' + 'd1' + '07' + '02010000' + '04000000' + '000000000000f83f' + '00000070084cf341' + '000000000040af40' +
  '000016c3' + '9a99193f' + '00' + '53' + 'd9' + 'ff' + '01' + '54' + 'd9' + 'ff';
const bytes = (hex: string) => Uint8Array.from(hex.match(/../g)!.map((h) => parseInt(h, 16))).buffer;

test('the row decoder reads the backend’s frame (golden)', () => {
  const r = decodeRow(bytes(GOLDEN))!;
  assert.ok(r);
  assert.equal(r.slot, 1);
  assert.equal(r.path, 1);
  assert.equal(r.gapped, true);
  assert.equal(r.offset, true);
  assert.equal(r.gen, 7);
  assert.equal(r.seq, 258);
  assert.equal(r.bins, 4);
  assert.equal(r.t, 1.5);
  assert.equal(r.f0, 5180e6);
  assert.equal(r.df, 4000);
  const step = 0.6;
  for (const [i, db] of [-150, -100, -20, 3].entries()) assert.ok(Math.abs(r.avg[i] - db) <= step / 2 + 1e-3, `avg[${i}] ${r.avg[i]} ~ ${db}`);
  for (const [i, db] of [-149.4, -99.7, -19.9, 3].entries()) assert.ok(Math.abs(r.max[i] - db) <= step / 2 + 1e-3, `max[${i}] ${r.max[i]} ~ ${db} (10 dBFS clamps to the top)`);
});

test('the row decoder refuses a short, a wrong-magic and a wrong-length frame', () => {
  assert.equal(decodeRow(bytes(GOLDEN).slice(0, 47)), null);
  assert.equal(decodeRow(bytes('00' + GOLDEN.slice(2))), null);
  assert.equal(decodeRow(bytes(GOLDEN + '00')), null);
});
