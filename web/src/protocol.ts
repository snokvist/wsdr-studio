// SPDX-License-Identifier: GPL-2.0-only
// Client side of protocol v1 (src/protocol.h): JSON commands with ids, JSON events, binary rows.

// gapped: a wide span the link cannot carry continuously (8812CU on USB 2): one 8192-sample capture,
// then a ~1.5 ms gap while it ships. Spectrum yes, continuous IQ (recording) no.
export type Rate = { rate_sel: number; fs: number; gapped?: boolean; hw_fs?: number }; // hw_fs: captured rate (offset tuning)
export type Device = {
  pid: number; model: string; chip: string; bus: number; port: string; speed_mbps: number;
  controller: string; driver: string; can_open: boolean; max_msps: number;
  gapped_msps?: number; // widest span in gapped mode (40 on the 8812CU, 80 on the 8812BU), 0 = none
};
export type Caps = {
  open: boolean; adapter?: string; pid?: number; bus?: number; port?: string; chip?: string; superspeed?: boolean; paths?: number;
  rates?: Rate[]; gain_max?: number; gain_step?: number; wide_capable?: boolean;
};
// allow_gapped: a gapped span may be used (only ever because the user picked one)
export type RxSettings = { freq: number; rate_sel: number; wide: boolean; agc: boolean; gain_db: number; allow_gapped?: boolean };
export type DspSettings = { fft: number; rows_per_s: number; paths: 'a' | 'b' | 'ab'; dc_remove: boolean; wifi?: boolean };
// A Wi-Fi frame found in the samples (the "wifi" event): start (s, the rows' clock), channel centre
// (MHz), width (MHz), format (L legacy OFDM, H 802.11n, V 802.11ac, B 802.11b), L-SIG rate (Mb/s;
// 802.11b: 1, 2, 5 for 5.5, 11), length (bytes),
// airtime (us), SNR (dB)
// A 6 Mb/s frame decoded whole adds its sender's address, and a beacon its network (BSSID, name, channel).
export type WifiFrame = [
  t: number, mhz: number, bw: number, fmt: 'L' | 'H' | 'V' | 'B', rate: number, len: number, us: number, snr: number,
  ta?: string | null, bssid?: string | null, ssid?: string | null, ch?: number,
];
// One frame's equalised subcarriers (the "points" command; see Engine::points): z holds I, Q pairs at
// `scale` per unit: n_lsig L-SIG points, n_sig2 HT-SIG / VHT-SIG-A ones, then the payload's (bits per
// subcarrier `bits`, 0: none). EVM in dB (payload: null when none), carrier offset in Hz and ppm.
export type FramePoints = {
  frame: number; t: number; mhz: number; bw: number; fmt: 'L' | 'H' | 'V' | 'B'; rate: number; len: number; snr: number;
  cfo_hz: number; cfo_ppm: number; evm_lsig_db: number; evm_db: number | null; bits: number; n_lsig: number; n_sig2: number;
  scale: number; z: number[];
  ta?: string; ssid?: string; // a 6 Mb/s frame decoded whole: its sender; a beacon: its network
  // HT / VHT, from its SIG: why the payload is not shown (0 shown, 1 SIG CRC failed, 2 wider than 20 MHz,
  // 3 not a plain single stream, 4 not received, 5 none: a sounding frame), and when the SIG was read
  // its streams (0: multi-user), width and (not multi-user) MCS
  skip?: number; mcs?: number; nss?: number; width?: number; sgi?: boolean; ldpc?: boolean; stbc?: boolean;
};
// Another signal found in the samples (the "wifi" event's "other"): start (s), centre (MHz), width
// (MHz), kind (T Bluetooth, N narrowband, M microwave oven, E BLE advertising, C steady carrier),
// duration (us); a decoded BLE advertisement adds its sender's address, whether it is random, and its
// name ("" none); a steady carrier the spacing of the comb around it (kHz, 0 none).
export type OtherSignal = [t: number, mhz: number, bw: number, kind: 'T' | 'N' | 'M' | 'E' | 'C', us: number,
  addr?: string | null, random?: boolean | null, name?: string | null, comb?: number];
