# Reverse-engineering helpers

Used for [docs/gd32-7gclukjs-protocol.md](../../docs/gd32-7gclukjs-protocol.md) and
[docs/findings.md](../../docs/findings.md). Need the venv and dumps from `tools/setup.sh`.

| Tool | |
|---|---|
| `disasm.py <bin> <hexoff> <hexlen>` | Thumb disassembly of a slice of a GD32 dump |
| `disall.py <bin>` | Whole-image disassembly with PC-relative literals resolved |
| `scan.py <bin>` | Vector tables, flash/backup-register literals and strings in a GD32 dump |
| `emu_7gclukjs.py [bin]` | Runs the 7gclukjs firmware's own UART send and receive functions under unicorn and prints the frames |
