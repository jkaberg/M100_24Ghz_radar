> Archived red-team report, written against the tree at the time. Everything it found has been
> addressed; see [verification.md](../verification.md). Paths may have moved since (e.g. `research/`
> → `docs/`, `fw/` → `.deps/dumps/`, tests under `firmware/tests/`).

# Red-team review: hardware assumptions for the custom Zigbee-module firmware

Device: ZY-M100-24GV2, `_TZE204_7gclukjs`, sold as WZ-M100-W. Reviewed 2026-10-18.
Tags: **[V]** verified from a primary source (photo, datasheet, disassembly, SDK source).
**[S]** speculation or inference.

## Verdict

**No blocker found.** Assumptions (a), (b), (d) and (e) hold. Assumption (c) is the right call,
and the evidence makes it more important than it looked. Two residual risks are worth a
bench test before a ceiling-mounted unit gets the OTA image (see "Residual risks").

| # | Assumption | Result |
|---|---|---|
| a | Module is a ZT3L (TLSR8258F1KET32, 1 MB) | **Holds for 24GV2/7gclukjs** [V]. ZS3L is real, but on the older ZY-M100-L board |
| b | UART on PB1 (TX) / PB7 (RX) | **Holds** [V]. It is the ZT3L's only UART pad pair |
| c | Leave PB4, PA0 and PB5 undriven | **Correct and needed.** Tuya's MCU firmware assigns PB4/PB5 as wake lines [V-doc]; their use on this PCB is unknown |
| d | PB3 (app) and PC5 (bootloader) are not bonded out | **Holds** [V]. Neither pin exists on the QFN32 package |
| e | GD32 USART1 at 115200 8N1 | **Holds** [V]. Re-checked: USART1 = GD32 PA2/PA3, and the pin table matches |

## 1. Module identity

