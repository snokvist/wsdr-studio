# SPDX-License-Identifier: GPL-2.0-only
"""Shared pieces for the S3 SDR bench: the S3's Zephyr shell and the wsdr receiver.

The S3 runs esp-sdr-zephyr's rfprobe app; the receiver is a wsdr adapter through SoapySDR
(SOAPY_SDR_PLUGIN_PATH pointing at a wifi-sdr build-soapy folder).
"""
import glob
import re
import time

import numpy as np
import serial

PROMPT = b"uart:~$"
MAX_STEP = 0  # the bench radiates at TX step 0 only; raise it here, deliberately, if that changes


def find_port():
    """The S3's shell: the by-id link of an esp-sdr-zephyr board (its tty number changes on replug)."""
    ports = sorted(glob.glob("/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR*-if00"))
    if len(ports) != 1:
        raise RuntimeError(f"{len(ports)} esp-sdr boards on USB; pass --port")
    return ports[0]


class S3:
    """The S3's USB shell. Each command returns once the prompt is back (or after `wait` s)."""

    def __init__(self, port=None):
        self.port = serial.Serial(port or find_port(), 115200, timeout=0.1)
        self.cmd("")

    def cmd(self, line, wait=3.0):
        m = re.match(r"\s*probe\s+gain\s+\S+\s+(-?\d+)", line)
        if m and int(m.group(1)) > MAX_STEP:
            raise ValueError(f"TX step {m.group(1)} is above the bench's limit ({MAX_STEP})")
        self.port.read(65536)
        self.port.write(line.encode() + b"\r")
        out, end = b"", time.time() + wait
        while time.time() < end:
            out += self.port.read(4096) or b""
            if out.rstrip().endswith(PROMPT) and len(out) > len(line) + len(PROMPT) + 2:
                break
        return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode(errors="replace"))

    def setup(self, mhz):
        """Step 0 and the LO; rfprobe must be running (see README: our build starts it at boot)."""
        for c in ("probe gain -1 0", f"probe f {mhz:g}"):
            r = self.cmd(c)
            if "ret 0" not in r:
                raise RuntimeError(f"{c!r} failed (is rfprobe running?): {r.strip()}")

    def tone(self, hz, amp):
        """Start a tone at LO + hz; returns the offset the S3 actually plays (its DAC period rounds it)."""
        r = self.cmd(f"probe tone on {int(hz)} {int(amp)}")
        m = re.search(r"([+-]\d+) Hz period", r)
        if not m:
            raise RuntimeError(f"tone did not start: {r.strip()}")
        return float(m.group(1))

    def off(self):
        self.cmd("probe tone off")


class Rx:
    """A wsdr receiver with fixed gain, read in contiguous blocks of n samples."""

    def __init__(self, args, rate, mhz, gain=30, n=1 << 20):
        import SoapySDR
        from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX

        self.rate, self.n = rate, n
        d = self.dev = SoapySDR.Device(args)
        d.setGainMode(SOAPY_SDR_RX, 0, False)
        d.setGain(SOAPY_SDR_RX, 0, gain)
        d.setDCOffsetMode(SOAPY_SDR_RX, 0, True)
        d.setSampleRate(SOAPY_SDR_RX, 0, rate)
        got = d.getSampleRate(SOAPY_SDR_RX, 0)
        if got != rate:  # an 8812BU on USB 2 tops out at 5 MS/s and only warns
            raise RuntimeError(f"receiver runs at {got / 1e6:g} MS/s, not {rate / 1e6:g} (USB 2?)")
        d.setFrequency(SOAPY_SDR_RX, 0, mhz * 1e6)
        self.st = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
        d.activateStream(self.st)
        self.w = np.hanning(n)
        self.f = np.fft.fftshift(np.fft.fftfreq(n, 1 / rate))
        time.sleep(1.0)

    def drain(self):
        """Read until caught up. SoapyWsdr's ring keeps its OLDEST samples when it overflows, so after
        any pause in reading (talking to the S3) the next blocks would be stale: from before the change."""
        b = np.empty(2 * 65536, np.int16)
        end = time.time() + 3
        while time.time() < end:
            r = self.dev.readStream(self.st, [b], 65536, timeoutUs=200000)
            if 0 <= r.ret < 65536:
                return

    def block(self):
        buf = np.zeros(2 * self.n, np.int16)
        got, end = 0, time.time() + 5
        while got < self.n and time.time() < end:
            r = self.dev.readStream(self.st, [buf[2 * got:]], self.n - got, timeoutUs=200000)
            if r.ret > 0:
                got += r.ret
            elif r.ret < 0 and got:
                got = 0  # overflow mid-block: start again, the block must be contiguous
        if got < self.n:
            raise RuntimeError("receiver stopped delivering samples")
        return (buf[0::2] + 1j * buf[1::2]) / 32768.0

    def psd(self, k):
        """Mean power spectrum of k fresh blocks, in dBFS per bin (a tone of amplitude A reads 20 log A)."""
        self.drain()
        acc = 0
        for _ in range(k):
            acc = acc + np.abs(np.fft.fftshift(np.fft.fft(self.block() * self.w))) ** 2
        return 10 * np.log10(acc / k / np.sum(self.w) ** 2 + 1e-30)

    def peak(self, p, hz, span):
        m = np.abs(self.f - hz) < span
        i = np.flatnonzero(m)[np.argmax(p[m])]
        return self.f[i], p[i]

    def floor(self, p, hz):
        m = (np.abs(self.f - hz) > 20e3) & (np.abs(self.f - hz) < 60e3)
        return float(np.median(p[m]))

    def close(self):
        self.dev.deactivateStream(self.st)
        self.dev.closeStream(self.st)
