# SPDX-License-Identifier: GPL-2.0-only
"""Send commands to the S3's Zephyr shell and print the replies.

  s3shell.py "multiapp run rfprobe"            # reboots into rfprobe (v0.1.x: "app run")
  s3shell.py "probe gain -1 0" "probe f 2437"
  s3shell.py usj                               # hand the USB port to the ROM for esptool
"""
import argparse
import sys

import serial

from s3lab import S3


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the S3's shell (default: the one esp-sdr board's by-id link)")
    ap.add_argument("--wait", type=float, default=3.0, help="longest wait for each reply, s")
    ap.add_argument("commands", nargs="+")
    a = ap.parse_args()
    s3 = S3(a.port)
    for c in a.commands:
        try:
            sys.stdout.write(s3.cmd(c, a.wait))
        except serial.SerialException:  # app run and usj reboot or detach the port
            print(f"\n[{c}: the port went away, as expected for a reboot]")
            return 0
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
