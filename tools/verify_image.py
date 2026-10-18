#!/usr/bin/env python3
"""Checks on the raw firmware image (out/<name>.bin after tl_check_fw.py) for the A/B slot scheme.

- Telink header: 'KNLT' at +8, size at +0x18 == file size, OTA magic 5d02 at +6, CRC32 valid.
- Fits one slot: <= 0x40000 bytes (slots at 0x0 and 0x40000, NV from 0x80000).
- Marker aliasing: the ROM boots the first slot with 'KNLT' at +8, and the code checks markers at
  0x8008 (stock bootloader-mode app), 0x20008 ("small OTA" slot). Wherever this image sits in flash
  (slot 0x0, slot 0x40000, 0x8000 when started by the Tuya bootloader, 0x70000 as the leftover of
  the stock download), its own bytes must not put a marker on any of those addresses, or a later
  boot could jump into the middle of data. Also checks the single byte 0x4B in case the ROM only
  looks at the first byte.
"""
import binascii
import struct
import sys

KNLT = b'KNLT'
PLACEMENTS = {0x0: 'slot A', 0x40000: 'slot B', 0x8000: 'Tuya bootloader app', 0x70000: 'stock download leftover'}
CHECKED = [0x8008, 0x20008, 0x40008, 0x80008]


def main():
    d = open(sys.argv[1], 'rb').read()
    errs, warns = [], []

    def check(cond, msg, warn=False):
        print(('ok    ' if cond else ('WARN  ' if warn else 'FAIL  ')) + msg)
        if not cond:
            (warns if warn else errs).append(msg)

    check(d[8:12] == KNLT, "'KNLT' at +8")
    check(d[6:8] == b'\x5d\x02', 'OTA magic 5d02 at +6')
    size = struct.unpack_from('<I', d, 0x18)[0]
    check(size == len(d), f'size field 0x{size:x} == file size 0x{len(d):x}')
    crc = struct.unpack_from('<I', d, len(d) - 4)[0]
    check((binascii.crc32(d[:-4]) ^ 0xFFFFFFFF) & 0xFFFFFFFF == crc, 'CRC32')
    check(len(d) <= 0x40000, f'fits one A/B slot ({len(d)} <= {0x40000})')
    # first-boot copy goes 0x8000 -> 0x40000: an image above 0x38000 would overlap its own source
    check(len(d) <= 0x38000, f'first-boot copy cannot overlap itself ({len(d)} <= {0x38000})')

    for base, what in PLACEMENTS.items():
        for addr in CHECKED:
            off = addr - base
            if off <= 8 or off + 4 > len(d):
                continue
            check(d[off:off + 4] != KNLT, f"no 'KNLT' at 0x{addr:05x} when placed at 0x{base:05x} ({what})")
            check(d[off] != 0x4B, f"no 0x4B byte at 0x{addr:05x} when placed at 0x{base:05x} ({what})", warn=True)

    print()
    print('PASS' if not errs else f'{len(errs)} check(s) FAILED', f'({len(warns)} warning(s))' if warns else '')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
