// SPDX-License-Identifier: GPL-2.0-only
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hexDump, vendorLabel } from '../src/frames/index';

test('hexDump: offset, hex and printable text, 16 bytes a line', () => {
  const hex = Buffer.from('WSDR\x07\x00\x00\x00hello, world!!!\x7f<b>', 'latin1').toString('hex');
  const lines = hexDump(hex);
  assert.equal(lines.length, 2);
  assert.equal(lines[0], '0000  57 53 44 52 07 00 00 00 68 65 6c 6c 6f 2c 20 77  WSDR....hello, w');
  assert.equal(lines[1], '0010  6f 72 6c 64 21 21 21 7f 3c 62 3e' + ' '.repeat(15) + '  orld!!!.<b>');
  assert.deepEqual(hexDump(''), []);
});

test('vendorLabel: known names get their label, others their name', () => {
  const v = { version: 1, to: 'ff:ff:ff:ff:ff:ff', seq: 1, retry: false, enc: false, len: 0 };
  assert.equal(vendorLabel({ ...v, name: 'espnow' }), 'ESP-NOW');
  assert.equal(vendorLabel({ ...v, name: 'remoteid' }), 'remoteid');
});
