# GD32E230 ⇄ Zigbee-module UART protocol: `_TZE204_7gclukjs` (ZY-M100-24GV2)

Image: `.deps/dumps/ZY-M100-24GV2_TZE204_7gclukjs.bin` (fetched by `tools/setup.sh`). Addresses are flash offsets from 0x08000000.
The code is unoptimised Keil/armclang output (-O0), so the control flow reads almost 1:1 as C.

Tools:
- `tools/disall.py <bin> [endhex]`: linear-sweep disassembly with PC-relative literals resolved.
- `tools/emu_7gclukjs.py`: a Unicorn harness that calls the TX functions and the RX parser. It
  hooks `zigbee_uart_putc` (0x54e8) and prints the exact bytes on the wire. Every byte string
  below marked **[emu]** comes from this harness.

Legend: **[dis]** = read directly from the disassembly. **[emu]** = confirmed by emulation.
**[inf]** = inference, not proven.

---

## 0. Hardware / clocks / UART

| Item | Finding | Evidence |
|---|---|---|
| Zigbee UART | **USART1 (0x40004400)**. TX is blocking and polls the TBE flag (0x6d8). RX is per-byte in the USART1 IRQ (vector 44 → 0x664 → 0x547c), which pushes into a ring at 0x200005ba (head 0x20000140, tail 0x20000144) | [dis] |
| Radar UART | USART0 (0x40013800), IRQ vector 43 → 0x604 → 0x44dc | [dis] |
| Baud | **115200 on both**. `usart_baudrate_set(USARTx, 0xE1<<9 = 115200)` at 0x1ff8 / 0x200c. The init (0x1f34) runs for index 0 and 1 (table of USART bases at 0x5b40). Word length and parity are never set, so the reset default 8N1 applies [inf: reset defaults] | [dis] |
| System clock | 24 MHz: PLL = IRC8M/2 × 6 (CFG0 PLLMF bits = 0b0100, PLLSEL = 0), at 0x4aaa–0x4af6. `SystemCoreClock` .data init value is 0x016E3600 = 24,000,000 (0x5bc0). The USART divisor comes from the live RCU clock (0x31f0), so 115200 is correct either way (BRR ≈ 208, +0.16 %) | [dis] |
| Time base | SysTick reload = SystemCoreClock/1000 (0x4b20) → 1 ms. The ISR (0x27c) increments the ms counter 0x20000120 (read by 0x2cc0) | [dis] |
| Main tick | Main loop 0x2558: `if (elapsed(2 ms)) { 0x558; 0x2080 keys; 0x479c; 0x127c protocol/report tick; 0x3ce4 radar/distance tick }`. Then, every iteration: 0x5814 UART parser, 0x44f4 radar parser, 0x2f48, 0x234c illuminance | [dis] |

"1 tick" below = 2 ms (≥ 2 ms; the stamp is reset to `now`, so there is slight drift).

---

## 1. Frame writer, parser, version byte, sequence numbers

### Writer: `zigbee_uart_write_frame(cmd, len)` @ **0x5a08** [dis][emu]
- TX buffer 0x20000750, payload at +8 (filled by `put_byte` 0x4770 and `put_buf` 0x4734).
- Header: `55 AA 02 <seqH=00> <seqL> <cmd> <lenH> <lenL> <data…> <sum>`.
  - **Version byte 0x02** (0x5a2c).
  - **seqH is always 0x00** (0x5a40 stores a constant 0). **seqL = low byte of the MCU's own
    16-bit counter** 0x20000104 (`next_seq` 0x470c: `++seq; if seq >= 0xFFF1 then seq = 1`).
    One counter is shared by every frame the MCU sends (replies and reports alike). The MCU
    **never echoes the module's seq**: the dispatcher reads bytes 3–4 of the RX frame (0x1066)
    and never uses them.
  - Checksum = 8-bit sum of every byte from 0x55 through the last data byte (0x1a08).
- Sent byte by byte via 0x59cc → 0x54e8 → 0x6d8 (busy-wait on TBE).
- Cross-check with the z2m log: the Tuya `seq` arrives as 0x9000, 0x9100, 0x9200, …, which is
  bytes `00 9x` read little-endian. So the module forwards the MCU's seq bytes unchanged, and
  consecutive DP9 frames are consecutive MCU frames (nothing else was sent in between).

