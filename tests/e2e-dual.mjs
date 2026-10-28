// SPDX-License-Identifier: GPL-2.0-only
// End-to-end check of the second view (slot B) against a running wsdr-studiod with view A open:
//   node tests/e2e-dual.mjs
// Needs a second RTL8812BU (the 8812CU is never used here). View B is stopped for the test if it runs,
// and opened again as it was afterwards. Checks: one adapter per view (B cannot open A's adapter),
// rows of B carry its slot and its centre, a retune of B moves only B, closing B leaves A running.
import { readFileSync } from 'node:fs';

import os from 'node:os';
const [, port, token] = readFileSync(`${process.env.XDG_RUNTIME_DIR || `${os.homedir()}/.cache`}/wsdr-studio/instance`, 'utf8').trim().split(' ');
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ws = new WebSocket(`ws://127.0.0.1:${port}/ws?t=${token}`);
ws.binaryType = 'arraybuffer';
const caps = [null, null], settings = [{}, {}], rows = [[], []];
let devices = [];
const pending = new Map();
let nextId = 1;
ws.onmessage = (m) => {
  if (m.data instanceof ArrayBuffer) {
    const v = new DataView(m.data);
    if (v.getUint32(0, true) !== 0x57525357) return;
    const slot = (v.getUint8(6) >> 4) & 1, bins = v.getUint32(12, true), f0 = v.getFloat64(24, true), df = v.getFloat64(32, true);
    rows[slot].push({ centre: f0 + df * (bins / 2 - 0.5), at: Date.now() });
    if (rows[slot].length > 200) rows[slot].shift();
    return;
  }
  const e = JSON.parse(m.data);
  if (pending.has(e.id)) {
    pending.get(e.id)(e);
    pending.delete(e.id);
    return;
  }
  if (e.type === 'devices') devices = e.devices;
  if (e.type === 'caps' && e.slot !== undefined) caps[e.slot] = e;
  if (e.type === 'settings' && e.slot !== undefined) settings[e.slot] = { ...settings[e.slot], ...e };
};
await Promise.race([new Promise((r) => (ws.onopen = r)), new Promise((_, rej) => setTimeout(() => rej(new Error('WebSocket open: no answer in 5 s')), 5000))]);
const req = (cmd, args = {}) => new Promise((r) => { const id = nextId++; pending.set(id, r); ws.send(JSON.stringify({ cmd, id, ...args })); });
const recent = (slot, ms) => rows[slot].filter((r) => Date.now() - r.at < ms);
const centredAt = (slot, f) => { const r = recent(slot, 500); return r.length > 0 && r.every((x) => Math.abs(x.centre - f) < 2.5e6); };
await sleep(1500);

check(caps[0]?.open === true, 'view A is open (start the app with an 8812BU in view A)');
const key = (d) => `${d.bus}-${d.port}`;
const aKey = caps[0]?.open ? `${caps[0].bus}-${caps[0].port}` : null;
const wasB = caps[1]?.open ? { pid: caps[1].pid, bus: caps[1].bus, port: caps[1].port, ...settings[1].rx } : null;
if (wasB) {
  check((await req('close', { slot: 1 })).type === 'ack', 'view B stopped for the test');
  await sleep(1500);
}
const free = devices.find((d) => d.pid === 0xb812 && key(d) !== aKey && d.can_open);
if (!aKey || !free) {
  console.log('skipped: needs view A open and a second RTL8812BU');
} else {
  const aDev = devices.find((d) => key(d) === aKey);
  const busy = await req('open', { slot: 1, pid: aDev.pid, bus: aDev.bus, port: aDev.port, freq: 5300e6, rate_sel: 1, wide: true });
  check(busy.type === 'error' && busy.kind === 'busy', `view B cannot open view A's adapter (${busy.kind ?? busy.type})`);
  const aCentre = recent(0, 500).at(-1)?.centre;
  const open = await req('open', { slot: 1, pid: free.pid, bus: free.bus, port: free.port, freq: 5300e6, rate_sel: 1, wide: true, allow_gapped: false });
  check(open.type === 'ack', `view B opens the other 8812BU (bus ${free.bus})`);
  await sleep(2500);
  check(caps[1]?.open === true, 'view B reports open');
  check(centredAt(1, 5300e6), `rows tagged view B, centred on 5300 MHz (${recent(1, 500).length} in 0.5 s)`);
  check(aCentre !== undefined && centredAt(0, aCentre), "view A's rows keep their centre");
  check((await req('set', { slot: 1, freq: 5340e6 })).type === 'ack', 'retune view B to 5340 MHz acked');
  await sleep(1500);
  check(centredAt(1, 5340e6), "view B's rows move to 5340 MHz");
  check(aCentre !== undefined && centredAt(0, aCentre), 'view A did not move');
  check((await req('close', { slot: 1 })).type === 'ack', 'view B closed');
  await sleep(1000);
  check(caps[1]?.open === false, 'view B reports closed');
  check(recent(0, 500).length > 0, 'view A still streaming');
}
if (wasB) {
  const back = await req('open', { slot: 1, ...wasB });
  check(back.type === 'ack', `view B opened again as it was (${(wasB.freq / 1e6).toFixed(0)} MHz)`);
}
ws.close();
console.log(fails ? `${fails} check(s) failed` : 'e2e-dual passed');
process.exit(fails ? 1 : 0);
