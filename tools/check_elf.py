#!/usr/bin/env python3
"""Post-build checks on the linked ELF that tests on the PC can't see.

1. Reporting is wired up: something calls report_handler, which arms the report timers, whose
   callback sends zcl_reportAttrs (the red-team review found the app never called
   report_handler, so no ZCL report would ever have been sent).
2. The stock->custom handoff is RAM-only: when the Tuya bootloader starts us at 0x8000, main()
   and everything it calls before SYSTEM_RESET must sit in .ram_code. We require that
   ensure_correct_ota_scheme's whole call graph stays in .ram_code, that main() is in .ram_code,
   and that main() itself calls only ensure_correct_ota_scheme and flash_main (the normal path).

usage: check_elf.py <elf> <toolchain prefix, e.g. .../tc32-elf>
"""
import re
import subprocess
import sys

elf, prefix = sys.argv[1], sys.argv[2]
errs = []


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


syms = {}
for line in run(prefix + '-nm', elf).splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

# whole-binary call graph (direct calls only)
callers, cur = {}, None
for line in run(prefix + '-objdump', '-d', elf).splitlines():
    m = re.match(r'^([0-9a-f]+) <(.+)>:', line)
    if m:
        cur = m.group(2)
        continue
    m = re.search(r'\t(tjl|bl|blx)\s+[0-9a-f]+\s*<([^>+]+)', line)
    if m:
        callers.setdefault(m.group(2), set()).add(cur)

# SDK 3.7.2.0 chain: report_handler -> reportAttrTimerStart -> (timers) reportingTimerCb -> zcl_reportAttrs
for callee, why in (('report_handler', 'the app calls report_handler'),
                    ('reportAttrTimerStart', 'report_handler arms the report timers'),
                    ('zcl_reportAttrs', 'the reporting timer sends reports')):
    ok = bool(callers.get(callee))
    print(('ok    ' if ok else 'FAIL  ') + f'{why} ({callee} <- {sorted(callers.get(callee, []))})')
    if not ok:
        errs.append(callee)

sec = {}
for line in run(prefix + '-objdump', '-h', elf).splitlines():
    m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
    if m:
        sec[m.group(1)] = (int(m.group(3), 16), int(m.group(3), 16) + int(m.group(2), 16))
lo, hi = sec['.ram_code']
# the Tuya bootloader copies a fixed 0x5000 bytes of RAM code when it starts the app at 0x8000
ok = sec['.vectors'][1] - sec['.vectors'][0] + (hi - lo) <= 0x5000
print(('ok    ' if ok else 'FAIL  ') + f'vectors + .ram_code = 0x{sec[".vectors"][1] - sec[".vectors"][0] + hi - lo:x} <= 0x5000 (Tuya bootloader copy)')
errs += [] if ok else ['ram_code size']

dis = run(prefix + '-objdump', '-d', f'--start-address={lo:#x}', f'--stop-address={hi:#x}', elf)
funcs, cur = {}, None
for line in dis.splitlines():
    m = re.match(r'^([0-9a-f]+) <(.+)>:', line)
    if m:
        cur = m.group(2)
        funcs[cur] = []
    elif cur:
        m = re.search(r'\t(tjl|bl|blx)\s+([0-9a-f]+)\s*<([^>+]+)', line)
        if m:
            funcs[cur].append((int(m.group(2), 16), m.group(3)))

ok = 'main' in funcs
print(('ok    ' if ok else 'FAIL  ') + 'main() is in .ram_code')
errs += [] if ok else ['main']

calls = {name for _, name in funcs.get('main', [])}
ok = calls <= {'ensure_correct_ota_scheme', 'flash_main'}
print(('ok    ' if ok else 'FAIL  ') + f'main() calls only {sorted(calls)}')
errs += [] if ok else ['main calls']

seen, stack, bad = set(), ['ensure_correct_ota_scheme'], []
while stack:
    f = stack.pop()
    if f in seen:
        continue
    seen.add(f)
    if f not in funcs:
        bad.append(f)
        continue
    for addr, name in funcs[f]:
        if not lo <= addr < hi:
            bad.append(f'{f} -> {name}')
        else:
            stack.append(name)
ok = not bad
print(('ok    ' if ok else 'FAIL  ') + f'handoff call graph stays in .ram_code ({len(seen)} functions)'
      + ('' if ok else f': {bad}'))
errs += bad

print('PASS' if not errs else 'FAILED')
sys.exit(1 if errs else 0)
