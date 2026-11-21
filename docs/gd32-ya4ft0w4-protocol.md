# GD32 ⇄ Zigbee-module UART protocol: `_TZE204_ya4ft0w4` (ZY-M100-24GV3)

Image: `ZY-M100-24GV3_TZE204_ya4ft0w4.bin` from Andrik45719/ZY-M100 (64 KiB dump, code ends at
0x61C0, sha256 `0823b583…2a9c6600`). `tools/setup.sh` does not fetch it yet. Addresses are flash
offsets from 0x08000000.

This is the same vendor template as the V2 (`7gclukjs`), rebuilt with a different register
allocation, so no function sits at the V2 address and the code had to be re-traced. Most
behaviour is unchanged. This document lists what was checked and what differs. Where a row says
"as V2", the details in [gd32-7gclukjs-protocol.md](gd32-7gclukjs-protocol.md) apply unchanged.

Tools:
- `tools/re/disall.py <bin> 61c0`: disassembly.
- `tools/re/emu_ya4ft0w4.py [bin]`: Unicorn harness. It hooks `zigbee_uart_putc` (0x5a80),
  calls the TX functions, the report scheduler and the RX parser, and maps 128 KiB of flash
  because of the DP101 page (§7). Byte strings marked **[emu]** come from it.
- `tools/check_frames_gd32.py <tx_frames.bin> <bin>`: now picks the V2 or V3 addresses from the
  product id in the image.

Legend: **[dis]** read from the disassembly, **[emu]** confirmed by emulation, **[inf]** inference.

---

## Differences from 7gclukjs at a glance

| | 7gclukjs (V2) | ya4ft0w4 (V3) |
|---|---|---|
| Product info | `{"p":"7gclukjs","v":"1.0.0"}` | `{"p":"ya4ft0w4","v":"2.0.1"}` |
| 0x0B version byte | 0x40 | **0x81** |
| DP table | 11 entries | 12: adds **(0x65, bool)** = DP101 |
| DP9 with no target | −3 (0xFFFFFFFD) | **0** (state none) |
| DP9 gate | none | **suppressed while DP101 = 0**, except the "0" on target loss |
| DP104 | enum 0/1 (presence) | **enum 0..11: presence plus how long** (§3) |
| Full dump | 11 frames | **12 frames** (DP101 last) |
| Non-volatile state | none | **DP101 in flash page 0x0801F800** |
| Writes accepted | DP 2, 3, 4, 102, 105 | DP 2, 3, 4, **101**, 102, 105 |

Everything else (UART, framing, dispatcher, pairing, ACK handling, lux, timers, watchdogs,
MCU-OTA) behaves as on the V2.

---

## 0. Hardware / clocks / UART

| Item | Finding | Evidence |
|---|---|---|
| Zigbee UART | **USART1 (0x40004400)**. Vector 44 → 0x684 → RX push 0x5a14 into the ring at 0x200005ca (head 0x20000140, tail 0x20000144). TX blocks on TBE (0x6fc) | [dis] |
| Radar UART | USART0 (0x40013800), vector 43 → 0x620 | [dis] |
| Baud | **115200 on both**, `usart_baudrate_set(…, 0xE1<<9)` at 0x2292 / 0x22a6 in the init 0x21b0 (called for index 0 and 1). 8N1 by reset default [inf] | [dis] |
| System clock | `SystemCoreClock` .data init 0x016E3600 = 24 MHz (0x6190) | [dis] |
| Main loop | 0x2814: `if (elapsed(2 ms)) { 0x570; 0x2320 keys; 0x4cf8; 0x13b4 report tick; 0x40e0 radar/distance tick }`, then every pass 0x1c54, **0x5db4 UART parser**, 0x4a30, 0x3270, 0x2604 lux | [dis] |

---

## 1. Frame writer, parser, version byte, seq: as V2

