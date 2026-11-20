# SPDX-License-Identifier: GPL-2.0-only
"""Decode rate against level: the S3 (wsdrtx) sends numbered ESP-NOW frames at falling amplitudes
while a wsdr receiver records; test_core --wifi says which counters came through.

  decode_level.py --rx driver=wsdr,bus=6 --amps 400,200,100,50,25,12,6,3 --count 200 --rate 1

Per amplitude: frames sent, decoded (exact, by counter), the median SNR test_core measured.
The S3 transmits at TX step --step (default 0); its firmware allows frames up to step 8.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

from s3lab import S3, Rx

HERE = os.path.dirname(os.path.abspath(__file__))


def record(rx, seconds_max, start, done):
    """Catch up with live samples, call start(), then read until done is set (or seconds_max)."""
    n_max = int(rx.rate * seconds_max)
    buf = np.zeros(2 * n_max, np.int16)
    got, gaps = 0, 0
    rx.drain()
    start()
    end_at = None
    while got < n_max:
        r = rx.dev.readStream(rx.st, [buf[2 * got:]], min(65536, n_max - got), timeoutUs=200000)
        if r.ret > 0:
            got += r.ret
        elif r.ret < 0:
            gaps += 1  # an overflow: samples lost (counted, the recording goes on)
        if done.is_set():
            end_at = end_at or time.time() + 0.05
            if time.time() > end_at:
                break
    return buf[: 2 * got], gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the S3's shell (default: the one esp-sdr board's by-id link)")
    ap.add_argument("--rx", default="driver=wsdr")
    ap.add_argument("--mhz", type=float, default=2437)
    ap.add_argument("--gain", type=float, default=30)
    ap.add_argument("--amps", default="400,200,100,50,25,12,6,3")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--period", type=int, default=5, help="ms between frames")
    ap.add_argument("--rate", type=int, default=1, choices=(1, 2))
    ap.add_argument("--size", type=int, default=10, help="ESP-NOW payload bytes, 10..100")
    ap.add_argument("--step", type=int, default=0, choices=range(0, 9), help="TX step for the frames, 0..8")
    ap.add_argument("--test-core", default=os.path.join(HERE, "../../build/test_core"))
    ap.add_argument("--keep", help="keep the recordings in this folder")
    a = ap.parse_args()

    s3 = S3(a.port)
    s3.setup(a.mhz)
    rx = Rx(a.rx, 20e6, a.mhz, a.gain)
    tmp = a.keep or tempfile.mkdtemp(prefix="decode_level-")
    os.makedirs(tmp, exist_ok=True)
    print(f"{'amp':>4} {'dB':>6} {'sent':>5} {'decoded':>8} {'%':>6} {'SNR':>6}  (rate {a.rate} Mb/s, "
          f"{a.size} bytes, every {a.period} ms, step {a.step})", flush=True)
    try:
        for amp in [int(x) for x in a.amps.split(",")]:
            reply, done = {}, threading.Event()

            def send():
                reply["r"] = s3.cmd(f"probe frames {a.count} {a.period} {amp} {a.rate} {a.size} {a.step}",
                                    wait=a.count * a.period / 1000 + 5)
                done.set()

            t = threading.Thread(target=send)
            iq, gaps = record(rx, a.count * a.period / 1000 + 6, t.start, done)
            t.join()
            m = re.search(r"frames (\d+) sent, (\d+) errors.*counters (\d+)\.\.(\d+)", reply.get("r", ""))
            if not m:
                print(f"{amp:4d}: the S3 did not answer as expected: {reply.get('r', '').strip()}")
                break
            sent, c0, c1 = int(m.group(1)), int(m.group(3)), int(m.group(4))
            base = os.path.join(tmp, f"amp{amp}")
            iq.tofile(base + ".sigmf-data")
            json.dump({"global": {"core:datatype": "ci16_le", "core:sample_rate": 20e6, "core:version": "1.0.0"},
                       "captures": [{"core:frequency": a.mhz * 1e6, "core:sample_start": 0}],
                       "annotations": []}, open(base + ".sigmf-meta", "w"))
            out = subprocess.run([a.test_core, "--wifi", base + ".sigmf-data"], capture_output=True, text=True,
                                 env=dict(os.environ, ESPNOW_TIMES="1")).stdout
            seen = {}
            for n, snr in re.findall(r"espnow (\d+) at [\d.]+ s, SNR (-?[\d.]+) dB", out):
                if c0 <= int(n) <= c1:
                    seen[int(n)] = float(snr)
            snr = f"{statistics.median(seen.values()):6.1f}" if seen else "     -"
            n = c1 - c0 + 1
            print(f"{amp:4d} {20 * np.log10(amp / 400):6.1f} {sent:5d} {len(seen):8d} {100 * len(seen) / n:6.1f} {snr}"
                  f"{'  (' + str(gaps) + ' receive gaps)' if gaps else ''}", flush=True)
            if not a.keep:
                os.remove(base + ".sigmf-data")
                os.remove(base + ".sigmf-meta")
    finally:
        rx.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
