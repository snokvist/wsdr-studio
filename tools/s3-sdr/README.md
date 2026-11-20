# S3 SDR bench

A LilyGO T-Display-S3 as a known 2.4 GHz signal source for wsdr receivers, using
[cyber-murmel/esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr): its `rfprobe` app
plays a tone at a set offset and amplitude, which these scripts measure through SoapySDR.

| Script | Does |
|---|---|
| `s3shell.py` | sends commands to the S3's shell |
| `tone.py` | measures one tone or a sweep of tones: level, floor, image, frequency error |
| `chanscan.py` | channel 1–13 occupancy, to pick a quiet channel |
| `s3lab.py` | the shared S3 shell and receiver code |

They need `pyserial`, `numpy`, the SoapySDR Python bindings and
`SOAPY_SDR_PLUGIN_PATH=<wifi-sdr>/build-soapy`.

## The S3

The v0.1.1 release image for the XIAO ESP32S3 (`esp-sdr-multiapp-v0.1.1-xiao_esp32s3.bin`) runs
unchanged on the T-Display-S3. Put an antenna on its U.FL connector; a loose U.FL shows as a tone that
comes and goes. To flash, first hand the USB port to the ROM, then write the image at 0:

```sh
python3 s3shell.py usj
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 esp-sdr-multiapp-v0.1.1-xiao_esp32s3.bin
```

After every power cycle, start rfprobe (it reboots; the port is back in about 8 s):

```sh
python3 s3shell.py "app run rfprobe"
python3 s3shell.py "probe gain -1 0" "probe f 2437" "probe tone on 1000000 400" "probe tone off"
```

**TX step 0 only.** The bench radiates at TX step 0 (the lowest of 18 steps) with amplitudes up to
400. `s3lab.py` refuses higher steps (`MAX_STEP`).

The `usj` step also restores the ESP-NOW bench firmware: `cd ../espnow-bench && pio run -e tdisplays3 -t upload`.

## Measuring

```sh
python3 tone.py --rx driver=wsdr,bus=6                  # +1 MHz tone, ~10 s
python3 tone.py --rx driver=wsdr,bus=6 --offsets sweep --guard --csv sweep.csv   # ~2 min
```

- **Read until caught up before every capture.** SoapyWsdr's ring keeps its oldest samples when it
  overflows (0.25 s at 40 MS/s: 0.5 s at 20 MS/s). After any pause in reading, for example while
  talking to the S3, the first blocks are from before the change. `Rx.psd` drains first; without
  it, tones come and go and the receiver looks deaf.
- `--guard` re-measures a +1 MHz reference every 4 tones and stops if it fades.
- After a replug, an 8812BU can come back on USB 2 (5 MS/s at most); `Rx` stops with an error then.
  `wsdr_usbmode --bus N --port 1 --to 3` (wifi-sdr) switches it back.

## Reference results (2026-11-20)

8812BU, gain 30, channel 6, the S3 about 10 cm away, step 0:

- +1 MHz tone, 2^20-point bins: +11 to +14 dB over the floor at amplitude 40, +28 to +37 at 400.
- The S3's clock: +2 ppm against the 8812BU, drifting a few hundred Hz as the boards warm.
- Sweep at 20 MS/s, amplitude 400, S3 TX and 8812BU RX together: flat within 2 dB from ±2 to
  ±6 MHz (about −47 dBFS); −6 dB at ±1 MHz and −11 dB at ±0.5 MHz; −10 dB at ±9.5 MHz; a 12 dB dip at
  +7 MHz only. Image −23 to −30 dBc.
- The 8812BU's floor is its own front-end noise from gain 25 up: more gain doesn't help.