- Writer `zigbee_uart_write_frame(cmd, len)` @ **0x5fc0** [dis][emu]. TX buffer 0x20000760.
  Header `55 AA 02 00 <seqL> <cmd> <lenH> <lenL>`, version byte 0x02 (0x5fe0), seqH constant
  0, seqL from the shared counter 0x20000104 (`next_seq` 0x4c58, wraps at 0xFFF1). Checksum
  0x1c64 (byte sum). Sent via 0x5f7c → 0x5a80.
- Parser @ **0x5db4** [dis][emu]. Linear buffer 0x200006d4, count 0x2000075e. byte[2] must be
  **0x02** (0x5e72); versions 0x00, 0x01 and 0x03 are dropped [emu]. Frame ≤ 0x89 bytes. Bad
  checksum skips 3 bytes. Valid frames go to the dispatcher 0x115c.

## 2. Receive dispatcher @ 0x115c (jump table 0x11c0, cmd ≤ 0x28)

Same command set and behaviour as V2 [dis][emu]:

| cmd | Handler | Behaviour |
|---|---|---|
| 0x00 | 0x1264 | nothing |
| 0x01 | 0x32d4 | `55 AA 02 00 ss 01 00 1C {"p":"ya4ft0w4","v":"2.0.1"} cs` |
| 0x02 | 0x6044(data[0]) | ack `02` len 0, then status handling (§5) |
| 0x03 | 0x2f9c | nothing (empty switch) |
| 0x04 | loop 0x1292 → 0x5d20 → 0x1826 | §6 |
| 0x05, 0x06, 0x07, 0x09, 0x0A | (none) | nothing |
| **0x08** | 0x1304 | **nothing, the DP query is ignored** |
| 0x0B | 0x39c8 | `55 AA 02 00 ss 0B 00 01 81 cs` (2.0.1 → 2<<6 \| 0<<4 \| 1) |
| 0x0C / 0x0D / 0x0E | 0x3898 / 0x2cd8 / 0x2ebc | MCU-OTA (§8) |
| 0x24, 0x25 | 0x132c / 0x132a | nothing |
| **0x28** | 0x132e → 0xe40 → 0x3808 | all report flags set: full dump (§3). No direct reply |

The MCU sends 0x01, 0x02, 0x03, 0x06, 0x0B and 0x0C–0x0E, nothing else [dis].

---

## 3. DP reporting

### Primitives (cmd 0x06, one DP per frame) [dis][emu]
| fn | Payload |
|---|---|
| 0x2a08 bool(dpid, v) | `dpid 01 00 01 v` |
| 0x2aa6 enum(dpid, v) | `dpid 04 00 01 v` |
| 0x2b1a value(dpid, u32) | `dpid 02 00 04 BE32` |

### DP table @ 0x6134 (dpid, type), used to type-check writes [dis]
`(FF,bool) (FF,bool) (01,enum) (02,val) (03,val) (04,val) (09,val) (67,val) (69,val) (66,val) (68,enum) (65,bool)`.

### Report scheduler @ 0x13b4, 25-tick slot (≈ 50 ms), one frame per slot [dis][emu]
Flag word 0x200000f0 (set 0x3864, clear 0x381c, test 0x384c). State struct `CFG` = 0x20000374.

| bit | DP | type | Value |
|---|---|---|---|
| 0, 1 | 0xFF | bool | template switches (junk), as V2 |
| 8 | **1** state | enum | CFG+2: raw 0 → 0, 2 → 1, 1 or 3 → 2 (as V2) |
| 15 | **104** | enum | **0..11, see below** (0x46f8) |
| 12 | **9** distance | value | state 0 → **0**. Else if DP101 = 0 → **nothing sent**, flag cleared. Else `(raw_cm + 5)/10 − 3` (raw u16 0x200000fe). So raw < 25 cm still wraps negative while a target is present, and 25–34 cm gives 0 [emu] |
| 7 | **103** lux | value | u16 CFG+0x18, 0..2000 |
| 9 | **2** move sens | value | byte CFG+3 |
| 14 | **102** presence sens | value | byte CFG+4 |
| 16 | **3** min range | value | min gate (CFG+1) × 75 cm |
| 10 | **4** max range | value | max gate (CFG+0) × 75 cm |
| 13 | **105** timeout | value | u16 CFG+0x12 (radar readback, s) |
| 17 | **101** find_switch | bool | CFG+0x34 |

