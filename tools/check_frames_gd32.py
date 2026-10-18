#!/usr/bin/env python3
"""Push every frame the module firmware sent in the host harness (tx_frames.bin, written by
firmware/tests/test_glue) through the *real* 7gclukjs GD32 receive parser under unicorn, and
check the MCU accepts each one and reacts as intended.

Harness and addresses from tools/emu_7gclukjs.py (parse buffer 0x200006c4, length 0x2000074e,
parser 0x5814, UART putc 0x54e8, report flags 0x200000f0).

Usage: check_frames_gd32.py <tx_frames.bin> [fw.bin]
"""
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP

FRAMES = sys.argv[1]
FW = sys.argv[2] if len(sys.argv) > 2 else '.deps/dumps/ZY-M100-24GV2_TZE204_7gclukjs.bin'
d = open(FW, 'rb').read()
B = 0x08000000
PUTC = B + 0x54e8
RET = 0x0800fff0


def mk():
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mu.mem_map(B, 0x10000)
    mu.mem_write(B, d)
    mu.mem_map(0x20000000, 0x2000)
    mu.mem_map(0x40000000, 0x30000)
    mu.mem_map(0x48000000, 0x2000)
    mu.mem_map(0xE000E000, 0x1000)
    mu.mem_write(0x20000000, d[0x5bc0:0x5bf0])
    mu.mem_write(0x2000036c, bytes([8, 1]))      # radar config read: max gate 8, min gate 1
    out = []

    def hook(mu, addr, size, ud):
        if addr == PUTC:
            out.append(mu.reg_read(UC_ARM_REG_R0) & 0xff)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
        elif addr == RET:
            mu.emu_stop()
    mu.hook_add(UC_HOOK_CODE, hook)
    return mu, out


def rx(mu, out, f):
    mu.mem_write(0x200000f0, bytes(4))
    mu.mem_write(0x200006c4, f)
    mu.mem_write(0x2000074e, len(f).to_bytes(2, 'little'))
    mu.reg_write(UC_ARM_REG_SP, 0x20001f00)
    mu.reg_write(UC_ARM_REG_LR, RET | 1)
    del out[:]
    mu.emu_start((B + 0x5814) | 1, RET, count=2000000)
    flags = int.from_bytes(mu.mem_read(0x200000f0, 4), 'little')
    return bytes(out), flags


def frames_in(blob):
    i = 0
    while i < len(blob):
        n = blob[i]
        yield blob[i + 1:i + 1 + n]
        i += 1 + n


def main():
    blob = open(FRAMES, 'rb').read()
    sent = list(frames_in(blob))
    # identical frames except the seq field behave the same: test one of each kind
    kinds = {}
    for f in sent:
        kinds.setdefault((f[5], f[6:-1]), f)

    mu, out = mk()
    errs = 0
    print(f'{len(sent)} frames sent by the module, {len(kinds)} distinct (cmd, payload)')
    for (cmd, payload), f in sorted(kinds.items()):
        reply, flags = rx(mu, out, f)
        rcmd = reply[5] if len(reply) > 5 else None
        if cmd in (0x0C, 0x0D, 0x0E):
            ok, want = False, 'never sent'
        elif cmd == 0x01:
            ok, want = rcmd == 0x01 and b'7gclukjs' in reply, 'product info reply'
        elif cmd == 0x02:
            ok, want = rcmd == 0x02, 'status ack'
        elif cmd == 0x04:
            ok, want = rcmd == 0x06 and reply[8] == f[8], 'DP echoed as 0x06'
        elif cmd == 0x28:
            ok, want = flags == 0xffffffff, 'full dump scheduled'
        elif cmd in (0x03, 0x05, 0x06):
            ok, want = reply == b'' and flags == 0, 'ignored (ack)'
        else:
            ok, want = False, 'unexpected command'
        errs += not ok
        print(f"{'ok  ' if ok else 'FAIL'} cmd 0x{cmd:02x} {payload.hex(' ')[:40]:40s} -> "
              f"{(reply.hex(' ') or '(none)')[:48]:48s} [{want}]")
    print('PASS' if not errs else f'{errs} FAILED')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
