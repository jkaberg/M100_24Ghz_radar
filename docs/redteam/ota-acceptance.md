> Archived red-team report, written against the tree at the time. Everything it found has been
> addressed; see [verification.md](../verification.md). Paths may have moved since (e.g. `research/`
> → `docs/`, `fw/` → `.deps/dumps/`, tests under `firmware/tests/`).

# Red-team: stock ZT3L -> custom first-stage OTA (2026-10-18)

Scope: will the slacky-style first-stage image (`1141-d3a3`, app at 0x8000 + appended bootloader)
be accepted by the radar's stock Tuya module (0x1141 / 0xD3A3 / v74) and install without bricking?

## Verdict
No hard blocker was found. There are three real ways to brick the module, and all of them need SWS
wiring to recover:
1. **The first-stage app fails to boot from the stock bootloader.** Once the stock bootloader has
   copied it to 0x8000, the old Tuya app is gone. Precedent: romasku #1.
2. **Power is lost during the bootloader copy in `main()`.** Sector 0 is erased before the new
   bootloader is written, and no voltage is checked first. Precedent: slacky #90.
3. **The stock layout differs from the thermostats' layout** (no bootloader, app not at 0x8000).
   Our module is unverified. Not likely, but possible (see Q3).

The most common failure isn't a brick: the device fails to rejoin after conversion. That is fixed by
removing power and pressing a pairing key (on our device: holding the key for 6 s or more makes the
MCU send `0x03 01`).

## Q1. Failure reports
### slacky1965/tuya_thermostat_zrd
- **#90** (Nikolay-Ch, 2025-02-15): a unit died after updating. slacky: "the bootloader died ...
  the weak point is copying the bootloader. If for some reason the marker isn't read correctly the
  bootloader can be overwritten, and if power is lost at that moment it's a 100% brick." kirill-k2:
  bricked twice after (1) many retried OTAs over a weak link, and (2) a power cycle after the
  download started but before the reboot. This was the era when the copy code ran on every boot.
  1.0.15 removed it from the main firmware (README changelog: "potential threat of boot sector
  corruption"). It now runs only in the first stage, the same as our plan.
- **#83** (2025-01/02): INVALID_IMAGE (status 150 in upgradeEndRequest) on a custom-to-custom
  update. liderman: "4 of my thermostats got bricked too", all recovered by wire (bootloader at 0,
  app at 0x8000). Exact cause not stated.
- **#49**: 2 of 4 thermostats didn't rejoin after conversion. slacky: "for a still unknown reason
  thermostats don't always update correctly". Fixed by a power cycle.
- **#162, #174, #143, #44, #128**: no rejoin after updating. Fixed by long power-off plus the
  MCU pairing key, or by moving the coordinator closer. In #174 the device was eventually reflashed
  by wire (the first wired attempt failed because an HTML page had been flashed instead of the
  image file).
- **#109, #122, #208, #172**: downloads stall or time out (ABORT, "did not start/finish", adapter
  ACK timeouts). These came from the radio path or the coordinator, or from pushing the 1141 image
  onto custom firmware. None of them bricked.
- **#77, #118, #147, #202**: the device has an EFR32 instead of Telink. The OTA isn't offered, so
  nothing is flashed. The IEEE prefix 0xa4c138 indicates Telink; ours matches.
- **#146, #154**: going back to stock is only possible by wire.

### romasku/tuya-zigbee-switch
- **#1** (2024-12-25): two devices were bricked by stock-to-custom OTA. romasku: "the build with
  the new SDK fails to boot after the update"; they had to be flashed with a programmer.
  **Lesson: an image that can't boot from the stock bootloader is a brick.** Our SDK (v3.7.1.2) and
  `main.c` are identical to slacky's, which is proven from stock, but our app code is new.
- **#64, #149**: unstable or "bricked" after OTA. The causes were Router firmware on a no-neutral
  device and a weak signal. Both recovered.
- known_issues.md: TLSR8253 512 KB devices have severe issues. 1.1.0 couldn't OTA, <1.0.18
  config-string brick, <1.0.17 4-gang couldn't OTA.
- romasku's handoff (`src/telink/ota_reformating/ensure_ota_scheme.c`) is safer than slacky's. It
  copies the image to 0x40000, verifies every page, and only then clears the TLNK flags at 0x0 and
  0x8000, relying on the ROM's multi-address boot. There is no step where sector 0 is erased before
  its replacement is written.

## Q2. Signature, encryption or other checks
- I found no report of a stock Tuya TLSR8258 module rejecting an image over a signature or
  encryption. Every failure above is a timeout, a radio problem, a user error or a boot failure.
- The Telink SDK has `g_otaEncryptionNeeded`, which rejects images without the AES tag (`ota.c`
  ~1036). I found no evidence that Tuya's 0xD3A3 build sets it, and slacky's plain-tag image is
  accepted by many 0xD3A3 thermostats. **Unverified for our appVersion 74.**
