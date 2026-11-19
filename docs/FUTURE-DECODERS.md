# Possible future decoders

_2026-11-19. Deferred ideas, not planned work. Each one is something that actually transmits inside the range our
adapters tune, and that wsdr-studio could show or decode._

## What we can reach

The adapters tune **2.4 GHz channels 1–14** and **5 GHz channels 16–233, roughly 5.08–6.17 GHz**. They stream at
2.5–40 MS/s, and 80 MS/s on the 8812BU over USB 3.

Broadcast FM and AM, DAB, ADS-B, AIS, weather satellites, GPS, DECT and 868 MHz LoRa are all **out of range**. They would
need an external downconverter.

## How an add-in slots in

Two mechanisms:
- **A tap kind** (`src/tap.h`): a decoder that follows one signal the user clicked. It is a `TapKind` plus a
  `TapDecoder`, one entry in the Engine's tap list, and a panel in `web/src/taps/`. The analog video picture
  (`src/video.cpp`) is the worked example.
- **The signal finder** (`src/signals.cpp`) or **the Wi-Fi/BLE paths** (`src/wifi.cpp`): for things that should be found
  and marked without being asked for, the way Bluetooth, BLE adverts and microwave ovens are today.

## The ideas, in suggested order

### 1. Audio from analog video transmitters (a tap kind)

- **What:** analog FPV transmitters and analog AV senders carry their audio as FM on a subcarrier, usually 6.0 or
  6.5 MHz above the picture carrier.
- **Where:** 5.65–5.95 GHz (FPV bands A/B/E/F/R) and 2.4 GHz AV senders.
- **Work:** a new `audio` tap kind next to `video`. The decoder mixes to the subcarrier, filters about 150 kHz,
  demodulates the FM, de-emphasises and decimates to 48 kHz. The page plays it with WebAudio, via a new frame magic
  and a panel.
- **Why first:** it's the "FM audio receiver" slot the tap layer was built for, it proves the layer with a second kind,
  and it's small.
- **Testing:** we have no transmitter. Use synthetic subcarriers mixed into the video simulation, or a recording.
  Ask gilankpam whether his VTX recordings carry audio.
- **Later:** the same kind could take a plain narrowband FM or AM signal anywhere in range, such as an analog baby
  monitor.

### 2. Drone Remote ID (Wi-Fi/BLE path)

- **What:** EU (ASD-STAN EN 4709-002) and US (ASTM F3411) rules require drones to broadcast their serial number,
  position, height, speed and the operator's location. It's meant to be received by anyone.
- **Where:** Wi-Fi beacons and Wi-Fi NAN frames (mostly 2.437 GHz; some on 5 GHz), and BLE advertisements
  (service data UUID 0xFFFA).
- **Work:** mostly parsing. We already decode Wi-Fi frames and BLE legacy advertising. Add the Remote ID message
  parser and a drone list or panel, with positions.
- **Limits:** the BLE 5 long-range form (Coded PHY) probably isn't decodable with our BLE path. The Wi-Fi forms and
  BLE 4 legacy adverts are.

### 3. Pulsed radar (signal finder)

- **What:** C-band weather radars around 5.6 GHz (5600–5650 MHz) and other pulsed radars in the 5 GHz band.
- **Work:** a finder kind that spots short, strong, wideband pulses and reports their width, repetition rate and
  the antenna's rotation period (the pulses come back each sweep). It marks them in the waterfall. It's
  essentially Wi-Fi's DFS radar detection.
- **Check first:** scan 5600–5650 MHz at the desk and see whether a radar is in view.

### 4. V2X car messages: ITS-G5 / 802.11p (Wi-Fi path, larger)

- **What:** cars and roadside units broadcast short safety messages:
  - **CAM:** position, speed, heading, acceleration and size, up to 10 times a second.
  - **DENM:** hazard warnings.
  - **SPATEM/MAPEM:** traffic-light phase and junction layout.
  - Messages are signed, not encrypted.
- **Where:** 5855–5925 MHz in 10 MHz channels. The control channel is ch 180, 5900 MHz. All of it is in our range.
- **Work:**
  - The radio format is 802.11a OFDM at half clock (10 MHz channels, 3–27 Mb/s), so the Wi-Fi decoder needs a
    half-rate mode.
  - Above it: the GeoNetworking and BTP headers, then the ETSI messages, which are ASN.1 UPER. A generated ASN.1
    decoder handles CAM and DENM.
  - A panel lists the stations, with a small map.
- **Who transmits (late 2026):**
  - In Europe it's almost only **Volkswagen**. Over 2 million VW cars have been built with Car2X (ITS-G5): Golf 8
    from late 2019, ID.3/4/5/7, ID. Buzz, Multivan T7, Tiguan III, Passat B9, T-Roc II, and as an option in some
    Cupras. Since 2024/25 it's often optional rather than standard, except on the ID.7.
  - Škoda, Audi and Seat in Europe, and Volvo, Mercedes, BMW and others, don't use direct radio. Their "car to X"
    features go over the mobile network.
- **Roadside units:** C-Roads deployments on German and Austrian motorways, and city traffic lights and transit
  priority (Vienna, Graz, Klagenfurt, Berlin, Hamburg and others). I found no Swedish deployment.
- **Hobbyists already receive it:** a 2026 Grazer Linuxtage talk uses an ESP32-C5, from a few hundred metres, and
  the OpenTrafficMap project maps it. So reception is practical.
- **Expectation in Sweden:** passing VW ID./Golf 8 cars near a road, intermittently. No infrastructure. LTE-V2X
  (C-V2X), used by Chinese makes, is a different radio (SC-FDMA) and not decodable by this path.

### Smaller ones

- **Zigbee / Thread (IEEE 802.15.4)** at 2405–2480 MHz: O-QPSK, 2 MHz channels. Device discovery and
  addresses.
- **OpenIPC / wfb-ng FPV links:** they are Wi-Fi frames, so they could be recognised and labelled by their channel
  id. The video is encrypted without the link's key.
- **5.8 GHz road-toll DSRC** (CEN DSRC, 5795–5815 MHz): roadside beacons with ASK downlink. Niche.

## Sources (V2X)

- Car 2 Car Communication Consortium, Euro NCAP direct V2X Q&A, Sept 2026:
  https://www.car-2-car.org/fileadmin/documents/General_Documents/EuroNCAP__20260904_final.pdf
- OpenTrafficMap wiki, "Verbreitung" (as of Oct 2026): https://wiki.opentrafficmap.org/verbreitung
- "C-ITS: Mit einem ESP32 Ampeln, Straßenbahnen und Autos tracken", GLT26, 2026-04-11:
  https://media.ccc.de/v/glt26-688-c-its-mit-einem-esp32-ampeln-straenbahnen-und-autos-tracken
- Vector, "V2X on Europe's Roads": https://www.vector.com/us/en/news/news/v2x-on-europes-roads-the-future-or-already-a-reality/
