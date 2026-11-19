# ESP-NOW bench firmware

A sender of numbered ESP-NOW frames and a receiver that counts them, for checking what wsdr-studio
decodes. PlatformIO + Arduino, with the board envs of snokvist/esp32-supermini-projects:
`esp32c3_supermini` (ESP32-C3 SuperMini) and `tdisplays3` (LilyGO T-Display-S3).

## Build and flash

```sh
cd tools/espnow-bench
pio run -e esp32c3_supermini -t upload --upload-port /dev/ttyACM0   # or -e tdisplays3
pio device monitor -b 115200
```

The platform's Arduino core 2.x (ESP-IDF 4.4) speaks ESP-NOW v1 (payloads up to 250 bytes) and sets
one rate for all ESP-NOW frames. On a core 3.x / IDF 5.4+ build, v2 (1470 bytes) and per-peer rates
work too.

## Commands (one a line, 115200 baud)

| Command | Does |
|---|---|
| `role tx\|rx` | sender (default) or receiver |
| `chan N` | Wi-Fi channel, 1–13 (default 6) |
| `mode bcast\|ucast\|absent\|enc` | broadcast, unicast to `peer` (ACKed), unicast to nobody (every frame retried), encrypted unicast to `peer` |
| `peer MAC\|none` | on the sender: the receiver; on the receiver: the sender, as an encrypted peer (needed to decrypt `enc`; `none` removes it, so plain frames from it are taken) |
| `rate 1L\|2L\|2S\|11L\|6M\|MCS0` | PHY rate (1L, 1 Mb/s long preamble, is ESP-NOW's default) |
| `size N` | payload bytes, 10–250 |
| `hz N` | frames a second |
| `txpower dBm` | transmit power, 2–20 dBm (some C3 SuperMini boards transmit properly only around 8.5) |
| `go [N]` / `stop` | send N frames (or until stop) |
| `stats` / `reset` / `help` | this board's MAC, settings and counts; zero them; the list |

Payload: `"WSDR"`, a u32 counter (little-endian), u8 mode, u8 rate index, then filler bytes.

## Checking wsdr-studio against it

Record the sender's channel in wsdr-studio (20 or 40 MS/s, gap-free) while it sends, then:

```sh
build/test_core --wifi recording.sigmf-data
```

It prints every ESP-NOW sender → destination with the counters it decoded and the ones it missed:
`ESP-NOW 24:58:7c:.. -> ff:ff:ff:ff:ff:ff: 200 frames (v1, ...); counters 0..199: 200 of 200 (100.0 %)`.
With `ESPNOW_TIMES=1` it also lists each numbered frame's time and SNR, to find a missed one in the samples.

What wsdr-studio decodes whole: 1 and 2 Mb/s 802.11b with the long preamble, and 6 Mb/s OFDM. `2S`, `11L`
and `MCS0` frames are seen without their payload. An encrypted frame is named ESP-NOW by its addressing only.

## Notes from the first bench (2026-11-19)

- **A C3 SuperMini that transmits badly.** One of ours sent almost nothing at the default and 19.5 dBm: a T-Display-S3 next to it received 18 of 81 frames, wsdr-studio 1 of 100. At `txpower 5` it was 100 of 100 for both. Its receiver was fine at every setting. A unicast sender only sees ACKs from it at that low setting.
- **No ACK, no data retry.** An ESP32 whose unicast frame is not acknowledged sends RTS frames (about 32), not the data again.
- **ACKs at 6 Mb/s.** The C3 acknowledges 1 Mb/s ESP-NOW with 6 Mb/s OFDM ACKs.

