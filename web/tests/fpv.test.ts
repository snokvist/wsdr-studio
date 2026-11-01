// SPDX-License-Identifier: GPL-2.0-only
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { FPV_BANDS, fpvChannels, videoStandard, videoText } from '../src/fpv';

test('the FPV band tables: 8 channels each, the shared ones agree', () => {
  for (const chans of Object.values(FPV_BANDS)) assert.equal(chans.length, 8);
  assert.equal(FPV_BANDS.F[7], FPV_BANDS.R[6]); // F8 = R7 = 5880
  assert.equal(FPV_BANDS.R[4], 5806);
});

test('the channels near a frequency, nearest first', () => {
  assert.deepEqual(fpvChannels(5806), ['R5 5806', 'A4 5805']);
  assert.deepEqual(fpvChannels(5880.4), ['F8 5880', 'R7 5880']);
  assert.deepEqual(fpvChannels(5658), ['R1 5658']);
  assert.deepEqual(fpvChannels(5500), []);
  assert.deepEqual(fpvChannels(2432), []); // (2.4 GHz video senders: no FPV table)
});

test('the standard from the line rate', () => {
  assert.equal(videoStandard(15.625), 'PAL/SECAM, 625 lines');
  assert.equal(videoStandard(15.735), 'NTSC, 525 lines');
  assert.equal(videoStandard(16.0), '16.000 kHz lines');
});

test('the hover text', () => {
  assert.equal(videoText(5806.2, 15.734, 8.1), 'analog video, FM (NTSC, 525 lines) · ≈5806 MHz, FPV R5 5806 or A4 5805 · deviation 8 MHz p-p');
  assert.equal(videoText(2432, 15.625, 9.4), 'analog video, FM (PAL/SECAM, 625 lines) · ≈2432 MHz · deviation 9 MHz p-p');
});
