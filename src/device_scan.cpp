// SPDX-License-Identifier: GPL-2.0-only
#include "device_scan.h"

#include <libusb.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "receiver.h"

namespace studio {

namespace fs = std::filesystem;

namespace {
std::string read_line(const fs::path &p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return s;
}
} // namespace

std::vector<DeviceInfo> scan_devices() {
  std::vector<DeviceInfo> out;
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0)
    return out;
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != 0x0bda ||
        (d.idProduct != wsdr::kPid8812bu && d.idProduct != wsdr::kPid8812cu))
      continue;
    DeviceInfo info;
    info.pid = d.idProduct;
    const bool cu = d.idProduct == wsdr::kPid8812cu;
    info.model = cu ? "RTL8812CU" : "RTL8812BU";
    info.chip = cu ? "RTL8822C" : "RTL8822B";
    info.bus = libusb_get_bus_number(list[i]);
    uint8_t ports[8];
    const int np = libusb_get_port_numbers(list[i], ports, sizeof ports);
    for (int k = 0; k < np; k++)
      info.port += (k ? "." : "") + std::to_string(ports[k]);
    info.sysfs = std::to_string(info.bus) + "-" + info.port;
    const fs::path dev = fs::path("/sys/bus/usb/devices") / info.sysfs;
    info.speed_mbps = std::atoi(read_line(dev / "speed").c_str());
    info.devnum = std::atoi(read_line(dev / "devnum").c_str());
    std::error_code ec;
    // /sys/bus/usb/devices/6-1 -> .../0000:67:00.3/usb6/6-1: the PCI device above the root hub.
    for (fs::path p = fs::canonical(dev, ec); !ec && p.has_parent_path() && p != p.root_path();
         p = p.parent_path()) {
      const std::string name = p.filename().string();
      if (name.size() == 12 && name[4] == ':' && name[7] == ':' && name[10] == '.') {
        info.controller = name;
        break;
      }
    }
    const fs::path drv = dev / (info.sysfs + ":1.0") / "driver";
    if (fs::exists(drv, ec))
      info.driver = fs::read_symlink(drv, ec).filename().string();
    char node[64];
    std::snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", info.bus, libusb_get_device_address(list[i]));
    info.can_open = access(node, R_OK | W_OK) == 0;
    info.max_msps = !cu && info.speed_mbps >= 5000 ? 40 : 5;
    // gapped spans wider than the link carries: 40 MS/s over USB 2 (and the 8812CU), 80 MS/s on an
    // 8812BU on USB 3 (wifi-sdr evidence/gapped-80msps.txt)
    info.gapped_msps = cu || info.speed_mbps < 5000 ? 40 : 80;
    out.push_back(info);
  }
  if (list)
    libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return out;
}

} // namespace studio