- Version: slacky's shipped first stage has fileVersion 0x10233001 (low byte 0x01). Ours is
  0x10013001, the same pattern, so a low-byte compare would have broken slacky's image too. The
  inner Telink header manuf/type (ours 0x6565/0x5244, slacky's 0x6565/0x0391) evidently isn't
  checked. Ours: CRC32 OK, 0x025d magic OK, appended bootloader at app_size has KNLT, size 0x56d4.
- "Zigbee module 1.0.10" / appVersion 74 appears in **no** success or failure report I found. The
  thermostat dumps carry no comparable version field. **Gap.**

## Q3. Is the download always at 0x70000?
- All 16 slacky thermostat dumps and 23 romasku 1 MB switch dumps share one stock bootloader
  (sha e0eb8761) with the app at 0x8000. That includes TS0601 `_TZE284_jtbgusdc`, a TuyaMCU device.
  NOVATO TS130F differs only by 2 data bytes at 0x7080.
- **Counter-examples with the same 4417/54179 IDs:** `REMOTE_HOBEIAN_BUTTON_TS0001` (1 MB and
  512 KB) and `REMOTE_HOBEIAN_TS0044` (TLSR8253 512 KB) have **no bootloader**: the app is at 0x0
  and only 0x0 carries the TLNK flag. So 0x1141/0xD3A3 alone doesn't prove the 0x8000 layout. Other
  layouts (GIRIER_B with its app at 0x80000, DEFAULT_B, HOBEIAN modules) use other image types
  (2050, 524, none).
- Consequence (inferred from Telink SDK no-bootloader OTA logic, not tested): on a no-bootloader
  stock, our 0x8000-linked image would be written to the other slot and booted there, the old flag
  cleared, and the app would crash, so the device would brick. The counter-examples are battery
  remotes. Mains TuyaMCU routers (the thermostats, jtbgusdc) all use the bootloader layout, which
  makes ours likely to match. **Not verified for this unit.**
- Data we erase: the stock thermostat dumps keep Tuya data at 0xD8000–0xE0FFF, inside the
  0x70000–0xE6000 range that `main()` wipes, plus 0xE6000–0xEFFFF, which is not wiped and is the
  SDK's NV base. That is harmless for slacky's thermostats. It means a stock restore needs a full
  dump taken before conversion.

## Q4. Recovery and power loss
- Power loss during the download: only the OTA area is touched and the old app stays. Supported by
  the slacky/romasku reports of interrupted downloads that resumed (#49 at 60%, #109, #149) with no
  brick.
- Power loss while the stock bootloader installs: no first-hand report found. **Unknown.**
- Power loss in `main()` between `erase_sector(0)` and the last page write: **brick**. The window is
  about 22 KB of writes (6 sectors). There is no voltage check before it, although the copy runs
  before `drv_platform_init`/voltage_detect. The README notes the 2.2 V flash-write risk on power
  failure (1.0.15).
- A first-stage image that crashes before the copy: brick (romasku #1).
- Re-entry: until 0x70000 is erased, every boot re-reads `hdr[0x18]` at 0x70000 and would recopy
  the bootloader. `bootloader_copy` checks the KNLT magic, manuf 0x6565 and size == BOOT_SIZE before
  writing, which addresses the #90 "marker misread" failure.
- Recovery in every case: SWS (TB-03/TB-04 kit with pvvx/TLSRPGM). Pads: 3V3, GND, SWS, RST.

## Q5. Rejoin
- The new IEEE and re-pair after conversion are expected (README: the MAC sector 0xFF000 is erased).
- Failed rejoins are common: slacky #44, #49, #128, #143, #162, #174; romasku #1, #149. Fixes:
  long power-off, the pairing key, the coordinator nearby, interview and reconfigure. For our
  device, firmware must keep the `0x03 01` key path working, otherwise the only remaining fix is a
  power cycle.

## Q6. TuyaMCU presence sensors
- I found no precedent for converting a TuyaMCU presence sensor to custom ZT3L firmware.
  romasku #407 (`_TZE200_cqtamhh5`, ZG-302ZL) only got a dump, and the uploaded dump has no TLNK
  flag anywhere and looks corrupt. TuyaMCU conversions that worked: slacky's thermostats and
  romasku #387 (Avatto dimmer `_TZE284_jtbgusdc`, 4417/54179, OTA'd manually because stock hid OTA)
  plus #498.
- Specific risk for the radar: the GD32 resets itself and has its own pairing logic (RESEARCH.md
  §12). That affects rejoining, not bricking.

## Recommendations
1. Dump the module over SWS before converting if at all possible. That settles Q3, gives a
   restore image and keeps the 0xD8000 data.
2. Use romasku-style handoff ordering (copy, verify, then switch flags) instead of erasing sector 0
   first, or at least check supply voltage first and verify the written pages.
3. Do the OTA with the device next to the coordinator on a stable supply, and don't power-cycle for
   a minute after 100%.
