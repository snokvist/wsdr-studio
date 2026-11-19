# ESP-NOW bench firmware

A sender of numbered ESP-NOW frames and a receiver that counts them, for checking what wsdr-studio
decodes. One ESP-IDF project for the ESP32-C3 and ESP32-S3; it is driven from the serial console.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
cd tools/espnow-bench
idf.py set-target esp32c3        # or esp32s3
# Boards on the chip's own USB (Espressif 303a:1001) want the console there:
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usbjtag" build
idf.py -p /dev/ttyACM0 flash monitor
```

ESP-NOW v2 (payloads up to 1470 bytes) needs ESP-IDF 5.4 or later.

## Commands

| Command | Does |
|---|---|
| `role tx\|rx` | sender (default) or receiver |
| `chan N` | Wi-Fi channel, 1–13 (default 6) |
| `mode bcast\|ucast\|absent\|enc` | broadcast, unicast to `peer` (ACKed), unicast to nobody (every frame retried), encrypted unicast |
| `peer MAC` | on the sender: the receiver; on the receiver: the sender (needed to decrypt `enc`) |
| `rate 1L\|2L\|2S\|11L\|6M\|MCS0` | PHY rate (1L, 1 Mb/s long preamble, is ESP-NOW's default) |
| `size N` | payload bytes, 10–250 (1470 on v2) |
| `hz N` | frames a second |
| `go [N]` / `stop` | send N frames (or until stop) |
| `stats` / `reset` | this board's MAC, settings and counts; zero them |

Payload: `"WSDR"`, a u32 counter (little-endian), u8 mode, u8 rate index, then filler bytes.

## Checking wsdr-studio against it

Record the channel in wsdr-studio (20 or 40 MS/s, gap-free) while the sender runs. Then:

```sh
build/test_core --wifi recording.sigmf-meta
```

It prints every ESP-NOW sender → destination with the counters it decoded, so the share received is
exact: `ESP-NOW 24:0a:c4:.. -> ff:ff:ff:ff:ff:ff: 400 frames (v1, ...); counters 0..399: 400 of 400 (100.0 %)`.

What wsdr-studio decodes whole: 1 and 2 Mb/s 802.11b with the long preamble, and 6 Mb/s OFDM. `2S`, `11L`
and `MCS0` are seen as frames without their payload. An encrypted frame is named ESP-NOW by its addressing only.
