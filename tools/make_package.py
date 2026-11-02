#!/usr/bin/env python3
"""Wrap a raw application .bin in the Drumboy Pro SD-card update package.

Layout (little-endian), identical to the stock Drumboy-Pro-Firmware-Builder.cpp:
  24 bytes  ASCII "RW_DRUMBOY_PRO_FIRMWARE "
   2 bytes  version major      2 bytes  version minor
   4 bytes  payload size       4 bytes  CRC-32 (zlib / 0xEDB88320) of the payload
   N bytes  payload
"""
import argparse
import struct
import sys
import zlib

MAGIC = b"RW_DRUMBOY_PRO_FIRMWARE "
# The app region is sectors 1-7 of the 1 MB flash: 0x08020000..0x080FFFFF.
APP_REGION_BYTES = 896 * 1024


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", help="raw application image (objcopy -O binary)")
    ap.add_argument("out", help="package to write, e.g. Drumboy_Pro_Fw_01_02.bin")
    ap.add_argument("--major", type=int, required=True)
    ap.add_argument("--minor", type=int, required=True)
    args = ap.parse_args()

    payload = open(args.bin, "rb").read()
    if len(payload) > APP_REGION_BYTES:
        print(f"error: image is {len(payload)} bytes, the app region holds {APP_REGION_BYTES}", file=sys.stderr)
        return 1
    # First word of a valid image is the initial stack pointer, inside RAM (0x2000_0000..0x3FFF_FFFF).
    sp = struct.unpack_from("<I", payload, 0)[0]
    if not 0x20000000 <= sp <= 0x3FFFFFFF:
        print(f"error: initial SP 0x{sp:08X} is not in RAM; is this a raw image linked for 0x08020000?", file=sys.stderr)
        return 1

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = MAGIC + struct.pack("<HHII", args.major, args.minor, len(payload), crc)
    open(args.out, "wb").write(header + payload)
    print(f"{args.out}: v{args.major}.{args.minor}, {len(payload)} bytes, crc32 {crc:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
