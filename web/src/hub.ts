// SPDX-License-Identifier: GPL-2.0-only
// One socket for the window, shared by both views. Rows and events tagged with a slot go to that
// view (rows also reach the other view, for its overlay); untagged events go to everyone. The latest
// state events are kept, so a view that appears later (entering a two-adapter mode) starts current.
import { Client, type Row } from './protocol';
import type { TapFrame } from './taps/index';

/* eslint-disable @typescript-eslint/no-explicit-any */
export type Sub = {
  slot: number | null;            // null: every event (the app shell); no rows
  row?: (r: Row) => void;         // this slot's rows
  other?: (r: Row) => void;       // the other slot's rows
  all?: (r: Row) => void;         // every row (the stitched Panorama plot)
  tap?: (f: TapFrame) => void;    // this slot's tap frames (taps/index.ts)
  event?: (e: any) => void;
  connection?: (up: boolean) => void;
  giveUp?: (why: string) => void;
};

const KEPT = new Set(['hello', 'devices', 'caps', 'settings', 'status', 'tap']);

export class Hub {
  private client: Client | null = null;
  private subs = new Set<Sub>();
  private kept = new Map<string, any>(); // `${slot}:${type}` (a tap's: `:${kind}` added) -> the latest event
  private gen = 0;
  private up = false;
  private fatal: string | null = null;

  start() {
    const gen = ++this.gen;
    Client.fromLocation()
      .then((c) => {
        if (gen !== this.gen) return; // stopped meanwhile
        this.client = c;
        c.onRow = (r) => {
          if (gen !== this.gen) return; // a stopped client's late callbacks
          for (const s of this.subs) {
            s.all?.(r);
            if (s.slot === r.slot) s.row?.(r);
            else if (s.slot !== null) s.other?.(r);
          }
        };
        c.onTap = (f) => {
          if (gen !== this.gen) return;
          for (const s of this.subs) if (s.slot === f.slot) s.tap?.(f);
        };
        c.onConnection = (up) => {
          if (gen !== this.gen) return;
          this.up = up;
          if (!up) this.kept.clear(); // the server sends the state again on reconnect
          for (const s of this.subs) s.connection?.(up);
        };
        c.onGiveUp = (why) => gen === this.gen && this.giveUp(why);
        c.onEvent = (e) => {
          if (gen !== this.gen) return;
          // a closed adapter has no live settings: the last run's must not come back to a late view
          if (e.type === 'caps' && !e.open) {
            const k = `${e.slot ?? '-'}:settings`, kept = this.kept.get(k);
            if (kept?.rx) {
              const rest = { ...kept };
              delete rest.rx;
              this.kept.set(k, rest);
            }
          }
          if (KEPT.has(e.type)) {
            const k = `${e.slot ?? '-'}:${e.type}${e.type === 'tap' ? `:${e.kind}` : ''}`; // (one per tap kind)
            // settings may carry rx or dsp alone: keep both
            this.kept.set(k, e.type === 'settings' ? { ...this.kept.get(k), ...e } : e);
          }
          for (const s of this.subs) if (s.slot === null || e.slot === undefined || e.slot === s.slot) s.event?.(e);
        };
        c.connect();
      })
      .catch((e: Error) => gen === this.gen && this.giveUp(e.message));
  }

  stop() {
    this.gen++;
    this.client?.close();
    this.client = null;
    this.up = false;
    this.kept.clear();
  }

  private giveUp(why: string) {
    this.fatal = why;
    for (const s of this.subs) s.giveUp?.(why);
  }

  // Returns the unsubscribe function. The connection state and the kept events are replayed first.
  subscribe(s: Sub): () => void {
    this.subs.add(s);
    if (this.fatal) s.giveUp?.(this.fatal);
    if (this.up) s.connection?.(true);
    for (const type of KEPT)
      for (const e of this.kept.values())
        if (e.type === type && (s.slot === null || e.slot === undefined || e.slot === s.slot)) s.event?.(e);
    return () => this.subs.delete(s);
  }

  request(cmd: string, args: Record<string, unknown> = {}): Promise<any> {
    return this.client ? this.client.request(cmd, args) : Promise.reject(new Error('not connected'));
  }
}