[emu] DP9 slot:
```
state=0 find_switch=1 raw=0     09 02 00 04 00 00 00 00
state=0 find_switch=0 raw=0     09 02 00 04 00 00 00 00
state=2 find_switch=1 raw=0     09 02 00 04 ff ff ff fd
state=2 find_switch=1 raw=25    09 02 00 04 00 00 00 00
state=2 find_switch=1 raw=150   09 02 00 04 00 00 00 0c
state=2 find_switch=0 raw=150   (nothing sent)
```

**DP104 is not a presence bool on this MCU** [dis][emu]. 0x47ac counts seconds with no target
(0x20000370) and with a target (0x2000036c). Both are cleared when the state leaves none (0x3cfc), so they mean
"time since the target left" and "time since a target appeared". Both are held at 0 while the
periodic-report enable CFG+0x24 is 0. 0x46f8 maps them to:

| time in current state | no target | target |
|---|---|---|
| < 60 s | 0 | 1 |
| 60–179 s | 2 | 7 |
| 180–299 s | 3 | 8 |
| 300–899 s | 4 | 9 |
| 900–1799 s | 5 | 10 |
| ≥ 1800 s | 6 | 11 |

It is sent only with state changes and dumps, so the value is stale between them. z2m ignores
DP104 for this model. Odd values below 6 and 7..11 mean "target", so `val != 0` is wrong as a
presence test.

### What sets the flags: as V2
- **DP9** (0x40e0, every 500 ticks ≈ 1 s): same rules as V2 (report on any raw cm change,
  once on "unchanged", and once with "0" when the state goes to none). The DP101 gate is
  applied later in the scheduler, so the 1 s tick keeps running.
- **DP1 + DP104**: debounced state change in the radar frame handler 0x3a5c (0x3d1a/0x3d20),
  not while an MCU-OTA is running. It also re-arms the 6 × 10 s dump burst (0x3d2e).
- **DP103**: 0x2604, identical maths and thresholds to V2 (lux = (4095−adc)·660/1024, IIR ¾ old,
  clamp 2000; Δ > 10, or Δ ≥ 3 and lux < 50, or Δ ≥ 6 and lux ≤ 199; ≥ 501 ms apart).
- **DP101**: only in dumps and as the write echo.
- **Full dump** (0x3808 sets all flags) = **12 frames at 50 ms**: FF, FF, 1, 104, 9, 103, 2, 102,
  3, 4, 105, 101 [emu]. Triggers as V2: status 1 after non-1 (main loop 0x284a sets CFG+0x25,
  0x2904 → 0x3110), cmd 0x28, and the periodic re-reports while CFG+0x24 = 1 (one dump every
  10 s, 6 times, after each state/lux event, 0x2886; otherwise one every 10 min, 0x28bc).
  CFG+0x24 is set when the radar answers a config read (frame type 4 or 0xF4, 0x3aee) and
  cleared by status 0. While it is 0, the main loop asks the radar for its config every 5 s
  (0x28e8 → 0x200e).

### Andrik45719's patch (12 bytes) confirms these locations
- 0x1560: `bl 0x2b1a` (DP9 = 0 when state is none) → NOPs. Only the "no target" report goes,
  ordinary distance reports stay. Hence the file name "TD_0_disable".
- 0x26fc: lux minimum interval `movw r1, #0x1f5` (501 ms) → `#0x2710` (10,000 ms).
- 0x29e0: burst interval literal 10,001 → 60,001 ms.
- 0x29ec: radar watchdog literal 60,000 → 90,000 ms.

## 4. ACK / retry: as V2
No ACK wait, no retransmit; incoming 0x05/0x06 are ignored [dis].

---

## 5. Network status, start-up, pairing: as V2 [dis][emu]