### Parser @ **0x5814** (called on every main-loop iteration) [dis][emu]
- Moves ring bytes into the linear buffer 0x200006c4 (count 0x2000074e, ≤ 0x88 bytes).
- Scans for `55 AA`. **byte[2] must be 0x02**: anything else skips 2 bytes and the frame is
  dropped (confirmed [emu] with ver = 0x03). Total frame = len+9 must be < 0x8A, so the
  payload is ≤ 128 bytes. On checksum mismatch it skips 3 bytes and rescans. A partial frame is
  kept until the rest arrives.
- A valid frame goes to the dispatcher `0x1030(offset)`.

---

## 2. Receive dispatcher @ 0x1030 (jump table at 0x1090, cmd ≤ 0x28)

| cmd (module→MCU) | Handler | What the MCU does / sends back |
|---|---|---|
| 0x00 | 0x1134 | nothing |
| **0x01** product info query | 0x2fa8 | replies 0x01 with `{"p":"7gclukjs","v":"1.0.0"}` (see §7) |
| **0x02** network status | 0x5a84(data[0]) | **acks with `55 AA 02 00 ss 02 00 00 cs`** (cmd 0x02, len 0), then: see §5 |
| 0x03 (module's reply to MCU's 0x03) | 0x2c9c | nothing (switch with empty cases) |
| **0x04** DP write | loop at 0x1162 → 0x577c → 0x15f8 | handles every DP in the frame (step = 4+len). Per-DP 0x06 echo, see §6 |
| 0x05, 0x06 | 0x11ce / 0x11d0 | nothing. **The MCU does not process module ACKs** |
| 0x07 | default | nothing |
| **0x08** DP query | 0x11d6 | **nothing. The DP query is ignored** [dis][emu] |
| 0x09, 0x0A | 0x11d2 / 0x11d4 | nothing |
| **0x0B** MCU version query | 0x3698 | replies `55 AA 02 00 ss 0B 00 01 40 cs` (version byte **0x40** = 1.0.0) |
| 0x0C OTA notify | 0x3574 | MCU-OTA start (§8). Replies 0x0C 00 (accept) or 0x0C 01 (reject) |
| 0x0D OTA block data | 0x29f4 | stores the block, requests the next one (0x0D) or sends the result (0x0E) |
| 0x0E OTA result ack | 0x2bcc | if data[0] == 0, sets RTC_BKP0 = 0x11AA33DD (§8) |
| 0x0F–0x23 | default | nothing |
| 0x24 (time) | 0x11fe | nothing |
| 0x25 | 0x11fc | nothing |
| 0x26, 0x27 | default | nothing |
| **0x28** | 0x1200 → 0xdec → 0x34e0 | **sets every report flag (0xFFFFFFFF)**, so a full DP dump follows (§3). No direct reply [emu] |
| > 0x28 | default | nothing |

Commands the MCU **sends** (every caller of 0x5a08):
- 0x01 (product info reply)
- 0x02 (len 0: ack, and also a periodic poll, §5)
- 0x03 (`00` at boot, `01` = pairing request, §5)
- **0x06 (all DP reports)**
- 0x0B (version reply)
- 0x0C / 0x0D / 0x0E (MCU-OTA)

The MCU **never sends 0x05**, **never sends 0x24** (time) and never queries anything else on
the module side.

---

## 3. DP reporting

### Report primitives (all use **cmd 0x06**) [dis][emu]
| fn | Payload |
|---|---|
| 0x2740 bool(dpid, v) | `dpid 01 00 01 v` |
| 0x27d4 enum(dpid, v) | `dpid 04 00 01 v` |
| 0x284a value(dpid, u32) | `dpid 02 00 04 BE32` |

One DP per frame, always.

### DP table @ 0x5b64 (dpid, type), used to type-check writes
`(FF,bool) (FF,bool) (01,enum) (02,val) (03,val) (04,val) (09,val) (67,val) (69,val) (66,val) (68,enum)`.
The two 0xFF bool entries are unused "switch" placeholders from the template, but the firmware
still reports them (see below).

### Report scheduler @ 0x127c, 25-tick slot [dis][emu]
Every **25 ticks ≈ 50 ms** the scheduler checks the event-flag word 0x200000f0 in this priority
order, **sends at most one frame**, and clears that bit. Set bit = 0x3540, clear = 0x34f8, test = 0x3528.

