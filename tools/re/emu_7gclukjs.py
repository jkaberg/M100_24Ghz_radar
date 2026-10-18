# Unicorn harness for the 7gclukjs GD32 image: calls UART-protocol functions and
# captures bytes sent to the Zigbee UART (hook on 0x080054e8 = zigbee_uart_putc).
import sys
from unicorn import *
from unicorn.arm_const import *
FW = sys.argv[1] if len(sys.argv) > 1 else '.deps/dumps/ZY-M100-24GV2_TZE204_7gclukjs.bin'
d = open(FW, 'rb').read()
B = 0x08000000
PUTC = B + 0x54e8
RET = 0x0800fff0          # magic return address (flash, filled with 0xFF)
def mk():
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mu.mem_map(B, 0x10000); mu.mem_write(B, d)
    mu.mem_map(0x20000000, 0x2000)
    mu.mem_map(0x40000000, 0x30000)     # peripherals (dummy)
    mu.mem_map(0x48000000, 0x2000)
    mu.mem_map(0xE000E000, 0x1000)
    # .data init (scatter table @0x5b9c: copy 0x30 bytes 0x08005bc0 -> 0x20000000)
    mu.mem_write(0x20000000, d[0x5bc0:0x5bf0])
    out = []
    def hook(mu, addr, size, ud):
        if addr == PUTC:
            out.append(mu.reg_read(UC_ARM_REG_R0) & 0xff)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
        elif addr == RET:
            mu.emu_stop()
    mu.hook_add(UC_HOOK_CODE, hook)
    return mu, out
def call(mu, out, fn, *args):
    for i, a in enumerate(args):
        mu.reg_write(UC_ARM_REG_R0 + i, a)
    mu.reg_write(UC_ARM_REG_SP, 0x20001f00)
    mu.reg_write(UC_ARM_REG_LR, RET | 1)
    del out[:]
    mu.emu_start((B + fn) | 1, RET, count=2000000)
    return bytes(out)
def show(name, b):
    print(f'{name:38s} {b.hex(" ")}')
mu, out = mk()
show('0x01 product info reply (0x2fa8)', call(mu, out, 0x2fa8))
show('  ascii payload', b'')
print('   ', call(mu, out, 0x2fa8)[8:-1])
show('0x0B MCU version reply (0x3698)', call(mu, out, 0x3698))
show('0x02 ack / poll (0x2900)', call(mu, out, 0x2900))
show('0x03 at boot (0x2c78)', call(mu, out, 0x2c78))
show('0x03 pairing (0x290c)', call(mu, out, 0x290c))
show('DP9 value=12 (0x284a)', call(mu, out, 0x284a, 9, 12))
show('DP1 enum=2 (0x27d4)', call(mu, out, 0x27d4, 1, 2))
# reporter 0x127c: set flag 12 (DP9) with raw distance 0 (no target) and run 25 ticks
for raw in (0, 150, 154, 145):
    mu.mem_write(0x200000f0, (1 << 12).to_bytes(4, 'little'))
    mu.mem_write(0x200000fe, raw.to_bytes(2, 'little'))
    mu.mem_write(0x200000e4, (24).to_bytes(4, 'little'))
    show(f'reporter DP9 raw={raw}', call(mu, out, 0x127c))
# full re-report: all flags set (as by cmd 0x28 / 0x34e0), run until no frames for 30 ticks
mu.mem_write(0x200000f0, (0xffffffff).to_bytes(4, 'little'))
n = 0
for t in range(25 * 14):
    b = call(mu, out, 0x127c)
    if b:
        n += 1; show(f'  dump tick {t:3d}', b)
print('frames in full dump:', n)

# ---- receive path: inject a module->MCU frame into the parse buffer, run parser 0x5814
def frame(cmd, data=b'', seq=0x1234, ver=2):
    f = bytes([0x55, 0xAA, ver, seq >> 8, seq & 0xff, cmd, len(data) >> 8, len(data) & 0xff]) + data
    return f + bytes([sum(f) & 0xff])
def rx(mu, out, f):
    mu.mem_write(0x200006c4, f)
    mu.mem_write(0x2000074e, len(f).to_bytes(2, 'little'))
    return call(mu, out, 0x5814)
def dp(dpid, typ, val, n=4):
    return bytes([dpid, typ, 0, n]) + val.to_bytes(n, 'big')
print('\n-- receive path (radar cfg: max gate 8, min gate 1) --')
mu, out = mk()
mu.mem_write(0x2000036c, bytes([8, 1]))
tests = [
    ('0x00', frame(0x00)),
    ('0x01 product query', frame(0x01)),
    ('0x02 status 01', frame(0x02, b'\x01')),
    ('0x03 reply', frame(0x03, b'\x00')),
    ('0x04 DP2=5', frame(0x04, dp(2, 2, 5))),
    ('0x04 DP2=15 (out of range)', frame(0x04, dp(2, 2, 15))),
    ('0x04 DP4=450', frame(0x04, dp(4, 2, 450))),
    ('0x04 DP4=50 (< min gate)', frame(0x04, dp(4, 2, 50))),
    ('0x04 DP3=2000 (>1000)', frame(0x04, dp(3, 2, 2000))),
    ('0x04 DP105=30', frame(0x04, dp(0x69, 2, 30))),
    ('0x04 DP102=7', frame(0x04, dp(0x66, 2, 7))),
    ('0x04 DP2 wrong type (enum)', frame(0x04, dp(2, 4, 5, 1))),
    ('0x04 DP1 enum', frame(0x04, dp(1, 4, 1, 1))),
    ('0x04 DP2+DP102 in one frame', frame(0x04, dp(2, 2, 3) + dp(0x66, 2, 4))),
    ('0x05', frame(0x05)), ('0x06', frame(0x06)),
    ('0x08 DP query', frame(0x08)),
    ('0x0B version query', frame(0x0B)),
    ('0x24 time', frame(0x24, bytes(8))),
    ('0x28', frame(0x28)),
    ('ver byte 0x03 (dropped?)', frame(0x01, ver=3)),
    ('bad checksum', frame(0x01)[:-1] + b'\x00'),
]
for name, f in tests:
    r = rx(mu, out, f)
    flags = int.from_bytes(mu.mem_read(0x200000f0, 4), 'little')
    print(f'{name:30s} -> {r.hex(" ") or "(no reply)"}   flags=0x{flags:08x}')
    mu.mem_write(0x200000f0, bytes(4))
