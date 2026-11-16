// SPDX-License-Identifier: GPL-2.0-only
// The in-app udev rule (self-contained, so the AppImage can offer it) against wifi-sdr's rules file and the README.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { UDEV_RULES } from '../src/util';

const read = (rel: string) => readFileSync(new URL(rel, import.meta.url), 'utf8');

test('UDEV_RULES is the rule list of wifi-sdr tools/70-wsdr.rules', () => {
  const rules = read('../../external/wifi-sdr/tools/70-wsdr.rules').split('\n').filter((l) => l.startsWith('SUBSYSTEM=='));
  assert.deepEqual(UDEV_RULES.split('\n'), rules);
});

test('the README quick start carries every rule', () => {
  const readme = read('../../README.md');
  for (const rule of UDEV_RULES.split('\n'))
    assert.ok(readme.includes(rule), rule);
});
