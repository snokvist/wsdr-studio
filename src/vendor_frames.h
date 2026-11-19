// SPDX-License-Identifier: GPL-2.0-only
// Vendor protocols carried in 802.11 management frames: a parser per protocol, tried in turn on every
// management frame decoded whole. The one that recognises a frame fills Frame::vendor (its name,
// version, payload), the one part of a frame that is kept. ESP-NOW is the first; another protocol
// (Remote ID in beacons or NAN frames, say) is one more parser in vendor_frames.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wifi.h"

namespace wifi {

constexpr size_t kVendorMax = 1470; // payload bytes kept at most (ESP-NOW v2's)

// mac: the MAC frame without its FCS (n bytes, at least 24); f: its header already read (parse_mac)
void parse_vendor(const uint8_t *mac, size_t n, Frame &f);

} // namespace wifi
