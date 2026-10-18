# Verification

Everything below was done before the first flash, to make it as safe as possible. Then it was
flashed on a test unit; see [On hardware](#on-hardware).

## Run it yourself

```sh
tools/setup.sh          # toolchain, SDK, dumps, venv (sha256-checked)
firmware/build.sh       # builds the current version, runs verify_image.py and check_elf.py
make -C firmware/tests  # everything below except the long libFuzzer run
make -C firmware/tests libfuzzer
```

## What is checked

| Check | What it proves |
|---|---|
| `tests/test_core.c` | Parser (resync, checksum, oversize, cut-off frames), DP codec, product id, dedup, distance policy (deadband, interval incl. 1 s / 0 cm, wrap-around, −3 = no target), distance calibration (offset, scale, clamps, overflow), setting clamps, lux → ZCL conversion. 261 checks |
| `tests/test_glue.c` | The real `app_radar.c` against SDK stubs and a simulated GD32 built from the RE. Covers pairing, rejoin, a 10 min replay of the recorded flood, distance on and off, Zigbee writes echoed through the MCU's rounding, key hold vs MCU reboot, link loss and recovery, the review regressions (no leave mid-join, joined stays joined, resync), and calibration over Zigbee persisting across a restart. 1962 checks |
| `tools/check_frames_gd32.py` | Every frame the firmware sent in the glue test, fed through the **real GD32 firmware's** receive parser under unicorn. All are accepted with the intended reaction, and no MCU-OTA frame is ever sent |
| `tests/handoff/test_handoff.c`, `test_handoff_rom.c` | The real handoff code against a NOR-flash model seeded with a **real stock ZT3L dump** plus our image. A power cut at every one of 759 flash ops (prefix and bit-level partial programming, garbage after an interrupted erase), a watchdog reset at every op, and every pair of cuts on a smaller image, for both stock layouts. 0 failures, ≤ 4 boots to recover. Control runs: romasku's original code fails the double cut, and a wrong marker order fails a single cut |
| `tests/nv/test_nv_leftovers.c` | The SDK's own `drv_nv.c` on the flash as it looks after conversion (leftover download over the NV area), plus 200 layouts with forged NV headers. Nothing lost, no garbage read as valid, no erase outside NV, no loops |
| `tests/fuzz/fuzz_uart.c` | Hostile UART input and random Zigbee writes through the real glue, under ASan/UBSan. Invariants: no MCU-OTA frame, only well-formed frames, never "not joined" while on a network, leave only after a real key-hold frame. 20k random sessions in the default run; libFuzzer reached 786k runs clean |
| `tools/verify_image.py` (every build) | Telink header, CRC, size. The image fits a slot and the first copy can't overlap itself. No boot byte 0x4B at any address the ROM or the handoff inspects, wherever the image sits (0x0, 0x40000, 0x8000, 0x70000) |
| `tools/check_elf.py` (every build) | ZCL reporting is wired up (`report_handler` → timers → `zcl_reportAttrs`). `main()` and the whole handoff run from RAM code, and the RAM-code block fits the Tuya bootloader's 0x5000-byte copy |
| z2m container checks | Both converters load in z2m 2.14.2 and decode reports with the right units. herdsman's own parser reads both OTA files, and its own `findMatchingOtaImage` offers stage 1 only to `_TZE204_7gclukjs`, then the custom-ID image to the converted device, then nothing |

## Red team

Four independent reviews tried to break the design. Their reports are in [redteam/](redteam/).

| Round | Result | Fixed |
|---|---|---|
| OTA acceptance | No blocker. Found 3 brick paths in the original slacky-style handoff | Switched to the romasku A/B handoff, hardened |
| Hardware | No blocker. ZT3L confirmed, pins confirmed | Voltage-sense pin moved to unbonded PC5, voltage gate off |
| Code review | **Blocker**: ZCL reporting was never driven. 3 medium (leave mid-join, a joined device saying "not joined", leave confirm for a child) and 8 low | All fixed, with tests and build checks |
| Final | No blocker. HIGH: z2m doesn't re-interview a rejoined device | Install guide; safe-mode exit fixed; build guards; steering retries |

## On hardware

Test unit (`_TZE204_7gclukjs`, stock Zigbee module 1.0.10 / MCU 1.0.0), z2m 2.14.2, 2026-10-18:

| Step | Result |
|---|---|
| Stock → v1.0.01 over the air | z2m: "OTA updated from 74 to 268513281". Handoff and rejoin by itself, same IEEE |
| Interview + Reconfigure | Identified as `ZY-M100-ZRD_1` / manufacturer 0x6565 / `v1.0.01`; binds and reporting as designed; `safe_mode` false |
| Values | Occupancy, presence state, illuminance (27–28 lx, same as stock just before), ranges and timeout read from the MCU |
| Traffic | Stock: 35–120 messages/min. Custom: **4 messages in 3 min**: one state report, one unchanged report, `occupancy: false` immediately when the room emptied, `presence_state: none` 10 s later |
| v1.0.01 → v1.0.02 over the air | Custom OTA chain (0x6565/0x5244): "OTA updated from 268513281 to 268578817", ~16 min. Rejoined by itself; settings kept; `safe_mode` false. During the download z2m republishes the device on every block request; that's update traffic, not radar reports |
| Automatic reconfigure | With the converter's definition `version` set, z2m reconfigured the device by itself after a restart: "Successfully configured 'test_mmwave' (definition v1.0.3)". After an OTA, z2m re-interviews by itself, so the first update needs only Update + Permit join |

## Residual risk

Before the first flash the estimates were roughly 90–95 % that the stock module installs the
image, 95 % that the handoff completes and 90 % that the device comes back updatable over the air.
All three held on the first unit.

Still open:
- other units may have a different stock flash layout. Every known dump has the same one, but it
  can't be checked without a wire;
- long-term behaviour: days of uptime, coordinator restarts, the radar MCU's own 72 h restart.

If a module never shows up on Zigbee again after an update, recovery needs the SWS wire.