| bit | DP | type | value source / scaling |
|---|---|---|---|
| 0 | **0xFF** | bool | 0x4938(0) (template switch 0, junk DP) |
| 1 | **0xFF** | bool | 0x4938(1) (template switch 1, junk DP) |
| 8 | **1** state | enum | radar state byte [0x2000036e]: raw 0 → **0** (none), raw 2 → **1** (presence), raw 1 or 3 → **2** (move) |
| 15 | **104** presence | **enum** (type 4), not bool | raw state == 0 → 0, otherwise 1 |
| 12 | **9** distance | value | **`(raw_cm + 5)/10 − 3`**, unsigned, i.e. decimetres rounded with a −30 cm offset. raw_cm = u16 0x200000fe. **No target → raw 0 → 0xFFFFFFFD (−3)**. Any raw < 25 cm also wraps to a negative value [emu] |
| 7 | **103** illuminance | value | u16 [0x20000384], 0…2000 (see below) |
| 9 | **2** move sensitivity | value | byte [0x2000036f] (0–10) |
| 14 | **102** presence sensitivity | value | byte [0x20000370] (0–10) |
| 16 | **3** min range | value | min_gate × 75 (cm) (0x3b08(1)) |
| 10 | **4** max range | value | max_gate × 75 (cm) (0x3b08(0)) |
| 13 | **105** presence timeout | value | u16 [0x2000037e] (radar readback; seconds [inf]) |

### What sets the flags (how often each DP goes out)
- **DP 9 distance** (0x3ce4, every 500 ticks ≈ **1 s**) [dis]:
  - state == 0: if the previous distance ≠ 0, report "0" once (which goes out as −3, see above).
  - otherwise, compare the 10-sample-filtered raw distance [0x2000037a] with the previous one.
    If it **decreased or increased, report**. If unchanged, report once (direction state 2),
    then stay silent.
  - So DP9 is sent **at most once per second, whenever the raw cm value changes**. A 1-cm
    jitter that rounds to the same decimetre still produces a frame, which explains the long
    runs of identical "12" values.
- **DP 1 + DP 104** (0x395e/0x3964): on a debounced state change (radar frame handler 0x36cc;
  the hold count depends on the transition, 0x386c–0x3926). Not sent while an OTA is in progress.
- **DP 103 illuminance** (0x234c, ADC sampled every ~126 ms, lux = (4095−adc)·660/1024, IIR
  ¾old + ¼new, clamp 2000):
  - reports when |Δ vs last reported| > 10, or (Δ ≥ 3 and lux < 50), or (Δ ≥ 6 and lux ≤ 199).
  - rate-limited to one every **≥ 501 ms** (0x2442).
- **Full dump = every flag set at once (0x34e0, flags = 0xFFFFFFFF)**. That is **11 frames at
  50 ms spacing**: DP FF, FF, 1, 104, 9, 103, 2, 102, 3, 4, 105 [emu]. It is triggered by:
  1. **network status going to 1**: in the main loop (0x258c), `status == 1 && prev != 1` sets
     [0x20000391] = 1, and the main loop then calls 0x2dfc → 0x34e0;
  2. **cmd 0x28** from the module;
  3. **periodic re-reports**, only once [0x20000390] = 1, which happens after the radar answers
     a config read (0x375e). The flag is cleared again by status 0. While it is set:
     - after every state change or illuminance report, a counter is set to 6 and a timestamp
       is taken (0x2468, 0x3974). Then **one dump every 10 s (0x2711 ms), 6 times**. Each new
       event re-arms the counter and the timer.
     - otherwise, **one dump every 10 min** (0x927C1 ms).

     With people moving around, that alone is ~66 frames/min of unchanged settings.
- **DP 0xFF**: a short key press (100 ms – 2 s) on PA1/PB1 toggles a template output and sets
  bit 0 or 1 (0x497c → 0x4a3c) [dis]. PA13/PA14 are the SWD pins, so the output probably does
  nothing on this board [inf].
- Writes (§6) send their echo directly, not through the flags.

### Andrik45719's patch (12 bytes) confirms these locations
- 0x1412: `bl 0x284a` replaced with NOPs, so the DP9 slot still runs but sends nothing.
- 0x2442: lux minimum interval 501 ms → 10,000 ms.
- 0x2718: burst interval 10,001 → 60,001 ms.
- 0x2724: radar watchdog 60,000 → 90,000 ms.

---

## 4. ACK / retry

