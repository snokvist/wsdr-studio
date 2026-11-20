# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn frames.c's 40 MS/s train into a 20 MS/s wsdr-like recording with noise and a clock offset.

  frames.py <in.raw> <out.sigmf-data> <SNR dB in 20 MHz> [ppm] [MHz]
"""
import json
import sys

import numpy as np

raw, out, snr = sys.argv[1], sys.argv[2], float(sys.argv[3])
ppm = float(sys.argv[4]) if len(sys.argv) > 4 else 2.2
mhz = float(sys.argv[5]) if len(sys.argv) > 5 else 2437
x = np.fromfile(raw, np.int16).astype(np.float64)
x = (x[0::2] + 1j * x[1::2])[::2]  # 20 MS/s (the pulse is within +-8.25 MHz)
fs = 20e6
x = np.concatenate([np.zeros(int(0.02 * fs)), x])  # 20 ms of noise first: the finder needs a run-in
x *= np.exp(2j * np.pi * mhz * 1e6 * ppm * 1e-6 * np.arange(len(x)) / fs)
on = np.abs(x) > 1e-9
p_sig = np.mean(np.abs(x[on]) ** 2)
rng = np.random.default_rng(1)
n = (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x))) * np.sqrt(p_sig / 10 ** (snr / 10) / 2)
y = (x + n) / np.sqrt(p_sig) * 10 ** (-30 / 20) * 32767  # the frames at -30 dBFS
v = np.empty(2 * len(y), np.int16)
v[0::2] = np.clip(np.round(y.real), -32768, 32767)
v[1::2] = np.clip(np.round(y.imag), -32768, 32767)
v.tofile(out)
meta = {"global": {"core:datatype": "ci16_le", "core:sample_rate": fs, "core:version": "1.0.0"},
        "captures": [{"core:frequency": mhz * 1e6, "core:sample_start": 0}], "annotations": []}
json.dump(meta, open(out[: -len(".sigmf-data")] + ".sigmf-meta", "w"), indent=2)
