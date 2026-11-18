# kepyur's analog video decoder (vendored)

`video.h`, `video.cpp`, `log.h` and `log.cpp` are copied unchanged from `native/dsp/` of
[kepyur](https://github.com/gilankpam/kepyur) by Gilang (gilankpam), the Android analog FPV receiver
built on wifi-sdr, at commit `eb0169fdcf73aea1a1363a5a7cc9d9e962942c8c` (2026-11-18). kepyur is
GPL-2.0 (its README), as wsdr-studio is.

It turns IQ from an analog FPV video transmitter (FM composite video) into pictures: a DC block and mixer
to the carrier, a halfband to 20 MS/s, the FM discriminator, sync (line and field), and the colour
decoder (NTSC and PAL; black and white when the colour burst is too weak). Measured in kepyur on a real
R4 NTSC transmitter: 96.5 % of lines locked, the picture holding to about 5 dB CNR.

wsdr-studio uses it through `src/video.cpp` (`kp::FrontEnd`). To update, copy the four files again from
a newer kepyur commit and change the commit above; keep them unchanged here so that is all an update takes.
