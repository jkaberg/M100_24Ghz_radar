#!/usr/bin/env python3
"""Wrap a patched Telink image (tl_check_fw.py: padded, size at 0x18, magic 5d02 at 6, CRC32) in a
Zigbee OTA file. Same layout as romasku/tuya-zigbee-switch src/telink/make_ota.py: 56-byte header,
one sub-element (tag 0) holding the image. No bootloader is appended (A/B slot scheme).

  package_ota.py <image.bin> <out.zigbee> --manuf 0x1141 --type 0xd3a3 [--version 0x...]
"""
import argparse
import struct

ap = argparse.ArgumentParser()
ap.add_argument('image')
ap.add_argument('out')
ap.add_argument('--manuf', type=lambda x: int(x, 0), required=True)
ap.add_argument('--type', type=lambda x: int(x, 0), required=True)
ap.add_argument('--version', type=lambda x: int(x, 0))
ap.add_argument('--header-string', default='Telink OTA Image')
a = ap.parse_args()

img = open(a.image, 'rb').read()
if img[6:8] != b'\x5d\x02':
    raise SystemExit('image not patched (run tl_check_fw.py first)')
version = a.version if a.version is not None else int.from_bytes(img[2:6], 'little')
hs = a.header_string.encode()[:31].ljust(32, b'\0')
total = 56 + 6 + len(img)
hdr = struct.pack('<I5HIH32sI', 0x0BEEF11E, 0x100, 56, 0, a.manuf, a.type, version, 2, hs, total)
with open(a.out, 'wb') as f:
    f.write(hdr + struct.pack('<HI', 0, len(img)) + img)
print(f'{a.out}: manuf 0x{a.manuf:04x} type 0x{a.type:04x} version 0x{version:08x} size {total}')
