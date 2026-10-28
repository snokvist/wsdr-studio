// SPDX-License-Identifier: GPL-2.0-only
// End-to-end check against a running wsdr-studiod, as a second client:
//   node tests/e2e.mjs [--record]
// Reads the URL + token from the instance file. Checks the security gates (Host, token, Origin, paths)
// and the protocol's errors; with an adapter open in view A also the stream: hello/caps/settings/status,
// rows decode, a retune moves the rows' centre, DSP settings change the bin count, and (with --record)
// a 2 s SigMF recording. Without an adapter the stream checks are skipped (no hardware needed).
import { readFileSync, existsSync, statSync } from 'node:fs';
import http from 'node:http';
import os from 'node:os';

// (the backend's instance file: the runtime directory, else ~/.cache, as main.cpp)
const instance = `${process.env.XDG_RUNTIME_DIR || `${os.homedir()}/.cache`}/wsdr-studio/instance`;
if (!existsSync(instance)) {
  console.log(`FAIL no running wsdr-studiod (${instance} not found)`);
  process.exit(1);
}
const [, portS, token] = readFileSync(instance, 'utf8').trim().split(' ');
const port = Number(portS);
const deadline = (ms, what) => new Promise((_, rej) => setTimeout(() => rej(new Error(`${what}: no answer in ${ms} ms`)), ms));
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function get(path, headers = {}, method = 'GET') {
  return new Promise((resolve) => {
    const req = http.request({ host: '127.0.0.1', port, path, method, headers: { Host: `127.0.0.1:${port}`, ...headers } }, (res) => {
      let body = '';
      res.on('data', (d) => (body += d));
      res.on('end', () => resolve({ status: res.statusCode, body, headers: res.headers }));
    });
    req.on('error', () => resolve({ status: 0, body: '' }));
    req.end();
  });
}
function upgrade(path, headers) {
  return new Promise((resolve) => {
    const req = http.request({ host: '127.0.0.1', port, path, headers: {
      Host: `127.0.0.1:${port}`, Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Version': '13',
      'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', ...headers } });
    req.on('upgrade', (res, sock) => { sock.destroy(); resolve(res.statusCode); });
    req.on('response', (res) => resolve(res.statusCode));
    req.on('error', () => resolve(0));
    req.end();
  });
}

// --- security gates
check((await get('/api/hello')).status === 200, 'GET /api/hello with a good Host');
check((await get('/api/hello', { Host: 'evil.example:' + port })).status === 403, 'bad Host -> 403 (DNS rebinding)');
check((await get('/../../etc/passwd')).status === 400, 'path traversal -> 400');
check((await get('/%2e%2e/etc/passwd')).status === 400, 'encoded traversal -> 400');
check((await get('/')).status === 200, 'index served');
check((await upgrade('/ws', {})) === 403, 'WebSocket without token -> 403');
check((await upgrade('/ws?t=wrong' + token.slice(5), {})) === 403, 'WebSocket with a wrong token -> 403');
check((await upgrade(`/ws?t=${token}`, { Origin: 'https://evil.example' })) === 403, 'WebSocket from a foreign Origin -> 403');
check((await upgrade(`/ws?t=${token}`, { Origin: `http://127.0.0.1:${port}` })) === 101, 'WebSocket with token + own Origin -> 101');
check((await get('/', {}, 'HEAD')).status === 405, 'HEAD -> 405');
for (const p of ['/api/session', '/api/launch-key', `/api/session?t=${token}`])
  check((await get(p)).status === 404, `${p.split('?')[0]} does not exist (no way to obtain the token over HTTP)`);
const idx = await get('/');
check(!idx.body.includes(token) && !idx.headers['set-cookie'], 'the page carries no token and sets no cookie');

// --- protocol
const ws = new WebSocket(`ws://127.0.0.1:${port}/ws?t=${token}`);
ws.binaryType = 'arraybuffer';
const events = [], rows = [];
const pending = new Map();
let nextId = 1;
ws.onmessage = (m) => {
  if (m.data instanceof ArrayBuffer) {
    const v = new DataView(m.data);
    if (v.getUint32(0, true) === 0x57525357 && !(v.getUint8(6) & 0x10)) rows.push({ slot: 0, path: v.getUint8(6) & 0x0f, bins: v.getUint32(12, true), f0: v.getFloat64(24, true), df: v.getFloat64(32, true), len: m.data.byteLength });
    return;
  }
  const e = JSON.parse(m.data);
  if (e.id && pending.has(e.id)) { pending.get(e.id)(e); pending.delete(e.id); return; }
  if (e.slot === 1) return; // this test drives view A (slot 0)
  events.push(e);
};
await Promise.race([new Promise((r) => (ws.onopen = r)), deadline(5000, 'WebSocket open')]);
const req = (cmd, args = {}) =>
  Promise.race([
    new Promise((resolve) => { const id = nextId++; pending.set(id, resolve); ws.send(JSON.stringify({ cmd, id, ...args })); }),
    deadline(5000, cmd),
  ]);
