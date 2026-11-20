# S3 SDR bench

A LilyGO T-Display-S3 as a known 2.4 GHz signal source for wsdr receivers, using
[cyber-murmel/esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr): its `rfprobe` app
plays a tone at a set offset and amplitude, which these scripts measure through SoapySDR.

| Script | Does |
|---|---|
| `s3shell.py` | sends commands to the S3's shell |
| `tone.py` | measures one tone or a sweep of tones: level, floor, image, frequency error |
| `chanscan.py` | channel 1–13 occupancy, to pick a quiet channel |
| `decode_level.py` | numbered ESP-NOW frames from the S3 at falling amplitudes: how many each level decodes |
| `s3lab.py` | the shared S3 shell and receiver code |

They need `pyserial`, `numpy`, the SoapySDR Python bindings and
`SOAPY_SDR_PLUGIN_PATH=<wifi-sdr>/build-soapy`.

## The S3

esp-sdr-zephyr's multiapp image for the XIAO ESP32S3 runs unchanged on the T-Display-S3. Put an
antenna on its U.FL connector; a loose U.FL shows as a tone that comes and goes.

**Our build (v0.2.0, rfprobe at boot).** The stock v0.2.0 image boots a MicroPython launcher that
drives the XIAO's display and knob, so on the T-Display its USB shell answers only as a Python REPL.
Build it to start rfprobe at every boot instead:

```sh
west init -m https://github.com/cyber-murmel/esp-sdr-zephyr --mr v0.2.0 esp-sdr-ws && cd esp-sdr-ws
west update && west blobs fetch hal_espressif && west blobs fetch esp-sdr-zephyr
pip install -r modules/zephyr/scripts/requirements-base.txt -r bootloader/mcuboot/scripts/requirements.txt "esptool>=5.0.2"
# Zephyr SDK: modules/zephyr/SDK_VERSION, minimal bundle plus xtensa-espressif_esp32s3_zephyr-elf
west build --sysbuild -d build-multiapp -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/multiapp \
  -- -Dmultiapp_CONFIG_MULTIAPP_AUTOSTART=\"rfprobe\"
west dfu -d build-multiapp --esptool --storage    # from a running esp-sdr shell (it sends usj)
```

If the board runs something without that shell (the launcher's REPL: `import esdr; esdr.shell("usj")`,
or other firmware), put it in download mode and write the four files `west dfu` lists with
`esptool write-flash` (MCUboot at 0x0, the image at 0x20000, the slot trailer at 0x2bf000, the
FAT volume at 0x400000).

The prebuilt v0.1.1 image boots to the shell without rfprobe; start it with `app run rfprobe`
(`multiapp run` from v0.2.0) after every power cycle.

```sh
python3 s3shell.py "probe gain -1 0" "probe f 2437" "probe tone on 1000000 400" "probe tone off"
```

**TX step 0 only.** The bench radiates at TX step 0 (the lowest of 18 steps) with amplitudes up to
400. `s3lab.py` refuses higher steps (`MAX_STEP`).

`python3 s3shell.py usj` first also lets the ESP-NOW bench firmware back on: `cd ../espnow-bench && pio run -e tdisplays3 -t upload`.

## Frames: the wsdrtx firmware

`fw/wsdrtx` is a multiapp app for the S3 with rfprobe's `probe f`, `probe gain` and `probe tone`
(so `tone.py` works with it) plus `probe frames`: numbered ESP-NOW broadcasts, 802.11b DSSS at
1 or 2 Mb/s with the long preamble, from the made-up sender 02:57:53:44:52:01. The payload is the
ESP-NOW bench's (`WSDR`, a u32 counter, ...), so `test_core --wifi` tallies exactly which arrived.

```
probe frames <count> [period ms] [amp] [rate 1|2] [size]    # e.g. probe frames 200 5 400 1 10
```

It blocks until the burst is done (60 s at most) and prints what was sent and the counters. The
firmware itself holds the bench limits: TX step 0, amplitude 400.

How it works: `src/dsss.c` turns a frame into one phase per 1 µs symbol; at the DAC's 40 MS/s one
symbol is exactly 40 samples, so each sample is a table entry picked by the previous, current and
next symbol (5 states each: four phases and silence; 20 KB of DAC words, raised-cosine chips,
±8.25 MHz). `esp_sdr_tx_dac_play_gen()` plays one frame per call from that table; the generator
runs in internal RAM (`ESP_SDR_CODE(PACED_SAMPLE, ...)`) and fills a 408 µs bank in under 250 µs.

Build (the workspace above, then a one-line hook in multiapp's CMakeLists):

```sh
git -C esp-sdr-ws/esp-sdr-zephyr apply <wsdr-studio>/tools/s3-sdr/fw/multiapp-wsdrtx.patch
export WSDRTX_DIR=<wsdr-studio>/tools/s3-sdr/fw/wsdrtx
west build --sysbuild -d build-wsdrtx -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/multiapp \
  -- -Dmultiapp_CONFIG_MULTIAPP_AUTOSTART=\"wsdrtx\"
west dfu -d build-wsdrtx --esptool --storage
```

`fw/wsdrtx/host` checks the modulator on the PC: `frames.c` renders the same frames through the
same table, `frames.py` makes a 20 MS/s recording with noise and a clock offset for `test_core`.
Offline, ESP-NOW frames decode 100 % from 2 dB SNR in 20 MHz at 1 Mb/s and from about 6 dB at
2 Mb/s.

`fw/wsdrtx/src/main.c` is GPL-3.0-or-later (it builds on esp-sdr-zephyr's rfprobe); `dsss.[ch]`
and `host/` are GPL-2.0-or-later.

## Measuring

```sh
python3 tone.py --rx driver=wsdr,bus=6                  # +1 MHz tone, ~10 s
python3 tone.py --rx driver=wsdr,bus=6 --offsets sweep --guard --csv sweep.csv   # ~2 min
```

- **Read until caught up before every capture.** SoapyWsdr's ring keeps its oldest samples when it
  overflows (0.25 s at 40 MS/s: 0.5 s at 20 MS/s). After any pause in reading, for example while
  talking to the S3, the first blocks are from before the change. `Rx.psd` drains first; without
  it, tones come and go and the receiver looks deaf.
- `decode_level.py` needs a `test_core` built from current sources (ESP-NOW tallies came with
  #38): `cmake --build build --target test_core`.
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
