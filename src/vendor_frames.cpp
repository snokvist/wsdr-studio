// SPDX-License-Identifier: GPL-2.0-only
#include "vendor_frames.h"

#include <algorithm>

namespace wifi {
namespace {

// ESP-NOW (Espressif; ESP-IDF docs, api-reference/network/esp_now): an Action frame, not to or from a
// distribution system, addr1 its destination, addr2 its source, addr3 broadcast. Its body: category
// 127 (vendor-specific), OUI 18:fe:34, 4 random bytes, then vendor elements [221, length, 18:fe:34,
// type 4, version byte (bits 3-0 the version; v2: bit 4 "more data"), up to 250 bytes of payload]:
// one in v1, up to 6 chained by "more data" in v2 (1470 bytes). Encrypted (CCMP, to one peer only),
// the body is ciphertext: what is left to know it by is that addressing.
bool espnow(const uint8_t *m, size_t n, Frame &f) {
  // (the Order bit on a management frame adds an HT control field: everything after the header moves)
  if (f.subtype != 13 || (m[1] & 0x03) || (m[1] & 0x80))
    return false;
  const bool to_all = std::all_of(m + 16, m + 22, [](uint8_t b) { return b == 0xff; });
  if (f.prot) {
    // (addr1 a group address: ESP-NOW does not encrypt those; the body: a CCMP header with its ExtIV
    // bit, the smallest ESP-NOW body (category, OUI, random, one element's head: 15 bytes), the MIC)
    if (!to_all || (m[4] & 1) || n < 24 + 8 + 15 + 8 || !(m[24 + 3] & 0x20))
      return false;
    f.vendor.name = "espnow";
    f.vendor.encrypted = true;
    return true;
  }
  static constexpr uint8_t kHead[4] = {127, 0x18, 0xfe, 0x34};
  if (n < 32 + 7 || !std::equal(kHead, kHead + 4, m + 24))
    return false;
  std::vector<uint8_t> data;
  int version = -1;
  for (size_t p = 32, k = 0; k < 6 && p + 7 <= n; k++) {
    const size_t l = m[p + 1];
    if (m[p] != 221 || l < 5 || p + 2 + l > n || m[p + 2] != 0x18 || m[p + 3] != 0xfe || m[p + 4] != 0x34 || m[p + 5] != 4)
      break;
    const uint8_t ver = m[p + 6];
    if (version < 0)
      version = ver & 0x0f;
    const size_t take = std::min(l - 5, kVendorMax - data.size());
    data.insert(data.end(), m + p + 7, m + p + 7 + take);
    p += 2 + l;
    if (!(ver & 0x10) || data.size() >= kVendorMax) // (no more data)
      break;
  }
  if (version < 0) // no ESP-NOW element
    return false;
  f.vendor.name = "espnow";
  f.vendor.version = version;
  f.vendor.data = std::move(data);
  return true;
}

constexpr bool (*kParsers[])(const uint8_t *, size_t, Frame &) = {espnow};

} // namespace

void parse_vendor(const uint8_t *mac, size_t n, Frame &f) {
  if (n < 24)
    return;
  for (const auto parse : kParsers)
    if (parse(mac, n, f))
      return;
}

} // namespace wifi