- **There is no ACK wait and no retransmit for DP reports** [dis]. The report functions call
  the writer and return 1. Nothing records an outstanding seq. Incoming 0x05/0x06 are ignored.
  Each flag is cleared right after its single send.
- A missing ACK therefore cannot cause duplicates on the MCU side.
- The z2m seqs in a z2m debug log from the test unit were contiguous (0x90, 0x91, …), so the
  "5 within one second" at 07:48:27 are five consecutive MCU frames. The firmware cannot
  produce DP9 faster than about 1/s. Further on in the same log the cadence is ~1/s with gaps
  (:40, :41, :43, :44, :45, :46, :48 …), as the code predicts.
- **[inf]** The 07:48:27 cluster is most likely frames that were sent 1 s apart and delivered
  in a bunch (module/network queueing right after the OTA probe and re-pair), not MCU
  retransmits. For the replacement module: dedupe on DP value, not on seq.
- The only retry logic is in MCU-OTA. The block request (0x0D) is resent every ≥ 2001 ms if
  the offset has not advanced; after 6 attempts the OTA is aborted (0x26c6–0x26ec → 0x2e9c,
  which sends 0x0E 01).

---

## 5. Network status, start-up, pairing

- **No start-up gating**:
  - DP reports are **not** gated on network status. The 25-tick scheduler never looks at
    0x200006c3.
  - The MCU does **not** wait for 0x01. It answers 0x01 whenever asked.
- **At boot**, main init (0x254c → 0x2c78) sends **`55 AA 02 00 01 03 00 01 00 cs` (cmd 0x03, data 00)** [emu].
- **Status poll**: every 500 ticks (~1 s), while the stored status (0x200006c3) ≠ 1, the MCU
  sends **cmd 0x02, len 0** (0x12cc → 0x2900). The module should answer with
  `55 AA 02 … 02 00 01 <status>`.
- **On a 0x02 frame from the module** (0x5a84):
  1. ack (0x02, len 0);
  2. the first one ever sets 0x2000013c = 1;
  3. then by status:
     - **0** → clears the periodic-report enable [0x20000390], and if the previous status was 1, **start pairing**;
     - **1 or 2** → stop the pairing LED (0x151c);
     - **3+** → nothing;
  4. store the status.

  Status ≥ 4 is also used for the LED (0x2274).
- **Automatic pairing request**: every 10 s (counter 0x200000dc), if 0x2000013c == 1 it is set
  to 2 (one-shot), and if status ≠ 1 → **start pairing**. So ≤ 10 s after the first status
  frame, a non-joined status triggers one request.
- **Start pairing** (0x1508): set the pairing flag 0x200000cc, start the LED blink, and send
  **cmd 0x03 data 01** (`55 AA 02 00 ss 03 00 01 01 cs`). The pairing flag/LED times out after
  180 s (0x12a4).
- **Keys** (0x2080, sampled every 10 ms on PA1 and PB1, "pressed" = level 1):
  - at 6 s held, the LED blinks;
  - on release after ≥ 6 s, **cmd 0x03 01**;
  - on release after 0.1–2 s, the DP-0xFF toggle above.
- **[inf]** 0x03 `00` (boot) looks like an "MCU up" notice and 0x03 `01` like "reset/enter
  pairing". The MCU ignores whatever the module answers to 0x03.
- **Joining**: status 1 after non-1 gives a full DP dump (§3). Sending 0 after 1 makes the MCU
  send 0x03 01.
- **Watchdog resets**: the MCU resets itself (0x2680 → 0x2e2e, NVIC_SystemReset) after a 2 s
  delay when:
  1. no radar frame for > 60 s (0x2652; [0x20000398] is set at 0x3806);
  2. RTC_BKP0 ≠ 0 (§8);
  3. uptime ≥ 72 h (0x0F731401 ms) while state == none.

  After any reset you see the boot 0x03 00 frame again, followed by 0x02 polls.

---

## 6. DP writes (cmd 0x04)

Path: 0x577c looks up the DP's index (0x1b04) in the table and **requires the frame's type
byte to match** (else returns silently, **no reply**). Then it calls 0x15f8, which handles only
**DP 2, 3, 4, 102, 105**. Writes to DP 1, 9, 103, 104 (and 0xFF) are ignored with no reply [emu].
The value is always decoded as a 4-byte big-endian (0xec0), whatever the length field says.

