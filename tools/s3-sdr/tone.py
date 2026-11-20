# SPDX-License-Identifier: GPL-2.0-only
"""Measure S3 rfprobe tones with a wsdr receiver: level, local floor, image, frequency error.

  tone.py --port /dev/ttyACM0 --rx driver=wsdr,bus=6                  # one tone at +1 MHz
  tone.py --port /dev/ttyACM0 --rx driver=wsdr,bus=6 --offsets sweep  # across a 20 MHz channel, ~2 min

Both radios sit on --mhz. Each tone is read as the mean of --blocks blocks of 2^20 samples
(19 Hz bins at 20 MS/s). The S3 transmits at TX step 0 only.
"""
import argparse
import csv
import sys

import numpy as np

from s3lab import S3, Rx

SWEEP = "-9.5,-9,-8,-7,-6,-5,-4,-3,-2,-1,-0.5,0.5,1,2,3,4,5,6,7,8,9,9.5"
REF = 1e6  # the reference tone --guard re-measures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0", help="the S3's shell")
    ap.add_argument("--rx", default="driver=wsdr", help="SoapySDR args of the receiver (add bus= with two adapters)")
    ap.add_argument("--mhz", type=float, default=2437, help="centre of both radios (default channel 6)")
    ap.add_argument("--rate", type=float, default=20e6)
    ap.add_argument("--gain", type=float, default=30, help="receiver gain; the 8812BU's floor rises with it from 25 up")
    ap.add_argument("--amp", type=int, default=400, help="tone amplitude, 1..511")
    ap.add_argument("--offsets", default="1", help="tone offsets in MHz, comma separated, or 'sweep'")
    ap.add_argument("--blocks", type=int, default=4)
    ap.add_argument("--ppm", type=float, default=0, help="expected S3 clock error against the receiver")
    ap.add_argument("--span", type=float, default=20e3, help="search the tone within this many Hz")
    ap.add_argument("--guard", action="store_true", help="re-check a +1 MHz reference every 4 tones; stop if it fades")
    ap.add_argument("--csv", help="write the rows here")
    a = ap.parse_args()
    offs = [float(x) * 1e6 for x in (SWEEP if a.offsets == "sweep" else a.offsets).split(",")]

    s3 = S3(a.port)
    s3.setup(a.mhz)
    s3.off()
    rx = Rx(a.rx, a.rate, a.mhz, a.gain)
    p_off = rx.psd(a.blocks)

    def measure(hz):
        nominal = s3.tone(hz, a.amp)
        try:
            p = rx.psd(a.blocks)
        finally:
            s3.off()
        expect = nominal + (a.mhz * 1e6 + nominal) * a.ppm * 1e-6
        ft, lt = rx.peak(p, expect, a.span)
        _, li = rx.peak(p, -ft, 3e3)
        i = np.argmin(np.abs(rx.f - ft))
        return dict(offset_mhz=hz / 1e6, found_khz=ft / 1e3, error_hz=ft - nominal, level_dbfs=lt,
                    floor_dbfs=rx.floor(p, ft), off_dbfs=p_off[i], image_dbc=li - lt)

    def reference(tag):
        r = measure(REF)
        snr = r["level_dbfs"] - r["floor_dbfs"]
        ok = snr > 15
        print(f"  [reference +1 MHz {tag}: {snr:+.1f} dB over floor{'' if ok else ' -- faded, stopping'}]", flush=True)
        return ok

    rows = []
    try:
        alive = reference("start") if a.guard else True
        for k, hz in enumerate(offs):
            if not alive:
                break
            r = measure(hz)
            rows.append(r)
            snr = r["level_dbfs"] - r["floor_dbfs"]
            ppm = r["error_hz"] / (a.mhz * 1e6 + hz) * 1e6
            print(f"{hz / 1e6:+5.1f} MHz: {r['level_dbfs']:6.1f} dBFS, {snr:+5.1f} dB over floor "
                  f"(tone off {r['off_dbfs']:6.1f}); error {r['error_hz']:+6.0f} Hz ({ppm:+.2f} ppm); "
                  f"image {r['image_dbc']:+6.1f} dBc", flush=True)
            if a.guard and (k % 4 == 3 or k == len(offs) - 1):
                alive = reference("check")
    finally:
        s3.off()
        rx.close()
    if a.csv and rows:
        with open(a.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
