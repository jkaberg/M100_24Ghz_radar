# Unicorn harness for the ya4ft0w4 (ZY-M100-24GV3) GD32 image: calls the UART-protocol
# functions and captures bytes sent to the Zigbee UART (hook on 0x08005a80 = zigbee_uart_putc).
# Same layout as emu_7gclukjs.py; addresses are in docs/gd32-ya4ft0w4-protocol.md.
import sys
from unicorn import *
from unicorn.arm_const import *
FW = sys.argv[1] if len(sys.argv) > 1 else '.deps/dumps/ZY-M100-24GV3_TZE204_ya4ft0w4.bin'
d = open(FW, 'rb').read()
B = 0x08000000
PUTC = B + 0x5a80
RET = 0x0801f000          # magic return address (flash, filled with 0xFF)
CFG = 0x20000374          # radar config/state struct
FLAGS = 0x200000f0        # report flag word
NV = 0x0801f800           # flash page holding find_switch (outside the 64 KiB dump)


def mk():
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    # map 128 KiB: the image stores DP101 in the page at 0x0801F800
    mu.mem_map(B, 0x20000)
    mu.mem_write(B, d + b'\xff' * (0x20000 - len(d)))
    mu.mem_map(0x20000000, 0x2000)
    mu.mem_map(0x40000000, 0x30000)     # peripherals (dummy)
    mu.mem_map(0x48000000, 0x2000)
    mu.mem_map(0xE000E000, 0x1000)
    # .data init (scatter table @0x6170: copy 0x30 bytes 0x08006190 -> 0x20000000)
    mu.mem_write(0x20000000, d[0x6190:0x61c0])
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


def u8(mu, a):
    return mu.mem_read(a, 1)[0]


mu, out = mk()
show('0x01 product info reply (0x32d4)', call(mu, out, 0x32d4))
print('   ', call(mu, out, 0x32d4)[8:-1])
show('0x0B MCU version reply (0x39c8)', call(mu, out, 0x39c8))
show('0x02 ack / poll (0x2bd8)', call(mu, out, 0x2bd8))
show('0x03 at boot (0x2f78)', call(mu, out, 0x2f78))
show('0x03 pairing (0x2be4)', call(mu, out, 0x2be4))
show('bool DP101=1 (0x2a08)', call(mu, out, 0x2a08, 0x65, 1))
show('enum DP1=2 (0x2aa6)', call(mu, out, 0x2aa6, 1, 2))
show('value DP9=12 (0x2b1a)', call(mu, out, 0x2b1a, 9, 12))


def tick(mu, out, bit=None):
    """one 25-tick reporter slot (0x13b4)"""
    if bit is not None:
        mu.mem_write(FLAGS, (1 << bit).to_bytes(4, 'little'))
    mu.mem_write(0x200000e4, (24).to_bytes(4, 'little'))
    return call(mu, out, 0x13b4)


print('\n-- DP9 slot (bit 12): state byte CFG+2, find_switch CFG+0x34, raw cm u16 0x200000fe --')
for state, fs, raw in [(0, 1, 0), (0, 0, 0), (2, 1, 0), (2, 1, 25), (2, 1, 150), (2, 1, 154),
                       (2, 0, 150)]:
    mu.mem_write(CFG + 2, bytes([state]))
    mu.mem_write(CFG + 0x34, fs.to_bytes(4, 'little'))
    mu.mem_write(0x200000fe, raw.to_bytes(2, 'little'))
    b = tick(mu, out, 12)
    print(f"{f'state={state} find_switch={fs} raw={raw}':38s} {b.hex(' ') or '(nothing sent)'}")

print('\n-- DP104 slot (bit 15): enum from state and the absent/present second counters --')
for state, secs in [(0, 10), (0, 100), (0, 200), (0, 600), (0, 1200), (0, 4000),
                    (1, 10), (1, 100), (1, 200), (1, 600), (1, 1200), (1, 4000)]:
    mu.mem_write(CFG + 2, bytes([state]))
    mu.mem_write(0x20000370, secs.to_bytes(4, 'little'))   # seconds without target
    mu.mem_write(0x2000036c, secs.to_bytes(4, 'little'))   # seconds with target
    b = tick(mu, out, 15)
    print(f'  state={state} t={secs:5d}s -> DP104 = {b[-2]}')

