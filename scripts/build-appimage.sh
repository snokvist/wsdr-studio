#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build wsdr-studio-x86_64.AppImage.
#   - web UI: built on the host (npm); static files, architecture independent
#   - backend: built inside Ubuntu 22.04 (needs glibc >= 2.34 at run time) for wide compatibility
#   - libraries: linuxdeploy bundles FFTW; libusb is added explicitly (linuxdeploy's exclude list
#     skips it); libstdc++ is linked statically; libudev stays the host's (it must match the host's udev)
#   - desktop shell: the pinned Electron release with desktop/ as its app; AppRun starts it, and it
#     starts the backend (Electron uses the host's usual desktop libraries: GTK 3, NSS, ALSA, GBM)
#   - AppImage: appimagetool with the pinned static type-2 runtime (no libfuse2 needed on the host)
# Tools are pinned by version and SHA-256. Output: build-appimage/wsdr-studio-x86_64.AppImage
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$R/build-appimage"
TOOLS="$OUT/tools"
mkdir -p "$TOOLS"

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
ELECTRON_VERSION=44.7.0
ELECTRON_URL=https://github.com/electron/electron/releases/download/v$ELECTRON_VERSION/electron-v$ELECTRON_VERSION-linux-x64.zip
ELECTRON_SHA=3ae7d5bdad61c664486c6ab61361dd084d064c8c08af8d13a687096e18ce555a
fetch() { # url sha file
  [ -f "$3" ] || curl -fsSL -o "$3" "$1"
  echo "$2  $3" | sha256sum -c --quiet - || { echo "checksum mismatch: $3" >&2; rm -f "$3"; exit 1; }
  chmod +x "$3"
}
fetch "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA" "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$TOOLS/appimagetool-x86_64.AppImage"
fetch "$RUNTIME_URL" "$RUNTIME_SHA" "$TOOLS/runtime-x86_64"
fetch "$ELECTRON_URL" "$ELECTRON_SHA" "$TOOLS/electron-v$ELECTRON_VERSION-linux-x64.zip"

[ -f "$R/external/wifi-sdr/external/devourer/CMakeLists.txt" ] || {
  echo "submodules missing: git submodule update --init external/wifi-sdr && git -C external/wifi-sdr submodule update --init external/devourer" >&2
  exit 1
}

echo "== web UI"
(cd "$R/web" && npm ci --no-audit --no-fund --silent && npm run build --silent)

DOCKER=docker
docker ps >/dev/null 2>&1 || DOCKER="sudo docker"
echo "== backend + AppImage (ubuntu:22.04)"
$DOCKER run --rm -v "$R":/src:ro -v "$OUT":/out -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e ELECTRON_ZIP="/out/tools/electron-v$ELECTRON_VERSION-linux-x64.zip" ubuntu:22.04 bash -euo pipefail -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends cmake g++ make pkg-config libusb-1.0-0-dev libfftw3-dev \
      libboost-dev nlohmann-json3-dev binutils-mipsel-linux-gnu xxd file ca-certificates unzip >/dev/null
  # (libstdc++ linked in: the host's may be older than GCC 12's, e.g. RHEL/Alma/Rocky 9)
  cmake -S /src -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" >/dev/null
  cmake --build /tmp/b -j"$(nproc)" --target wsdr-studiod test_core
  /tmp/b/test_core
  rm -rf /tmp/AppDir && DESTDIR=/tmp/AppDir cmake --install /tmp/b >/dev/null
  test -f /tmp/AppDir/usr/share/wsdr-studio/web/index.html
  cd /tmp
  export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
  /out/tools/linuxdeploy-x86_64.AppImage --appdir /tmp/AppDir \
      --executable /tmp/AppDir/usr/bin/wsdr-studiod \
      --library /usr/lib/x86_64-linux-gnu/libusb-1.0.so.0 \
      --desktop-file /src/packaging/wsdr-studio.desktop --icon-file /src/packaging/wsdr-studio.svg \
      --custom-apprun /src/packaging/AppRun >/dev/null
  rm -f /tmp/AppDir/usr/lib/libudev.so*
  test -f /tmp/AppDir/usr/lib/libusb-1.0.so.0
  # Electron goes in after linuxdeploy (its prebuilt binaries must not be touched)
  E=/tmp/AppDir/usr/lib/wsdr-studio/electron
  mkdir -p "$E" && unzip -q "$ELECTRON_ZIP" -d "$E"
  mv "$E/electron" "$E/wsdr-studio"
  rm -f "$E/resources/default_app.asar"
  mkdir -p "$E/resources/app"
  cp /src/desktop/main.js /src/desktop/preload.js /src/desktop/package.json /src/desktop/icon.png "$E/resources/app/"
  test -x /tmp/AppDir/AppRun && test -x "$E/wsdr-studio"
  # the menu entry starts the app (AppRun), not the bare backend
  ln -s ../../AppRun /tmp/AppDir/usr/bin/wsdr-studio
  sed -i "s/^Exec=wsdr-studiod$/Exec=wsdr-studio/" /tmp/AppDir/*.desktop /tmp/AppDir/usr/share/applications/*.desktop
  grep -q "^Exec=wsdr-studio$" /tmp/AppDir/usr/share/applications/wsdr-studio.desktop
  /out/tools/appimagetool-x86_64.AppImage --no-appstream --runtime-file /out/tools/runtime-x86_64 \
      /tmp/AppDir /out/wsdr-studio-x86_64.AppImage >/dev/null
  chown "$HOST_UID:$HOST_GID" /out/wsdr-studio-x86_64.AppImage
'
ls -la "$OUT/wsdr-studio-x86_64.AppImage"
