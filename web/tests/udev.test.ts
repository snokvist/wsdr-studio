// SPDX-License-Identifier: GPL-2.0-only
// The in-app udev rule (self-contained, so the AppImage can offer it) against wifi-sdr's rules file and the README.
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { test } from 'node:test';
import { UDEV_RULES } from '../src/util';

const read = (rel: string) => readFileSync(new URL(rel, import.meta.url), 'utf8');
const RULES = '../../external/wifi-sdr/tools/70-wsdr.rules';
// A web-only checkout has no submodule: skip rather than fail on a missing file
const noRules = !existsSync(new URL(RULES, import.meta.url)) && 'external/wifi-sdr is not checked out';

test('UDEV_RULES is the rule list of wifi-sdr tools/70-wsdr.rules', { skip: noRules }, () => {
  const rules = read(RULES).split('\n').filter((l) => l.startsWith('SUBSYSTEM=='));
  assert.deepEqual(UDEV_RULES.split('\n'), rules);
});

test('the README quick start carries every rule', () => {
  const readme = read('../../README.md');
  for (const rule of UDEV_RULES.split('\n'))
    assert.ok(readme.includes(rule), rule);
});
