# How it works

```
 radar ── UART ── GD32 (stock, untouched) ── UART 115200 ── ZT3L module ── Zigbee ── z2m
                                                             └─ this firmware
```

The GD32 keeps doing what it always did, including chattering about distance every second. The
module firmware decides what goes on air.

## Source layout

| Path | What |
|---|---|
| `firmware/src/tuya_mcu.c` | Tuya serial protocol: frame parser and builder, DP codec. No SDK dependencies |
| `firmware/src/radar.c` | DP map of the variant, dedup and the distance policy. No SDK dependencies |
| `firmware/src/app_radar.c` | Glue: UART link supervision, network-status policy, DP ⇄ ZCL attributes, distance reports, NV |
| `firmware/src/app_uart.c` | DMA RX into a ring buffer, TX |
| `firmware/src/app_endpoint_cfg.c` | Endpoint 1: Basic, Identify, Groups, Occupancy Sensing (+ radar attributes), Illuminance; OTA client |
| `firmware/src/app_main.c` | Start-up, default reporting, safe mode, main task |
| `firmware/src/zb_appCb.c` | Commissioning and leave callbacks |
| `firmware/src/zcl_appCb.c` | Attribute writes → app_radar |
| `firmware/src/common/main.c` | Entry point: first-boot handoff, then normal start-up |
| `firmware/src/ota_scheme/` | The handoff (adapted from romasku/tuya-zigbee-switch, MIT) |
| `z2m/zy-m100-zrd.mjs` | External converter, one file for both states: the stock definition with `ota: true`, and the custom firmware (with a definition `version`, so z2m reconfigures by itself after updates) |

## What gets reported

| z2m | Attribute | When |
|---|---|---|
| `occupancy` | 0x0406/0x0000 | every change, hourly heartbeat |
| `presence_state` | 0x0406/0xF000 | on change, at most every 10 s |
| `illuminance` | 0x0400/0x0000 | at most every 60 s, only on ~25 % change |
| `target_distance` | 0x0406/0xF001 | **off by default**, see below |
| settings | 0x0406/0xF004–0xF008 | only when they change (they're echoed back by the GD32 after a write) |
| `distance_offset`, `distance_scale` | 0x0406/0xF009, 0xF00A | read/write, stored on the module |
| `safe_mode` | 0x0406/0xF00F | read |

- Repeated values never go out: an unchanged attribute produces no report.
- The SDK's reporting engine handles everything except distance, so z2m may reconfigure the
  intervals.

**Distance** is sent by the firmware itself:
- only when `distance_report_interval` (0xF002) > 0;
- at most once per interval, and only after moving `distance_deadband` (0xF003) cm;
- target appearing or disappearing always counts;
- the latest value is sent when the interval expires.

The attribute isn't reportable through ZCL, so a reporting configuration can't bring the flood
back. Both settings are kept in NV.

**Calibration.** The GD32 computes distance as `(cm + 5) / 10 − 3` decimetres: 10 cm steps with
30 cm taken off, and −3 for no target. The module applies `cm × distance_scale / 100 +
distance_offset` to every reading before dedup and deadband. "No target" stays 0, and a present
target never reads below 1 cm. Defaults (0 cm, 100 %) leave the readings unchanged. Calibration has
its own NV item, so settings from earlier versions survive the upgrade.

## Talking to the GD32

Based on the RE in [gd32-7gclukjs-protocol.md](gd32-7gclukjs-protocol.md):

- **Finding the MCU.** Send the product-info query (0x01) at 115200 (and 9600 as a fallback). On
  `{"p":"7gclukjs",…}` lock the baud rate, pick the DP map and ask for a full dump (0x28). Every
  5 min check the MCU still answers; after 3 misses, search again.
- **Input parsing.** Frames are parsed byte by byte with checksum and length checks. A frame
  paused more than 50 ms is dropped, so a cut-off frame can't swallow the next ones.
- **Writes.** Zigbee writes become DP writes (0x04), clamped to what the MCU accepts. The MCU
  echoes the value it took and the attribute follows it.
- **Network status.**
  - A device that has a network always reports "joined" to the MCU, even while rejoining. A
    1 → 0 drop would make the MCU ask us to leave.
  - `0x03 01` from the MCU only causes a leave + re-pair when joined, idle and not factory-new,
    which is a held key.
  - `0x03 00` (every MCU boot) is ignored.
  - Status answers are rate limited, so the MCU's own 0x02 acks can't ping-pong.
- **Never sent:** MCU-OTA commands (0x0C–0x0E). The GD32 would stage an image it can't install
  and reboot-loop.

## First-boot handoff

The stock OTA client installs our image like a Tuya update. The stock bootloader then copies it to
0x8000 and starts it. On that first start only RAM code may run, because the image is linked for a
different address.

Then:
1. Copy the image to slot B (0x40000), verifying each 256-byte page. Sectors that already match
   are not touched.
2. Zero the boot byte of the Tuya bootloader (0x0), then of the old app (0x8000). These are
   program-only writes, with no erase.
3. Reset. The ROM now boots slot B.

From then on the Telink SDK's normal A/B update applies. A new image goes into the other slot,
and only once its CRC checks out is its boot byte set and the old one cleared.

A power cut or watchdog reset at any point leaves a bootable image. Before step 2 the old chain
just runs the handoff again. The "already matches" check is what makes a second cut during that
re-run harmless. romasku's original code bricks in exactly that case; our simulation reproduces
it.

`tools/check_elf.py` checks every build:
- `main()` and the whole handoff call graph sit in `.ram_code`;
- the RAM-code block fits the 0x5000 bytes the Tuya bootloader copies.

## Recovery by design

| Problem | Way out |
|---|---|
| Radar code misbehaves or crashes | **Safe mode**: 5 boots without 30 min of uptime → Zigbee + OTA only (`safe_mode` = true). Send a fixed image |
| Can't rejoin | 10 power cycles < 2 s → leave and pair again (clears the safe-mode counter). Or hold the key 6 s, if the unit has one |
| Bad update | A/B slots: the old image stays until the new one has been fully downloaded and checked |
| Module doesn't come up at all | SWS pad (ZT3L pad 17) + USB-UART + `make flash` (pvvx TlsrPgm). The only case that needs a wire |

There is no way back to the stock Tuya firmware without a wired flash.

## Build

- `tools/setup.sh` fetches the toolchain, the Telink SDK 3.7.2.0, the dumps used by the tests,
  and a Python venv. All checked by sha256.
- `firmware/build.sh` builds the version in `src/include/version_cfg.h` and packages it twice as
  Zigbee OTA files:
  - `1141-d3a3-…-from-tuya.zigbee` carries the stock module's IDs, for the first update;
  - `6565-5244-….zigbee` carries ours, for every update after that.

  Every build runs `tools/verify_image.py` and `tools/check_elf.py`.
- `tools/make_bundle.py` writes `dist/release/`: both OTA images, both converters, the offline zip,
  `SHA256SUMS` and `z2m_ota_index.json`, an OTA index with absolute release URLs. Users point z2m
  at `releases/latest/download/z2m_ota_index.json`, so publishing a release
  ([workflow](../.github/workflows/release.yml), on a `v*` tag) is all it takes for converted
  devices to be offered the update. Both indexes are locked to `_TZE204_7gclukjs` for stage 1 and
  pinned by SHA-512.
- Builds are reproducible apart from the ZCL date code (the build day): the same source on the
  same day gives byte-identical images.