- Boot (main 0x2790 → 0x2f78) sends **`55 AA 02 00 ss 03 00 01 00 cs`**.
- Every 500 ticks (≈ 1 s) while status (0x200006d3) ≠ 1: **cmd 0x02 len 0** (0x1406 → 0x2bd8).
- On 0x02 from the module (0x6044): ack; first one sets 0x2000013c = 1; status **0** clears
  CFG+0x24 and, **if the previous status was 1, starts pairing** (0x16cc → `03 01`); 1 or 2
  stops the pairing LED (0x16e0); 3 nothing. Status is stored.
- 10 s after the first status frame, if status ≠ 1: one `03 01` (0x1422–0x143c).
- Pairing LED/flag times out after 180 s (0x13d4–0x13ec).
- The key handler 0x2320 also sends `03 01` (calls at 0x23ce and 0x2474; not traced further).
- No gating: reports go out whatever the status is.

So the module must answer the polls with `02 01` and keep doing so; **never send `02 00` after
`02 01`**. `02 01` after anything else triggers a full dump.

---

## 6. DP writes (cmd 0x04) [dis][emu]

0x5d20 looks the DP up in the table and **requires the type byte to match** (else silent). The
value is decoded as 4-byte big-endian. 0x1826 handles **DP 2, 3, 4, 101, 102, 105**. DP 1, 9,
103, 104 and 0xFF are ignored with no reply.

| DP | Validation | Effect | Echo (cmd 0x06) |
|---|---|---|---|
| 2, 102 sens | low byte ≤ 10, **0 is valid**; otherwise silently ignored | radar cmd 4 / 5 | raw value, even when rejected (DP2 = 15 → 15) |
| 3 min range | v ≤ 1000, gate = min(v/75, 12), gate+1 ≤ max gate | radar cmd 2 | accepted: raw v. Rejected: current min × 75 (DP3 = 2000 → 75) |
| 4 max range | v ≤ 1000, gate = min(v/75, 12), gate ≥ min gate + 1 | radar cmd 1 | accepted: raw v (450 → 450). Rejected: current max × 75 (50 → 600) |
| 105 timeout | none in the handler. The radar command builder 0x45c2 **drops the whole command if v > 0xFFFF** (or a gate > 12) | radar cmd 3 | raw v (70000 → 70000, not applied) |
| **101** find_switch | **type must be bool (1)**; any non-zero = 1 | CFG+0x34 = 0/1, **written to flash** (0x3fc4 → 0x3a34) | `65 01 00 01 <0/1>` |

Commands 1–3 all go out as one radar frame carrying min gate, max gate and timeout together
(0x45c2). Two DPs in one 0x04 frame give two echoes [emu].

---

## 7. Settings persistence and the ~3-day reset

**DP101 is the only setting the GD32 stores** [dis][emu]. 0x0e9c/0x0ef0 read and write a 2 KiB
flash page at **0x0801F800** (FMC unlock 0x1bd4, page erase 0x1a44, program 0x1bfc). The record
is 8 bytes at offset 0: `u32 find_switch, u32 0x5A5A5A5A`. At boot (0x42f0) a missing magic sets
find_switch = **1** and writes the page. So DP101 **defaults to on and survives resets**. Every
DP101 write erases and rewrites the page.

[inf] 0x0801F800 is at 126 KiB. A 64 KiB GD32E230 has no flash there, so either this board
carries a 128 KiB part or the access faults. The dump is 64 KiB and cannot tell.

Sensitivities, ranges and timeout are **not** stored by the GD32. They live in the radar SoC. The
GD32 reads them back (config read every 5 s until it gets an answer) and reports what the radar
says. Whether the radar SoC keeps them over a power cut is not visible here [inf]. Until the
read-back arrives, CFG is zero, so a dump in the first seconds after boot reports DP 2, 102, 3,
4, 105 as 0 [dis]. This matches the "0 reported at times" in z2m reports.

**The self reset** (main loop 0x2916–0x295c, same as V2): NVIC reset after a 2 s delay when
1. no radar frame for > 60 s (CFG+0x2c), or
2. RTC_BKP0 ≠ 0 (§8), or
3. **uptime ≥ 0x0F731401 ms (72 h) and state = none.**

