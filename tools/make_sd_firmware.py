#!/usr/bin/env python3
"""Packs a firmware image for the Drumboy Pro's stock bootloader, which installs it from the SD card at power-on (no ST-Link needed).

The format is the stock Firmware/Drumboy-Pro-Firmware-Builder.cpp's: "RW_DRUMBOY_PRO_FIRMWARE " (24 bytes), version major/minor (u16
each), the image size (u32, < 917504) and its CRC-32 (u32, zlib), then the image (linked for 0x08020000). Put the output in the card's
root; the bootloader checks it, flashes it, re-reads the flash against the CRC, and moves the file to System/Firmware.

  python3 tools/make_sd_firmware.py firmware/build/dboy_cathode.bin Drumboy_Pro_Fw_CATHODE.bin
"""
import struct, sys, zlib

MAX = 917504   # the bootloader's limit: 896 KB, the application flash


def pack(image: bytes, major=1, minor=1) -> bytes:
    if len(image) % 4:   # the bootloader writes the flash in 32-bit words (size / 4): pad the tail with erased-flash bytes
        image += b"\xff" * (4 - len(image) % 4)
    if not 0 < len(image) < MAX:
        raise SystemExit(f"image is {len(image)} bytes; the bootloader takes 1..{MAX - 1}")
    return b"RW_DRUMBOY_PRO_FIRMWARE " + struct.pack("<HHII", major, minor, len(image), zlib.crc32(image) & 0xFFFFFFFF) + image


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    name = dst.split("/")[-1]
    if not (name.startswith("Drumboy_Pro_Fw") and name.endswith(".bin")):
        raise SystemExit("the bootloader only looks for Drumboy_Pro_Fw*.bin in the card's root")
    if len(name) > 31:   # its FatFs has _MAX_LFN 32 and rejects a name with no room for the terminator, and then sees only DRUMBO~1.BIN
        raise SystemExit(f"{name} is {len(name)} characters; the bootloader skips any over 31, and says nothing")
    out = pack(open(src, "rb").read())
    open(dst, "wb").write(out)
    print(f"{dst}: {len(out) - 36} bytes of firmware, CRC {struct.unpack('<I', out[32:36])[0]:08x}")


if __name__ == "__main__":
    main()