print('\n-- full dump (flags = 0xFFFFFFFF, as after cmd 0x28 or status 0->1) --')
mu, out = mk()
mu.mem_write(CFG, bytes([8, 1, 2, 7, 6]))                  # max gate, min gate, state, sens
mu.mem_write(CFG + 0x0e, (150).to_bytes(2, 'little'))
mu.mem_write(CFG + 0x12, (30).to_bytes(2, 'little'))
mu.mem_write(CFG + 0x18, (120).to_bytes(2, 'little'))
mu.mem_write(CFG + 0x34, (1).to_bytes(4, 'little'))
mu.mem_write(0x200000fe, (150).to_bytes(2, 'little'))
mu.mem_write(FLAGS, (0xffffffff).to_bytes(4, 'little'))
n = 0
for t in range(40):
    b = tick(mu, out)
    if b:
        n += 1
        show(f'  dump slot {t:3d}', b)
print('frames in full dump:', n)

# ---- receive path: inject a module->MCU frame into the parse buffer, run parser 0x5db4
def frame(cmd, data=b'', seq=0x1234, ver=2):
    f = bytes([0x55, 0xAA, ver, seq >> 8, seq & 0xff, cmd, len(data) >> 8, len(data) & 0xff]) + data
    return f + bytes([sum(f) & 0xff])


def rx(mu, out, f):
    mu.mem_write(0x200006d4, f)
    mu.mem_write(0x2000075e, len(f).to_bytes(2, 'little'))
    return call(mu, out, 0x5db4)


def dp(dpid, typ, val, n=4):
    return bytes([dpid, typ, 0, n]) + val.to_bytes(n, 'big')


print('\n-- receive path (radar cfg: max gate 8, min gate 1) --')
mu, out = mk()
mu.mem_write(CFG, bytes([8, 1]))
tests = [
    ('0x00', frame(0x00)),
    ('0x01 product query', frame(0x01)),
    ('0x02 status 01', frame(0x02, b'\x01')),
    ('0x03 reply', frame(0x03, b'\x00')),
    ('0x04 DP2=5', frame(0x04, dp(2, 2, 5))),
    ('0x04 DP2=0', frame(0x04, dp(2, 2, 0))),
    ('0x04 DP2=15 (out of range)', frame(0x04, dp(2, 2, 15))),
    ('0x04 DP102=7', frame(0x04, dp(0x66, 2, 7))),
    ('0x04 DP4=450', frame(0x04, dp(4, 2, 450))),
    ('0x04 DP4=50 (< min gate)', frame(0x04, dp(4, 2, 50))),
    ('0x04 DP3=2000 (>1000)', frame(0x04, dp(3, 2, 2000))),
    ('0x04 DP105=30', frame(0x04, dp(0x69, 2, 30))),
    ('0x04 DP105=15000', frame(0x04, dp(0x69, 2, 15000))),
    ('0x04 DP105=70000 (>0xFFFF)', frame(0x04, dp(0x69, 2, 70000))),
    ('0x04 DP101=0 (bool)', frame(0x04, dp(0x65, 1, 0, 1))),
    ('0x04 DP101=1 (bool)', frame(0x04, dp(0x65, 1, 1, 1))),
    ('0x04 DP101 as value (type 2)', frame(0x04, dp(0x65, 2, 0))),
    ('0x04 DP2 wrong type (enum)', frame(0x04, dp(2, 4, 5, 1))),
    ('0x04 DP1 enum', frame(0x04, dp(1, 4, 1, 1))),
    ('0x04 DP104 enum', frame(0x04, dp(0x68, 4, 1, 1))),
    ('0x04 DP2+DP102 in one frame', frame(0x04, dp(2, 2, 3) + dp(0x66, 2, 4))),
    ('0x05', frame(0x05)), ('0x06', frame(0x06)),
    ('0x08 DP query', frame(0x08)),
    ('0x0B version query', frame(0x0B)),
    ('0x24 time', frame(0x24, bytes(8))),
    ('0x28', frame(0x28)),
    ('ver byte 0x03 (dropped?)', frame(0x01, ver=3)),
    ('ver byte 0x00 (dropped?)', frame(0x01, ver=0)),
    ('bad checksum', frame(0x01)[:-1] + b'\x00'),
]
for name, f in tests:
    r = rx(mu, out, f)
    flags = int.from_bytes(mu.mem_read(FLAGS, 4), 'little')
    radar_q = int.from_bytes(mu.mem_read(0x20000008, 1), 'little')
    nv = bytes(mu.mem_read(NV, 8)).hex()
    print(f'{name:30s} -> {r.hex(" ") or "(no reply)"}   flags=0x{flags:08x} '
          f'find_switch={u8(mu, CFG + 0x34)} nv@0801f800={nv}')
    mu.mem_write(FLAGS, bytes(4))