await sleep(800);
const types = new Set(events.map((e) => e.type));
for (const t of ['hello', 'devices', 'caps', 'settings', 'status']) check(types.has(t), `received ${t}`);
const caps = events.find((e) => e.type === 'caps');
const open = caps?.open === true;
console.log(open ? `ok   adapter open (${caps.adapter})` : 'skip adapter not open in view A: the stream checks are skipped');
const centre = (r) => r.f0 + r.df * (r.bins / 2 - 0.5);
const settings0 = events.filter((e) => e.type === 'settings').at(-1);
const f_orig = settings0?.rx?.freq ?? 5180e6;

const bad = await req('nonsense');
check(bad.type === 'error' && bad.kind === 'bad_request', 'unknown command -> error with id');
const badf = await req('set', { freq: 1e9 });
check(badf.type === 'error' && badf.kind === 'bad_request', 'out-of-range frequency -> error');
const badt = await req('set', { freq: '5180' });
check(badt.type === 'error' && badt.kind === 'bad_request', 'frequency as a string -> error');
// a dsp request that must be rejected as a whole: path B on a one-path adapter (8812BU), else an
// unknown path name (8812CU has A and B)
const badp = await req('dsp', { fft: 1024, paths: (caps?.paths ?? 1) >= 2 ? 'c' : 'b' });
check(badp.type === 'error', (caps?.paths ?? 1) >= 2 ? 'unknown path -> error' : 'path B on a one-path adapter -> error');
const internal = await req('_lost');
check(internal.type === 'error', 'clients cannot send internal commands');
const s1 = await new Promise((r) => setTimeout(r, 300)).then(() => events.filter((e) => e.type === 'settings').at(-1));
check(s1.dsp.fft === settings0.dsp.fft, 'a rejected dsp request changes nothing');

// the stream (an adapter open): what it changes is put back, also when a check throws
try {
  if (!open) throw null;
  check(rows.length > 5, `rows arriving (${rows.length} in 0.8 s)`);
  check(rows.every((r) => r.len === 48 + 2 * r.bins), 'row frame length = header + 2 * bins');
  const target = f_orig + 40e6;
  check((await req('set', { freq: target })).type === 'ack', `retune to ${target / 1e6} MHz acked`);
  await sleep(1500);
  rows.length = 0;
  await sleep(500);
  check(rows.length > 0 && Math.abs(centre(rows.at(-1)) - target) < 2 * rows.at(-1).df, `rows centred on ${target / 1e6} MHz after retune (${rows.length ? (centre(rows.at(-1)) / 1e6).toFixed(3) : '-'})`);

  check((await req('dsp', { fft: 2048 })).type === 'ack', 'dsp fft 2048 acked');
  await sleep(400);
  rows.length = 0;
  await sleep(400);
  check(rows.length > 0 && rows.at(-1).bins === 2048, `bins = 2048 after the change (${rows.at(-1)?.bins})`);
} catch (e) {
  if (e) check(false, e.message);
} finally {
  if (open) {
    await req('dsp', { fft: settings0.dsp.fft }).catch(() => {});
    check((await req('set', { freq: f_orig }).catch(() => ({}))).type === 'ack', `back to ${f_orig / 1e6} MHz`);
  }
}

if (open && process.argv.includes('--record')) {
  const st = await req('record', { action: 'start' });
  check(st.type === 'ack' && st.file, `record start -> ${st.file}`);
  const again = await req('record', { action: 'start' });
  check(again.type === 'error' && again.kind === 'busy', 'a second record start is refused');
  await sleep(2000);
  const sp = await req('record', { action: 'stop' });
  check(sp.type === 'ack', 'record stop');
  const meta = st.file.replace(/\.sigmf-data$/, '.sigmf-meta');
  check(existsSync(st.file) && statSync(st.file).size > 0, `data file ${(statSync(st.file).size / 1e6).toFixed(1)} MB`);
  const m = JSON.parse(readFileSync(meta, 'utf8'));
  check(m.global['core:datatype'] === 'ci16_le' && m.captures[0]['core:frequency'] > 0, 'SigMF meta valid');
  const exp = m.global['core:sample_rate'] * 2 * 4;
  check(Math.abs(statSync(st.file).size / exp - 1) < 0.25, `size ~ 2 s of samples (${(statSync(st.file).size / exp).toFixed(2)} x)`);
  check(m.global['wsdr:samples'] * 4 === statSync(st.file).size, `meta sample count = file size / 4 (${m.global['wsdr:samples']})`);
  check(!m.global['wsdr:note'] && !m.global['wsdr:error'], 'recording complete (no early end, no error)');
}

ws.close();
console.log(fails ? `${fails} FAILED` : 'e2e passed');
process.exit(fails ? 1 : 0);
