# SPDX-License-Identifier: GPL-2.0-only
"""2.4 GHz channel occupancy with a wsdr receiver: per channel 1-13, the floor and how often it is busy.

  chanscan.py --rx driver=wsdr,bus=6

Pick the quietest channel for tone and frame tests (at the bench's desk: channel 6).
"""
import argparse
import time

import numpy as np


def main():
    import SoapySDR
    from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rx", default="driver=wsdr")
    ap.add_argument("--gain", type=float, default=30)
    ap.add_argument("--seconds", type=float, default=2.0, help="per channel")
    a = ap.parse_args()
    d = SoapySDR.Device(a.rx)
    d.setGainMode(SOAPY_SDR_RX, 0, False)
    d.setGain(SOAPY_SDR_RX, 0, a.gain)
    d.setDCOffsetMode(SOAPY_SDR_RX, 0, True)
    d.setSampleRate(SOAPY_SDR_RX, 0, 20e6)
    d.setFrequency(SOAPY_SDR_RX, 0, 2412e6)
    st = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    d.activateStream(st)
    buf = np.empty(2 * 65536, np.int16)
    for ch in range(1, 14):
        mhz = 2407 + 5 * ch
        d.setFrequency(SOAPY_SDR_RX, 0, mhz * 1e6)
        time.sleep(0.4)
        while 0 <= d.readStream(st, [buf], 65536, timeoutUs=200000).ret == 65536:
            pass  # drain: the ring keeps its oldest samples on overflow
        p, end = [], time.time() + a.seconds
        while time.time() < end:
            n = d.readStream(st, [buf], 65536, timeoutUs=200000).ret
            if n > 0:
                x = (buf[0:2 * n:2] + 1j * buf[1:2 * n:2]) / 32768.0
                p.extend((np.abs(x[: n // 200 * 200]) ** 2).reshape(-1, 200).mean(1))  # 10 us powers
        b = 10 * np.log10(np.array(p) + 1e-12)
        m = np.median(b)
        print(f"ch {ch:2d} ({mhz} MHz): floor {m:6.1f} dBFS, busy {100 * np.mean(b > m + 3):5.2f} % "
              f"(> floor +3 dB), {100 * np.mean(b > m + 10):5.2f} % (> +10 dB)", flush=True)
    d.deactivateStream(st)
    d.closeStream(st)


if __name__ == "__main__":
    main()