- **24GV2 wall board ("ZY-M100-V1.4-5.8G-24G-23.1211"), 7gclukjs:** the module's PCB silkscreen
  reads `ZT3L`, and its shield label reads `0000.00.00.ZT3L / ZYWD-03400289` [V: photo by
  jgarridoalcazar in Andrik45719/ZY-M100#4,
  https://github.com/user-attachments/assets/129481f0-742b-43f1-91e9-b0b538dd0a09, zoomed crop].
  The back of the same board reads "ZY-M100-V1.4-24G".
- **24GV2 ceiling board ZY_V230605-24G-V1.2/V1.3, 7gclukjs:** tandarra reports a ZT3L
  [V-report: #19045 comment 2361016040]. His photos show only the radar side, so this is his
  word, not a photo.
- **Test unit:** node descriptor manufId 0x1141 (Telink), OTA query 0x1141 / 0xD3A3 / v74,
  IEEE prefix `a4:c1:38`, which is the Telink OUI [V: research/z2m-debug-7gclukjs.log].
  An EFR32 (ZS3L) module would not answer with Telink's manufacturer code and the TLSR8258
  OTA image type.
- **"ZS3L" in Andrik's README is not a typo.** The README's "tech info" section describes the
  ZY-M100-L (5.8 GHz, JYSJ_5807_A01 radar, board ZY_JYSJ_V220723-L0K2 / "2022.7-V3.0"). His
  photos `pix/wall_1.jpg` and `pix/ceiling_2.jpg` show a module whose shield label reads
  "Model: ZS3L, P/N 2.01.03.00005" and whose PCB silkscreen reads ZS3L [V]. So at least some
  ZY-M100-L units ship with the Silabs EFR32MG21 module. adiov SWS-dumped a "ZT3L" from a
  ZY-M100-L, which suggests that model shipped with both modules over time [S].
- **Consequence:** the OTA image matches 0x1141/0xD3A3, so a ZS3L unit would never be offered
  it. The manufacturer/image-type pair is a working safety interlock. **Do not publish an OTA
  index entry that matches on model or manufacturerName alone.**

## 2. TLSR8258 QFN32 and ZT3L pad map

Source: Telink DS-TLSR8258-E17, Figure 1-5 / Table 1-3, "Pin layout for
TLSR8258F512ET32/TLSR8258F1KET32" [V]
(https://github.com/SoCXin/TLSR8258/blob/HEAD/docs/, page 29-31).

GPIOs bonded on the QFN32 package: **PD4, PD7, PA0, PA1, PA7(SWS), PB1, PB4, PB5, PB6, PB7,
PC0, PC1, PC2, PC3, PC4, PD2, PD3**. Nothing else.

- **PB3: not bonded** [V]. **PC5: not bonded** [V]. PB0, PB2, PA2-PA6, PC6, PC7, PD0, PD1, PD5
  and PD6 are not bonded either.
- Driving an unbonded pad high is harmless. Nothing outside the die can be shorted or
  back-powered. Precedent: pvvx/ZigbeeTLc picks an unbonded pin on purpose for VBAT
  measurement (`src/battery.h`: `#define GPIO_VBAT GPIO_PC5 // missing pin on case
  TLSR825xF512ET24`). devbis/z03mmc uses PC5 with `VOLTAGE_DETECT_ENABLE 1`. **PB3 is in
  `ADC_GPIO_tab`** (sar_aio<3>, SDK `platform/chip_8258/adc.c:46`), so the ADC channel is set
  correctly.
- Inside the module: the ZT3L has a PCB antenna and no RF switch. The flash is in the package
  ("F1K" = 1 MB internal), and the crystal uses the dedicated XC1/XC2 pins. Unbonded pins
  can't be used inside the module anyway.

ZT3L pad map (Tuya ZT3L datasheet, https://developer.tuya.com/en/docs/iot/zt3l-module-datasheet?id=Ka438n1j8nuvu) [V]:

| Pad | Signal | IC pin | Pad | Signal | IC pin |
|---|---|---|---|---|---|
| 1 | RST | (RESETB) | 10 | PC0 | 20 |
| 2 | PC4 (ADC) | 24 | 11 | PD4 | 1 |
| 3 | EN (tied to RST) | | 12 | PA0 | 3 |
| 4 | PD7 | 2 | 13 | PB4 | 14 |
| 5 | PD2 | 31 | 14 | PB5 | 15 |
| 6 | PC3 | 23 | 15 | RXD = PB7 | 17 |
| 7 | PC2 | 22 | 16 | TXD = PB1 | 6 |
| 8 | 3V3 | | 17 | SWS = PA7 | 5 |
| 9 | GND | | | | |

The pin numbers match Telink's QFN32 table, so the module really is the QFN32 die. PA1, PB6,
PC1 and PD3 are bonded but have no pad.

## 3. What the radar PCB connects to the module

- In the V1.4 photo, all 16 castellations are soldered [V]. That is normal for mechanical
  retention and says nothing about which pads carry nets. No trace-level photo or schematic of
  the module footprint was found. **Pad routing on the radar PCB remains unverified** beyond
  UART and power.
- **The GD32 side is fully accounted for** [V: disassembly of
  `fw/ZY-M100-24GV2_TZE204_7gclukjs.bin`, GPIO helpers 0x1c66 (mode), 0x1c1c (BC), 0x1c2c (BOP),
  0x1c3c (ISTAT read)]. GD32E230F8P6 TSSOP20 GPIO use:
  - PA0: push-pull output, LED (pin table 0x5b5c, init via 0x1e90, toggled at 0x22ac/0x2308)
  - PA7: push-pull output, set high at init (0x1e64) [S: second LED or radar enable]
  - PA1, PB1: inputs with pull-down, keys (0xe0e/0xe22, read at 0x20a6/0x2148)
  - PA5: analog, illuminance (0x1de4)
  - PA4, PA6: alternate function, timer (0x4b64/0x4c04, TIMER13 at 0x40002000)
  - PA2/PA3: USART1 to the module. PA9/PA10: USART0 to the radar (tables at 0x5b40-0x5b54)
  - PA13/PA14: SWD (also written as "template outputs" for DP 0xFF)

  That is every GPIO on the 20-pin package. **The GD32 does not read a "network status" pin
  and does not drive a module reset or wake line.** Network state comes only from UART cmd 0x02
  (research/gd32-7gclukjs-protocol.md §5). The GD32 itself drives the pairing LED and reads
  the key, so it uses Tuya's "MCU handles LED/key" mode.
- Not ruled out [S]: a module pad could go to GD32 **NRST** or **BOOT0** (not visible in
  firmware), or to an LED net the GD32 also drives (PA0/PA7). Our firmware never drives PB4, PA0,
  PB5, PC4 or any other pad, so none of these can be harmed. **The risky option would have been
  slacky's board file**: his bootloader drives PB4/PA0 push-pull and blinks them. If either pad
  were tied to GD32 PA0/PA7 (outputs), NRST or BOOT0, that would mean output contention or
  resetting/boot-moding the GD32.
- **Stock Tuya firmware pins:** Tuya's ZT3L wake-up table for its MCU serial firmware lists
  TX = PB1, RX = PB7, "wake up MCU" = **PB4**, "wake up module" = **PB5/PD2**, with the caveat
  that this "may vary depending on the firmware" (support.tuya.com, e.g.
  https://support.tuya.com/en/help/_detail/K9hrwg0wuyln0 and Kckzmqih26442). That applies to
  the low-power variant. This unit is a mains-powered router with a GP proxy, and the GD32 has
  no wake GPIO, so the stock module almost certainly uses none [S]. The doc still flags
  **PB4/PB5 as the most likely pads to be routed on a Tuya MCU-module board**, which makes
  leaving them as inputs the right choice.
- **PC4 (pad 2):** its use on this PCB is unknown. Moving VBAT sensing off it was correct. On
  slacky's thermostats PC4 is driven high permanently.

## 4. SDK side effects checked

- `drv_platform_init()` (`proj/drivers/drv_hw.c:284-305`): with `VOLTAGE_DETECT_ENABLE ON`
  (our app), `adc_vbat_pin_init()` sets the pin to output-high (`adc.c:87-94`). With it OFF and
  Zbit flash, the same init runs. The bootloader keeps it OFF and uses PC5. All three cases
  only touch unbonded pads [V].
- Default pulls (`gpio_default.h`): none, except SWS PA7 = 1 MΩ pull-up [V]. With no pull, the
  untouched pads float as inputs, which is harmless.
- The RX line PB7 has no explicit pull in our code. Telink's `uart_gpio_set()` (in the binary
  library) is believed to add a 10 kΩ pull-up on RX [S]. Either way, while the GD32 resets
  (it self-resets: 2 s watchdog path, 72 h uptime, radar silence > 60 s), the line may glitch.
  The frame parser must tolerate junk bytes and resync on `55 AA`.

## 5. RF power

- `RF_POWER_INDEX_P10p46dBm` is the TLSR8258 table maximum and the SDK default for 8258
  (`zigbee/common/includes/zb_config.h:149`). slacky uses the same [V].
- The ZT3L datasheet gives "+10 dBm typical max at 250 kbps" [V]. 10.5 dBm conducted plus
  3.09 dBi antenna gain is about 13.6 dBm EIRP. That is far below the ETSI EN 300 328 limit of
  20 dBm EIRP and the FCC 15.247 limit [V-arith].
- Formally, the product's CE/FCC grant covers Tuya's firmware. At the same power the practical
  risk is nil, but it is a modified radio [S]. Supply current at +10 dBm is about 9 mA TX
  (datasheet), which a mains AC/DC supply handles easily.

## 6. Flash chip / MID

- Our app has `FLASH_PROTECT_ENABLE` undefined (0), so the per-MID lock/unlock table
  (`drv_flash.c:45-55`) is never used. **An unknown MID cannot break locking.** The SDK's 8258
  1 MB parts are 0x1460C8 / 0x011460C8 (GD) and 0x14325E (Zbit) [V]. Read, write and erase use
  generic SPI opcodes.
- **Hard dependency:** `internalFlashSizeCheck()` (`drv_hw.c:96-104`) runs `while(1)` if the
  MID capacity byte is < 0x14 (1 MB) and the build is 1 MB. This happens before the watchdog
  starts, so it would be a permanent hang (SWS recovery). On a TLSR8258**F1K** the flash is
  1 MB by part number. The stock OTA area at 0x70000 also implies 1 MB. Risk is negligible [S].
- Zbit flash (0x14325E/0x13325E) adds a voltage gate in the library (`flash.c:153-160`): writes
  are silently dropped if the ADC reading is below `g_flashWorkVol` or fluctuates. The app adds
  its own gate (`drv_flash.c:182/201/232`, < 2.2 V means skip). Both depend on the PB3 VBAT
  reading being right.

## Residual risks (not blockers)

1. **VBAT read on PB3 controls boot and flash writes.** With `VOLTAGE_DETECT_ENABLE ON`,
   `voltage_detect(1)` at power-on needs 5 consecutive readings > 2.2 V within 1 s, or it calls
   `SYSTEM_RESET()`. A bad reading means a boot loop, the OTA client never runs again, and only
   SWS can recover the unit. Unbonded-pad VBAT sensing is proven (pvvx: PC5, devbis: PC5), and
   PB3 is a valid ADC channel, but **PB3 itself has no field precedent that we found**.
   Mitigations, either one:
   (a) use **PC5**, the pin with field precedent, which is also unbonded on QFN32, or
   (b) set `VOLTAGE_DETECT_ENABLE OFF` on this mains-powered device. That removes the
   boot-time reset loop. The SDK then still drives the pin and sets the Zbit flash threshold
   only if the flash is Zbit.
   Either way, first flash a unit with SWS wired (pad 17) on the bench.
2. **Unverified pad routing** (PC4, PD2, PD4, PD7, PC0, PC2, PC3, PA0, PB4, PB5). Harmless
   **as long as no future change drives them**. Add a comment or `#error` guard so nobody later
   enables `UART_PRINTF_MODE` (PB5 TX) or the bootloader LED outputs on this board.
3. The ZS3L variant exists on ZY-M100-L. Keep the OTA match strict (manuf 0x1141 + image type
   0xD3A3 + version cap) as the interlock.

## Sources

- Photos: Andrik45719/ZY-M100#4 (24GV2 V1.4 board with ZT3L),
  `upstream/ZY-M100/pix/wall_1.jpg`, `ceiling_2.jpg` (ZY-M100-L with ZS3L), #19045 tandarra
  photos (V1.2/V1.3 radar side).
- Koenkk/zigbee2mqtt#19045: adiov 1861047510 (SWS pad on the ZT3L underside), tandarra
  2361016040 (ZT3L, ZY-V230605-24G-1.3).
- Telink DS-TLSR8258-E17 (QFN32 pin table). Tuya ZT3L datasheet. Tuya support FAQ on ZT3L
  wake pins.
- SDK: `proj/drivers/drv_hw.c`, `proj/drivers/drv_flash.c`, `platform/chip_8258/adc.c`,
  `flash.c`, `gpio_default.h`.
- GD32 disassembly of `fw/ZY-M100-24GV2_TZE204_7gclukjs.bin` (addresses above).
- pvvx/ZigbeeTLc `src/battery.h`, devbis/z03mmc `src/app_cfg.h`, romasku/tuya-zigbee-switch.
