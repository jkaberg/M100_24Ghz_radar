# Findings

What we learned about the ZY-M100-24GV2 and why the fix ended up where it is. Everything here is
backed by a dump, a disassembly, a log or a test. Sources are linked at the end.

## The device

| | |
|---|---|
| Sold as | Wenzhi WZ-M100-W (and other names) |
| z2m model | `ZY-M100-24GV2`, `TS0601` / `_TZE204_7gclukjs` |
| Zigbee module | Tuya **ZT3L** (Telink TLSR8258F1KET32, 1 MB flash). Silkscreen photographed on a 7gclukjs board (Andrik45719/ZY-M100#4); node descriptor manufacturer 0x1141; IEEE prefix a4:c1:38 (Telink) |
| Module firmware | "Zigbee module: 1.0.10", OTA query `manufacturerCode 0x1141, imageType 0xD3A3, fileVersion 74` |
| Radar MCU | GigaDevice GD32E230F8, firmware "MCU module: 1.0.0", product id `7gclukjs`, MCU version byte 0x40 |
| Radar | DFRobot C4001-class 24 GHz module, ASCII commands over UART to the GD32 |

"WZ-M100-W" covers several boards. Some report `_TZE204_e5m9c5hl` (ZY-M100-S_1), others
`_TZE204_ya4ft0w4` (24GV3). The older 5.8 GHz ZY-M100-L uses a Silabs **ZS3L** module instead.
This project only targets `_TZE204_7gclukjs`.

### The 24GV3 (`_TZE204_ya4ft0w4`), not supported yet

Collected from public sources (issue #1) as groundwork for a V3 variant. Nothing below has been
checked on a V3 unit by us. The MCU side is reverse-engineered in
[gd32-ya4ft0w4-protocol.md](gd32-ya4ft0w4-protocol.md), and v1.0.4 carries a `ya4ft0w4` variant
for testing.

- **Same Zigbee module as the V2.**
  - Andrik45719/ZY-M100#22 shows the z2m page of a `_TZE204_ya4ft0w4` unit (IEEE `0xa4c138…`)
    and photos of its boards. The power board (`2023.2-V3.1`) carries a module silkscreened
    **ZT3L**. The radar board is `ZY_V230605-24G-V1.3`.
  - zigpy/zha `tests/data/devices/tze204-ya4ft0w4-ts0601-0x0000004a.json` has node descriptor
    manufacturer code 0x1141, max buffer 66, and OTA `current_file_version` 74 ("Zigbee module
    1.0.10"). All three match the V2 test unit.
  - Unknown: the OTA `imageType`. Expected to be 0xD3A3 like the V2, but no published query found.
- **`_TZE200_ya4ft0w4` is different hardware.** The zha dump `tze200-ya4ft0w4-ts0601.json`
  reports manufacturer code 0x1234 and max buffer 108, so not the Telink module. z2m puts
  `_TZE204/_TZE200_ya4ft0w4` and `_TZE204/_TZE200_gkfbdvyx` in one definition. An OTA index
  entry must match `_TZE204_ya4ft0w4` exactly.
- **Board revisions vary.** A `ZY-V240405-5.824G-V1.5` radar board also shows up as 24GV3
  (Andrik45719/ZY-M100#22). Its module wasn't photographed.
- **Radar MCU.** GD32E230 as on the V2. Andrik45719/ZY-M100 hosts a stock dump,
  `ZY-M100-24GV3_TZE204_ya4ft0w4.bin`, so the product id, baud rate and DP map can be taken from
  it without hardware. Not reverse-engineered yet. gekkehenkie11 reports that this firmware
  resets itself after 3 days (Andrik45719/ZY-M100#27).
- **DP map per z2m** (zigbee-herdsman-converters `tuya.ts`, `ZY-M100-24GV3`):

  | DP | Meaning |
  |---|---|
  | 1 | state enum (none/presence/move), presence derived from it |
  | 2, 102 | move/presence sensitivity 1–10, 0 reported at times |
  | 3, 4 | range min/max in cm, 75 cm steps (`gkfbdvyx`: 50 cm) |
  | 9 | distance in dm |
  | 101 | find_switch (distance reporting on/off) |
  | 103 | illuminance |
  | 105 | timeout in s, up to 15000 |

  Compared with the V2: no DP 104, adds DP 101, and a 10× longer timeout range.
- **Floods the same way.** Andrik's notes list target distance about once a second, illuminance
  every 500 ms and settings every 10 s. Some units are quieter while `find_switch` is off
  (Andrik45719/ZY-M100#22).

## Why it floods

- The GD32 sends target distance (DP 9) about once a second whenever the raw distance in cm
  changes. It reports in decimetres, so many consecutive reports carry the same value.
- It re-sends all 11 DPs every 10 s for a minute after each state or lux change, and every 10 min
  otherwise.
- The stock Zigbee module forwards every frame unchanged.
- Measured on the test unit right after pairing: 35–120 messages/min. In 45 s of debug log there
  were 30 DP 9 frames, often with the same value several times in a row.

## Fixing the radar MCU over the air: not possible

andi-at-1/z2m-tuya-mcu-ota showed that Tuya's MCU-OTA (cluster 0xEF00, cmds 0x0C/0x0D/0x0E) can
update the MCU of the WZ75 sensors. For this device it can't:

1. **There is no vendor image.** Wenzhi never released MCU firmware for the M100 24 GHz line
   (wzwenzhi/Wenzhi-ZigBee2mqtt#11, "under development" since 2024).
2. **The GD32 can't install one anyway.** It has the MCU-OTA receive code (PID check against
   `7gclukjs`, image staged at 0x0800A000, byte-sum check, then `RTC_BKP0 = 0x11AA33DD`). But no
   code anywhere reads that flag or copies the staged image: the dump ends at 0x5BEF, and there
   is no bootloader. The main loop resets the MCU while the flag is set. The result is a reboot
   loop until power-off, then the old firmware.

   Checked statically on all of Andrik's dumps and on an independently posted stock dump (none has
   code past ~0x5C00). For 7gclukjs it was re-checked separately, with partial emulation.
   Details: [gd32-7gclukjs-protocol.md §8](gd32-7gclukjs-protocol.md).

Andrik45719's patched GD32 images do fix the flood, but only via SWD wiring.

## The Zigbee module can be replaced over the air

- z2m's ordinary OTA (genOta) talks to the module, and the module answered an OTA query on the
  test unit.
- slacky1965 (thermostats) and romasku (182 switch and relay types with stock 0x1141/0xD3A3)
  install custom firmware on stock Tuya TLSR8258 modules this way. Stock modules check the
  header and CRC, but not a signature.
- The stock TuyaMCU bridge firmware is product-agnostic. Thirteen thermostat products carry the
  byte-identical app, and the `_TZE…` identity lives in a separate config area (0xEC000).
  All 16 stock dumps from slacky (and 23 from romasku) share one Tuya bootloader at 0x0 with the
  app at 0x8000.

So the fix is new firmware for the module: it talks to the GD32 like the stock bridge, but only
sends real changes over Zigbee.

## What the GD32 expects on the UART

Reverse-engineered from the 7gclukjs dump and reproduced by emulating its own send and parse
functions. Full write-up: [gd32-7gclukjs-protocol.md](gd32-7gclukjs-protocol.md).

- USART1 at 115200 8N1, version byte 0x02. Frames with another version are dropped.
- Every report is cmd 0x06 with one DP, at least 50 ms apart, no ACK expected and no retry.
- DP map:

  | DP | Meaning |
  |---|---|
  | 1 | state enum (none/presence/move) |
  | 104 | presence (enum 0/1) |
  | 9 | distance, dm signed; **−3 means no target** |
  | 103 | lux 0–2000 |
  | 2, 102 | sensitivities 0–10 |
  | 3, 4 | range in cm, 75 cm gates, ≤ 1000 |
  | 105 | timeout in s |
  | 0xFF | junk bool |

- Writable: DPs 2, 102, 3, 4 and 105 (type must be value). Each write is echoed back as a 0x06
  report, after rounding.
- Cmd 0x28 triggers a full dump. Cmd 0x08 is ignored.
- **Pairing behaviour that matters:**
  - At boot it sends `0x03 00`.
  - While the network status isn't 1, it sends an empty 0x02 every second.
  - It sends `0x03 01` (pairing request) when a key is held ≥ 6 s, ~10 s after a first non-1
    status, and when the status drops 1 → 0.

  A module that ever reports "not joined" while rejoining is told to leave the network. The
  firmware therefore reports "joined" for as long as it has a network.

## Facts about the stock module that shaped the firmware

- **Pins.** Only one UART: TXD = PB1, RXD = PB7. PB4 is Tuya's "wake MCU" pad, and B4/B5/A0 are
  plain module pads with unknown use on the radar PCB. The firmware drives nothing but the UART.
  slacky's thermostat build drives PA0/PB4 as LEDs and prints debug on PB5, which is fine there
  but not here.
- **Voltage sense.** On Zbit flash chips the SDK refuses every flash write (NV and OTA) unless an
  ADC reading on its VBAT pin is above 2.2 V, and in VBAT mode it drives that pin high.
  - The firmware uses **PC5**, which isn't bonded on the TLSR8258F1KET32 package (datasheet
    Table 1-3). pvvx/ZigbeeTLc and devbis/z03mmc use the same pin.
  - The boot-time voltage gate is off, since the device is mains powered.
- **Boot rules.** The B85 ROM boots the first of 0x0 / 0x20000 / 0x40000 whose byte at +8 is 0x4B
  (Telink B85 handbook). The stock Tuya bootloader checks the same byte at 0x8008, copies a fixed
  0x5000 bytes of RAM code and leaves a 3 s watchdog running.
- **Leftovers after conversion.**
  - The stock OTA leaves the downloaded image at 0x70000, overlapping the SDK's NV area
    (0x80000–0x96000). The SDK's NV code copes; see [verification.md](verification.md).
  - Tuya's own data (0xD8000–0xF0000, 0xFB000+) and the MAC at 0xFF000 are untouched, so the
    device keeps its IEEE address.

## How the conversion is done

Two existing approaches:

| | slacky1965 | romasku |
|---|---|---|
| Method | appends its own bootloader and copies it over the Tuya bootloader at 0x0 on first boot | moves the image into the ROM's own A/B slots (0x0 / 0x40000) and disables the old ones; no bootloader at all |
| Weakness | erases sector 0 before writing it, so a power cut there bricks the module (slacky#90) | — |

This project uses romasku's method with one fix. Without it, a power cut between the two
marker writes, followed by a second cut during the re-run, could erase the slot the device had
just booted from. See [how-it-works.md](how-it-works.md#first-boot-handoff).

## z2m behaviour worth knowing

- **OTA button.** z2m only shows OTA for definitions with `ota: true`, and the built-in
  ZY-M100-24GV2 definition doesn't set it. A temporary external converter adds it, which was
  also how the OTA identity above was read.
- **Local index matching** (`findMatchingOtaImage`) is on manufacturer code + image type, then
  `modelId`/`manufacturerName`. 0x1141/0xD3A3 is shared by every stock Tuya MCU bridge, so the
  stage-1 entry is pinned to `_TZE204_7gclukjs`. Tested: it isn't offered to other Tuya TS0601
  devices.
- **Re-interview and reconfigure after an update.** After an OTA, z2m re-interviews the device
  itself (`reInterview()` 5 s after "Finished update"), so a converted device is picked up as
  `ZY-M100-ZRD_1` without anyone pressing Interview. It only *reconfigures* (binds + reporting)
  when the definition `version` differs from the one it was last configured with, and the stock
  definition had already configured it as "0.0.0" a moment earlier. The custom definition
  therefore carries its own version (`1.0.3`). Verified live: z2m reconfigured the test unit
  by itself once the converter with the version was loaded.

## Sources

- wzwenzhi/Wenzhi-ZigBee2mqtt #1, #11
- Koenkk/zigbee2mqtt #19045 (324 comments)
- Andrik45719/ZY-M100 (GD32 dumps, MCU-OTA notes; #22 and #27 for the 24GV3)
- zigpy/zha device test data for `_TZE204_ya4ft0w4` and `_TZE200_ya4ft0w4`
- andi-at-1/z2m-tuya-mcu-ota
- slacky1965/tuya_thermostat_zrd
- romasku/tuya-zigbee-switch
- pvvx/ZigbeeTLc
- Tuya ZT3L datasheet and Zigbee serial protocol
- Telink TLSR8258 datasheet and B85 handbook
- Telink Zigbee SDK 3.7.2.0
- z2m 2.14.2 database, debug log and OTA query of the test unit (2026-10-18)