export type Status = {
  state: string; adapter: string; error: string; t: number; rows: number; ring_dropped: number; dsp_load: number;
  rx?: { state: string; banks: number; restarts: number; channel: number; width: number; centre: number; fs: number; gapped?: boolean; error: string;
        offset?: boolean; hw_centre?: number; hw_fs?: number }; // offset: radio tuned beside the view
  record: { active: boolean; file: string; samples: number; seconds: number; bytes: number; error: string };
  client_drops?: number;
  disk_free?: number; // bytes free where recordings go
};
export type Row = {
  slot: number; // view slot (0 = A, 1 = B)
  path: number; gapped: boolean; offset: boolean; gen: number; seq: number; bins: number; t: number; f0: number; df: number;
  avg: Float32Array; max: Float32Array;
};

const MAGIC = 0x57525357;
const HEADER = 48;

export function decodeRow(buf: ArrayBuffer): Row | null {
  if (buf.byteLength < HEADER) return null;
  const v = new DataView(buf);
  if (v.getUint32(0, true) !== MAGIC || v.getUint16(4, true) !== 1) return null;
  const bins = v.getUint32(12, true);
  if (bins < 2 || buf.byteLength !== HEADER + 2 * bins) return null;
  const dbMin = v.getFloat32(40, true), dbStep = v.getFloat32(44, true);
  const u = new Uint8Array(buf, HEADER);
  const avg = new Float32Array(bins), max = new Float32Array(bins);
  for (let i = 0; i < bins; i++) {
    avg[i] = dbMin + dbStep * u[i];
    max[i] = dbMin + dbStep * u[bins + i];
  }
  return {
    slot: (v.getUint8(6) >> 4) & 1, path: v.getUint8(6) & 0x0f, gapped: (v.getUint8(6) & 0x80) !== 0, offset: (v.getUint8(6) & 0x40) !== 0, gen: v.getUint8(7), seq: v.getUint32(8, true), bins, t: v.getFloat64(16, true),
    f0: v.getFloat64(24, true), df: v.getFloat64(32, true), avg, max,
  };
}

let sessionPromise: Promise<string> | null = null;

type Pending = { resolve: (r: any) => void; reject: (e: Error) => void };

export class Client {
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private closed = false;
  onEvent: (e: any) => void = () => {};
  onRow: (r: Row) => void = () => {};
  onConnection: (up: boolean) => void = () => {};
  onGiveUp: (why: string) => void = () => {};
  private failures = 0; // connection attempts that never opened, in a row

  private url: string;

  constructor(url: string) {
    this.url = url;
  }

  // The session token comes from the desktop shell (preload bridge, over IPC): it is never in a URL.
  static fromLocation(): Promise<Client> {
    sessionPromise ??= Client.session();
    return sessionPromise.then((url) => new Client(url));
  }

  private static async session(): Promise<string> {
    const bridge = (window as { wsdr?: { session(): Promise<{ token: string }> } }).wsdr;
    if (!bridge) throw new Error('This page runs inside the wsdr-studio app. Start wsdr-studio to use it.');
    const { token } = await bridge.session();
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/ws?t=${encodeURIComponent(token)}`;
  }

  connect() {
    this.closed = false;
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    let opened = false;
    ws.onopen = () => {
      opened = true;
      this.failures = 0;
      this.onConnection(true);
    };
    ws.onclose = () => {
      this.onConnection(false);
      for (const p of this.pending.values()) p.reject(new Error('connection closed'));
      this.pending.clear();
      if (this.closed) return;
      if (!opened && ++this.failures >= 3) {
        // refused (stale session after a restart) or nobody listening (the app has exited)
        this.onGiveUp('Lost the connection to wsdr-studio (it was closed or restarted). Start wsdr-studio again.');
        return;
      }
      setTimeout(() => this.connect(), 1000);
    };
    ws.onmessage = (m) => {
      if (m.data instanceof ArrayBuffer) {
        const r = decodeRow(m.data);
        if (r) this.onRow(r);
        return;
      }
      const e = JSON.parse(m.data as string);
      if ((e.type === 'ack' || e.type === 'error') && typeof e.id === 'number' && this.pending.has(e.id)) {
        const p = this.pending.get(e.id)!;
        this.pending.delete(e.id);
        if (e.type === 'ack') p.resolve(e);
        else p.reject(Object.assign(new Error(e.msg ?? 'error'), { kind: e.kind as string | undefined }));
        return;
      }
      this.onEvent(e);
    };
    this.ws = ws;
  }

  close() {
    this.closed = true;
    this.ws?.close();
  }

  request(cmd: string, args: Record<string, unknown> = {}): Promise<any> {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new Error('not connected'));
        return;
      }
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ cmd, id, ...args }));
    });
  }
}