| DP | Handler | Validation | Effect | Echo (cmd 0x06, sent immediately) |
|---|---|---|---|---|
| 2 move sens | 0x168c → 0x3bf0(v&0xFF, 0) | low byte < 11; otherwise **silently ignored** | radar cmd 4 queued (0x20000014) | **raw value echoed even when rejected** (e.g. 15) |
| 102 presence sens | 0x1744 → 0x3bf0(v&0xFF, 1) | same | radar cmd 5 (0x2000001c) | raw value echoed |
| 3 min range (cm) | 0x16de → 0x3b38(v, 1) | v ≤ 1000; gate = min(v/75, 12); requires gate+1 ≤ max_gate | radar cmd 2 (0x20000010) | accepted: raw v (later dumps report gate×75). Rejected: **current min×75** |
| 4 max range (cm) | 0x1592 → 0x3b38(v, 0) | v ≤ 1000; gate = min(v/75, 12); requires gate ≥ min_gate+1 | radar cmd 1 (0x2000000c) | accepted: raw v. Rejected: **current max×75** |
| 105 timeout | 0x1544 → 0x4150(v) | none | radar cmd 3 (0x20000018) | raw v |

Examples [emu], with max gate 8 and min gate 1:
- DP4 = 450 → echo 450 (later reported as 6×75 = 450).
- DP4 = 50 → echo 600.
- DP3 = 2000 → echo 75.
- A frame with two DPs gets two separate 0x06 echoes.

Radar commands go out from 0x3ce4 at the next 200 ms slot.

---

## 7. Product info, version

- 0x01 reply [emu]: `55 AA 02 00 ss 01 00 1C` + **`{"p":"7gclukjs","v":"1.0.0"}`** (28 bytes) + cs.
  The string is assembled at 0x2fa8 from the pieces at 0x5b7a / 0x5b8f / 0x5b81 / 0x5b89 / 0x5b98.
  There is no `"m"` or other field.
- 0x0B reply: **0x40**, computed by 0x1a50 from "1.0.0" as `maj<<6 | (min&3)<<4 | (patch&0xF)`.
- Baud and clock: see §0 (115200 8N1, 24 MHz).

---

## 8. MCU-OTA: can it install an image? **Verdict: does not install** (high confidence, static + partial emulation)

Notify (0x0C) handler @ 0x3574 [dis]:
- seeds the expected PID from the product string: **`7gclukjs`** (0xff8 copies 0x5b8f → 0x200000cd);
- memcmp of the PID against rx[8..15] (0x3616);
- requires **rx version > 0x40** (0x362c, signed compare);
- requires **size ≠ 0** (0x3636). The notify itself has no upper bound on the size. The size
  limit is enforced on write: offset+len < 0x6001 (0x2f22) and (offset+len)>>13 < 3 (0x1862),
  so **at most 0x6000 = 24 KiB**. The current code is 0x5BF0 bytes, so a full image would fit;
- on accept: 0x2e68 enables PMU BKPWEN (0x2f94), runs 0x197c (FMC unlock [inf]) and writes
  **FMC keys 0x45670123 / 0xCDEF89AB into RTC_BKP1/BKP2** (0x40002854 / 0x40002858). It then
  replies 0x0C 00 and requests block 0 (0x0D).

Block (0x0D) handler @ 0x29f4 → 0x2ec8:
- offset 0 must start with an SP in 0x2000xxxx (mask 0x2FFE0000), else abort with 0x0E 01;
- then the staging area **0x0800A000–0x0800FFFF is erased** (0x1798: loop 0x17a2–0x17ae);
- 48-byte blocks are programmed at **0x0800A000 + offset** (0x18a8);
- at the end, a byte-sum check → **0x0E 00** (success) or 0x0E 01 + abort (0x2e9c clears BKP1/2).

When the module answers the MCU's 0x0E with data `00` (handler 0x2bcc → **0x2eb8**), the MCU
sets **RTC_BKP0 (0x40002850) = 0x11AA33DD**. This is the only write to BKP0.

Main loop @ 0x2664: `if (BKP0 != 0 || radar silent > 60 s) { disable some clocks; delay 2000 ms; NVIC_SystemReset }` (0x844 writes AIRCR 0x05FA0004).

Attempts to refute the claim that nothing installs the image:

