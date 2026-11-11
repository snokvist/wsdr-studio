#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds wsdr-zero for 64-bit ARM and packs it as a .deb for the M5Stack CardputerZero's launcher
# (APPLaunch): build-arm64/wsdr-zero_<version>-1_arm64.deb. See zero/README.md.
#
# Needs (Debian/Ubuntu, with the arm64 architecture enabled for the two :arm64 packages):
#   g++-aarch64-linux-gnu libusb-1.0-0-dev:arm64 libfftw3-dev:arm64 nlohmann-json3-dev
#   binutils-mipsel-linux-gnu xxd cmake pkg-config dpkg-dev
set -eu
cd "$(dirname "$0")/.."
B=build-arm64
cmake -S . -B $B -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build $B -j"$(nproc)" --target wsdr-zero
V=$(sed -n 's/^set(WSDR_ZERO_VERSION \(.*\))$/\1/p' CMakeLists.txt)
R=$B/deb
rm -rf $R
A=$R/usr/share/APPLaunch
install -D -m 755 $B/wsdr-zero $A/bin/wsdr-zero
aarch64-linux-gnu-strip $A/bin/wsdr-zero
install -D -m 644 zero/packaging/wsdr-zero.desktop $A/applications/wsdr-zero.desktop
install -D -m 644 zero/packaging/wsdr-zero.png $A/share/images/wsdr-zero.png
install -D -m 644 zero/packaging/70-wsdr.rules $R/usr/lib/udev/rules.d/70-wsdr.rules
install -D -m 644 zero/README.md $R/usr/share/doc/wsdr-zero/README.md
cat > $R/usr/share/doc/wsdr-zero/copyright <<EOF
wsdr-zero is part of wsdr-studio: https://github.com/snokvist/wsdr-studio
License: GPL-2.0-only (see /usr/share/common-licenses/GPL-2). It links wifi-sdr and devourer
(both GPL-2.0-only); libstdc++ is linked statically (GPL-3.0+ with the GCC Runtime Library Exception).
EOF
mkdir -p $R/DEBIAN
cat > $R/DEBIAN/control <<EOF
Package: wsdr-zero
Version: $V-1
Architecture: arm64
Maintainer: snokvist <snokvist@users.noreply.github.com>
Section: APPLaunch
Priority: optional
Depends: libc6 (>= 2.38), libusb-1.0-0, libfftw3-single3
Installed-Size: $(du -sk $R/usr | cut -f1)
Homepage: https://github.com/snokvist/wsdr-studio
Description: Wi-Fi dongle spectrum and waterfall for the CardputerZero
 Turns a Realtek RTL8812BU, RTL8812CU or RTL8812EU USB Wi-Fi dongle into a
 live spectrum receiver on the 320 x 170 screen: 2.5 and 5 MHz continuous,
 10 to 80 MHz gapped, 2.4 and 5 GHz. Receive only.
EOF
cat > $R/DEBIAN/postinst <<'EOF'
#!/bin/sh
# the adapters' udev rule takes effect for dongles that are already plugged in
set -e
if command -v udevadm >/dev/null 2>&1; then
  udevadm control --reload-rules || true
  udevadm trigger --subsystem-match=usb --attr-match=idVendor=0bda || true
fi
EOF
chmod 755 $R/DEBIAN/postinst
OUT=$B/wsdr-zero_$V-1_arm64.deb
dpkg-deb --root-owner-group --build $R $OUT
echo "built $OUT"