Case 3 is the "resets after 3 days" in Andrik45719/ZY-M100#27: the first moment the room is empty
after 72 h of uptime. After the reset the MCU sends `03 00`, polls `02` every second until it
gets `02 01`, re-reads the radar config, and dumps all 12 DPs on `02 01`. DP101 comes back from
flash. The GD32 holds no other setting to lose.

---

## 8. MCU-OTA: as V2, does not install (quick check) [dis]

Same structure at new addresses. The notify handler 0x3898 checks the PID copied to 0x200000cd.
Staging is at **0x0800A000–0x0800FFFF**, size < 0x6001 (0x324c), and block 0 needs an SP in
0x2000xxxx (0x3222). The 0x0E handler 0x2ebc sets **RTC_BKP0 = 0x11AA33DD** (0x31dc) when the
module answers `0E 00`. Nothing reads BKP0 except the reset check in the main loop (0x2928). No
other code reads 0x0800A000, and VTOR is set only at start-up. Result: same reboot loop as V2.
**Never send 0x0C/0x0D/0x0E.**

---

## 9. Implications for the module firmware

`radar_variant_t` for `ya4ft0w4`:

```c
{
    .pid            = "ya4ft0w4",
    .dp_state       = 1,
    .dp_presence    = RADAR_DP_NONE,   /* DP104 is a duration enum here, not presence */
    .dp_distance    = 9,
    .dp_illuminance = 103,
    .dp_move_sens   = 2,
    .dp_pres_sens   = 102,
    .dp_range_min   = 3,
    .dp_range_max   = 4,
    .dp_timeout     = 105,
    .distance_mm    = 100,             /* dm, (cm + 5)/10 - 3 as V2 */
    .distance_none  = 0,               /* 0 = no target; <= 0 also covers < 35 cm */
    .range_mm       = 10,              /* cm, 75 cm gates */
    .sens_max       = 10,
    .range_max_cm   = 1000,
    .timeout_min_s  = 1,
    .timeout_max_s  = 15000,           /* z2m limit; MCU drops > 65535 */
}
```

Required:
1. **`dp_presence = RADAR_DP_NONE`.** With 104 the module would report presence for "no target
   for 1–30+ min" (values 2..6). The in-progress `radar.c` change that derives presence from DP1
   covers this.
2. **`distance_none = 0`.** The V2 value −3 would turn the V3's "no target" 0 into a 1 cm
   target. Cost: a target at 25–34 cm reads as no target.
3. No change for UART, baud (115200), version byte (0x02), status handling, 0x28 or the
   writes the module already sends. `check_frames_gd32.py` passes all 9 distinct frames from
   `make -C firmware/tests unit` against the V3 image [emu].

Recommended:
4. **DP101.** Distance needs find_switch = 1. It defaults to 1, but a unit set to 0 under the
   stock module keeps 0 in flash, and then DP9 only ever reports 0. The module should read
   DP101 from the dump and, if it is 0, write `65 01 00 01 01` once. Do not write it on every
   boot: each write erases a flash page. DP101 does not need to be exposed; the module already
   rate-limits distance.
5. Drop DP101 and DP104 frames as unknown DPs (they already count as `dp_dropped`), and drop
   DP 0xFF as on the V2.
6. Ignore DP 2/102/3/4/105 = 0 in a dump that arrives before the first DP1, or accept them
   and let the next dump correct them. Today the module would publish range 0 briefly after an
   MCU reset.
7. The module's 0x08 query after product info is harmless but useless; 0x28 is the way to get
   the state.

Open risks:
- The flash page at 0x0801F800 is outside the dump. If the real chip has 64 KiB, the DP101 path
  could fault [inf]. Test one DP101 write on hardware before relying on item 4.
- Radar-SoC persistence of sens/range/timeout over a power cut is unknown.
- Nothing here is checked on a V3 unit. `_TZE200_ya4ft0w4` and `gkfbdvyx` are other firmware.