| Check | Result |
|---|---|
| Literals 0x400028xx | only 0x40002850 (read @0x2664, written @0x2eb8), 0x40002854/58 (written @0x2e5a/0x2e60/0x2e78/0x2e7e). No 0x40002800 base literal exists, so no base+offset access. Thumb-1 `ldr/str` immediates (≤ 124) cannot reach +0x50 from any other peripheral base used: 0x40002000 is TIMER13 (PWM, @0x4860/0x4896/0x4b88/0x522a/0x536c), 0x40007000 is PMU. Nothing besides the 0x2664 check ever *reads* BKP0/1/2 |
| Literals 0x08009xxx–0x0800FFFF | only 0x0800A000 (erase start @0x17a2, program base @0x18a8) and 0x0800FFFF (erase end @0x17aa). The checksum reads staging data. Nothing branches to, or loads a vector table from, 0x0800A000 |
| VTOR (0xE000ED08) | a single writer, 0x2e34, called only from SystemInit @0x30e with (0x08000000, 0). No runtime relocation |
| Reset path | vector reset = 0x080000E1 → 0xe0: `blx SystemInit(0x291)` (RCU only, plus VTOR = 0x08000000) → `bx __main(0xcd)` → scatterload 0x144 (table 0x5b9c: copy 0x30 bytes to 0x20000000, zero 0xBB0 bytes from 0x20000030) → main 0x24d4. No flag check, no BKP access |
| Code in RAM | none. Scatterload copies only .data (0x30 bytes) and zeroes .bss. All indirect calls (`blx` @0x154, 0x596, 0x5e2; `mov pc` jump tables) target flash: soft-timer callbacks 0x08000DED / 0x08002319 and switch tables |
| Hidden bootloader | one vector table at 0x0. 0x5BF0–0xFFFF is erased (0xFF), the same as in the §6 analysis of the other dumps |
| GD32E230 ROM / option bytes | the system-memory bootloader runs only when BOOT0 is strapped high, and it is a UART/ISP loader. It does not interpret RTC backup registers or install images from user flash [inf: standard GD32 behaviour, no vendor doc in repo states otherwise]. No option-byte write (0x1FFFF800) literal appears in the code |

**Result**: the image lands in 0x0800A000 and BKP0 is set. The MCU then resets after ~2 s, boots
the old firmware from 0x08000000, sees BKP0 ≠ 0 again and resets again: a **reboot loop every
~2–3 s until the backup domain loses power** (power cycle). Nothing ever clears BKP0. The update
is **never installed**. Probably not a brick: the old firmware stays intact, and only staging
is erased and written.

One nuance compared with the e5m9c5hl notes: in this image BKP0 is not set by the block handler
on checksum success. It is set when the **module acknowledges the MCU's 0x0E result with `00`**
(cmd 0x0E handler 0x2bcc). A module that never sends 0x0E 00 back never triggers the reboot loop.

---

## 9. Practical implications for the replacement Zigbee-module firmware

1. Talk 115200 8N1, version byte **0x02** in every frame (frames with any other version are
   dropped). Checksum as usual. Use any seq.
2. Answer the MCU's 0x02 polls with `02 <status>` and send **0x02 01** once joined. A 0→1
   transition gives a full dump (11 frames). **Do not send 0x02 00 after 0x02 01** unless you
   want the MCU to send 0x03 01.
3. Expect:
   - 0x03 00 at every MCU boot;
   - 0x03 01 as a pairing request (long key press, or not joined ~10 s after the first status
     frame, or status 1→0);
   - optionally reply 0x03 to them, since the MCU ignores the reply.
4. Do not rely on 0x08 (DP query) to fetch the state: it is ignored. To get every DP, send
   **cmd 0x28** (or toggle status 0→1, which has side effects) and collect the 11-frame dump.
5. All reports arrive as **0x06** and never need an ACK. Filter in the module:
   - **DP 0xFF** (junk) — drop it;
   - periodic dumps of unchanged settings;
   - DP9 repeats (dedupe or deadband on value);
   - **DP9 = 0xFFFFFFFD (−3)** means "no target" (also any raw < 25 cm).
6. DP 104 is **enum** (type 4) 0/1. Ranges are in cm with 75 cm granularity (DP 3/4). Note
   that after a write, the echoed value can differ from what was applied, and a rejected write
   echoes the old value (DP 3/4) or the rejected value (DP 2/102).
7. Never send 0x0C / 0x0D / 0x0E (especially not 0x0E 00): MCU-OTA cannot install and can cause
   the reboot loop.
