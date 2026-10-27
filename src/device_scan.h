// SPDX-License-Identifier: GPL-2.0-only
// Finds supported adapters without opening them: USB descriptors, link speed, the host controller they
// hang off (sysfs), the kernel driver bound to the Wi-Fi interface, and whether this user can open them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace studio {

struct DeviceInfo {
  uint16_t pid = 0;
  std::string model;      // "RTL8812BU" / "RTL8812CU"
  std::string chip;       // "RTL8822B" / "RTL8822C"
  int bus = 0;
  std::string port;       // "1" or "1.2"
  std::string sysfs;      // e.g. "6-1"
  int speed_mbps = 0;     // 480 / 5000 / ...
  int devnum = 0;         // changes on every (re-)enumeration
  std::string controller; // PCI address of the host controller, e.g. "0000:67:00.3"
  std::string driver;     // kernel driver bound to interface 0, empty if none
  bool can_open = false;  // the device node is readable and writable by us
  double max_msps = 0;    // what the link allows continuously (40 on USB 3 for the 8812BU, else 5)
  double gapped_msps = 0; // widest gapped span: 40 (8812CU), 80 (8812BU)
};

std::vector<DeviceInfo> scan_devices();

} // namespace studio
