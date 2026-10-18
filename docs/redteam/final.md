> Archived red-team report, written against the tree at the time. Everything it found has been
> addressed; see [verification.md](../verification.md). Paths may have moved since (e.g. `research/`
> → `docs/`, `fw/` → `.deps/dumps/`, tests under `firmware/tests/`).

# Red team, final round (2026-10-18): "won't brick, and stays recoverable over the air"

**Verdict: no BLOCKER found.**
- The brick paths I could build all depend on conditions that the evidence below rules out, or that would need a hardware fault.
- **One HIGH must be fixed before flashing (H1).** It is a procedure and documentation bug. If it isn't fixed, the device converts fine but z2m keeps treating it as the stock TS0601. The v1.0.02 / recovery OTA is then never offered until the device is re-interviewed.
- Everything below is either verified by me, with file:line or a test I ran, or marked as inference.

New scratch tests (nothing under `firmware/src` was touched):
- `firmware/tests/final_rom/final_handoff_rom.c`: the handoff power-cut simulation re-run with the real ROM rules, plus bit-level partial programming and watchdog resets.
- `firmware/tests/final_nv/final_nv_garbage.c`: the SDK's real `drv_nv.c` run against the converted flash, plus forged NV headers.

---

## HIGH

### H1. z2m will not re-interview the converted device, so the stage-2 / recovery OTA is not offered

**Evidence**
- The handoff never touches the MAC sector 0xFF000.
  - The stock dump has the MAC at 0xFF000: `fa3ceb38c1a4123a`, i.e. a4:c1:38:… (Tuya OUI).
  - The SDK reads the IEEE from `CFG_MAC_ADDRESS` = `g_u32MacFlashAddr` = 0xFF000 (`tl_zigbee_sdk/proj/drivers/drv_nv.h:110,131`).
  - So the module rejoins with **the same IEEE**, not "possibly the same".
- herdsman 10.10.0, as shipped in the test setup's z2m 2.14.2 container (`node_modules/zigbee-herdsman/dist/controller/controller.js:629-665`): on `deviceJoined` for a known IEEE, it interviews only if `interviewState` is Pending or Failed. Otherwise it logs `Not interviewing …`.
- The DB entry keeps `modelID = TS0601` and `_TZE204_7gclukjs`.
- herdsman OTA matching requires `modelId` to equal the stored Basic modelID. The device now asks with manuf 0x6565, type 0x5244.
  - Index entry 2 (`modelId: ZY-M100-ZRD_1`) does not match.
  - Entry 1 needs manuf 4417, so it does not match either.
  - Result: "no update available".
- z2m also keeps applying the stock tuya definition, so the new occupancy/illuminance clusters are not decoded.
- romasku's conversion guide has an explicit **"Interview the device"** step after permit-join (`upstream/tuya-zigbee-switch/docs/updating.md`, Conversion step 12). Our `INSTALL.md` §2 says the device "rejoins as model `ZY-M100-24GV2-ZRD`". That is false for z2m 2.14.2.

**Failure scenario**
1. The user follows INSTALL.md and the conversion succeeds.
2. The device rejoins and still shows as the stock radar, with no data.
3. Step 3 "Check for update" reports nothing.
4. The user concludes the OTA chain is broken, or removes the stock-ota converter. Worse, they don't have a working OTA path when one is needed (safe mode, bad image).

**Fix (cheap)**
- In INSTALL.md §2, after permit join: z2m → device → **Interview** (z2m 2.x frontend button / `bridge/request/device/interview`), then **Reconfigure**.
- Alternatively, remove the device from z2m before converting, or before permitting join.
- Then check that the device shows `ZY-M100-24GV2-ZRD` before step 3.
- Fix the PLAN R6 / INSTALL wording to say "same IEEE".

**Clears when:** INSTALL.md has the interview step.

---

## MEDIUM

### M1. The Tuya bootloader leaves a 3 s watchdog running, and the handoff never feeds it

**Verified by disassembling the stock Tuya bootloader** (0x0–0x6A0C in `tuya_thermostat_orig_aoclfnxz.bin`, identical in all 16 dumps):
- 0x6650–0x6670: clears and then sets `reg_tmr_ctrl` (0x800620) bit 23 = `FLD_TMR_WD_EN` (`platform/chip_8258/register.h:741`), with bit 6 for timer 2. The capture value comes from `wd_set_interval_ms(0xBB8 = 3000)`, inlined at 0x6880. **Watchdog = 3 s.**
- 0x67A6–0x67EA (the boot path): copies exactly **0x5000 bytes** from 0x8000 into SRAM 0x840000. It checks only **the byte 0x4B at 0x8008** (`cmp r3,#0x4B` at 0x67C8). Then it writes 0x88 to 0x800602 (CPU restart from SRAM). The watchdog is not stopped first.
- The Tuya copy loops feed the watchdog: `strb 0x08 → 0x800623` at 0x66E8 and 0x6742. Our `move_flash_data` does not (`src/ota_scheme/ensure_ota_scheme.c:144-167`). Neither does romasku's.

**Estimate (inference)**
- The handoff copies 45 sectors / 712 pages of 182 KB, plus verify reads.
- At typical datasheet times (sector erase 40–60 ms, page program 0.7–2 ms, plus a 100 µs `sleep_us` per op), that is about 2.5–4 s. That is about the same as the 3 s watchdog.
- Expect zero to a few watchdog resets during the first copy. `INSTALL.md` says "a second or two".

**Why it is not a brick**
- Each reset goes ROM → Tuya bootloader → our RAM code again.
- `sector_matches()` skips finished sectors, so progress is monotone. Each boot gets about 2 s of useful work against about 0.5 s worst case per sector.
- I re-ran the full simulation with "watchdog reset at every flash op, flash finishes the op" (`final_rom`): **759/759 pass, ≤ 3 boots.**
- Field evidence: romasku's converted images are the same 4417/54179 Tuya bootloader, with up to 195,266 B of OTA file versus our 182,242 B. 179 of 240 `*from_tuya.zigbee` files in `upstream/tuya-zigbee-switch/bin` have the same 0x1A00 RAM-code size as ours.

**Residual (speculation)**
- The ROM's behaviour when a watchdog reset hits while the flash chip is still busy with a sector erase is undocumented. If its slot probe reads 0xFF everywhere, the device might sit in the ROM until power-cycled. That is not a brick: a power cycle fixes it.
- The Tuya bootloader also erases and rewrites its 12-byte record at **0xF7000** on every boot while in state 4 (0x6614, 0x67EC). So every watchdog reboot costs one extra erase of that sector. A cut there leaves an invalid record, which falls back to "boot 0x8000" (0x6680–0x6694). Benign.

**Options**
- (a) Leave the code as is: it is field-identical to romasku. Fix INSTALL.md to say "can take several seconds and a few reboots".
- (b) Feed the watchdog in `move_flash_data` with a raw RAM-safe store, `*(volatile u8*)0x800623 = 0x08`. This is a new, non-field-proven change, so it would need a re-run of check_elf and the simulation.

I recommend (a).

### M2. Safe mode: "power-cycle once to try normally again" is wrong

**Evidence:** `src/app_main.c:24-32`.
- `boot_cnt` is only reset by `bootCntClearCb` after 30 min of uptime.
- A power cycle *increments* it: 5→6, still ≥ 5. So the device stays in safe mode on every boot until it has run 30 min, and then needs one more reboot.

10 power cycles (the factory-reset gesture, `factory_reset.c`) always lands in safe mode as well (boot_cnt ≥ 10). The leave still works, because `factroyRst_handler` runs in safe mode (`app_main.c:114`).

**Scenario:** the user does the 10× reset to re-pair. The device rejoins with the radar disabled and `safe_mode=true`. Following INSTALL, they power-cycle, and it is still in safe mode. Zigbee and OTA keep working throughout, so it stays recoverable over the air. The cost is confusion and nothing worse.

**Fix:** correct INSTALL.md ("leave it powered 30 min, then power-cycle"). Optionally, clear the counter on a successful join instead of only after 30 min.

---

## LOW

### L1. The handoff size guard allows an overlapping, self-corrupting copy

**Evidence:** `MAX_FIRMWARE_SIZE 0x40000` (`ensure_ota_scheme.h`).
- The source is 0x8000..0x8000+size and the destination starts at 0x40000. They overlap once size > **0x38000**.
- The erase of 0x40000 would destroy source bytes before they are read. Then read == written == verify, so the corrupt copy is accepted and the markers are zeroed: a brick.
- The Tuya bootloader also copies only 0x5000 bytes of RAM code, so `_ramcode_size_` must stay ≤ 0x5000.

**Current values:** image 0x2C7A4, RAM code 0x1A00. **Not reachable today.**

**Fix:** have `tools/verify_image.py` enforce size ≤ 0x38000 and RAM code ≤ 0x5000 for the from-tuya image.

### L2. Steering stops on some BDB statuses

**Evidence:** `zb_appCb.c:218-227`. `NOT_PERMITTED`, `NO_SCAN_RESPONSE`, `FORMATION_FAILURE` and similar do not reschedule steering. `NO_NETWORK` / `TCLK_EX_FAILURE` / `TARGET_FAILURE` do (5–10 s).

**Scenario:** if the stack reports one of the non-retrying codes during the first join, the device stops trying until power-cycled. INSTALL already says "power-cycle once". Recoverable, not a brick.

### L3. The ROM checks only the byte 0x4B, so an SDK custom OTA cut mid-erase of sector 0 is about 1/256 likely to look bootable

- The ROM rule is from the Telink B85 handbook: "the values of 0x08 and 0x20008 are read and compared with 0x4b". Only 0x0, 0x20000 and 0x40000 are tried on B85; 0x80000 is B87 only.
- During custom→custom OTA into slot A, `ota.c:1478-1481` erases sector 0 first. A power cut during that 40–400 ms erase leaves random bits. If byte 8 happens to read 0x4B, the ROM boots garbage.
- This is generic to every Telink no-bootloader OTA device and not specific to this project. Probability is about 1e-4 × 1/256 per update.

### L4. Tooling: `tools/disasm.py` shadows the stdlib `dis`

`.venv/bin/python tools/check_frames_gd32.py …` crashes inside unicorn (`import inspect` → `import dis` → `tools/disasm.py`, IndexError). It works with `python -P`. Rename the file or document `-P`.

---

## NOTE (verified facts and resolved questions)

### N1. ROM boot rules, and a re-run of the power-cut simulation with them

- **Sources:** Telink "TC BLE Single Connection SDK" handbook (B85m section "Flash multiple address booting"): https://doc.telink-semi.cn/doc/en/software/res/sdk/ble/tc_ble_en/tc_ble_single_connection_en/
  - "B85 supports … 0x20000 and 0x40000; B87 … and 0x80000"
  - "read flash 0x08 … if the value is 0x4b …"
- **Tuya bootloader:** checks byte 0x4B at 0x8008 (disassembly 0x67C6-0x67CA).
- **The existing simulation assumed full-word checks** and an optional 0x80000 slot. `final_rom` re-runs it with:
  - ROM and Tuya bootloader checking the single byte;
  - slots {0, 0x20000, 0x40000};
  - three interruption models: prefix programming (as before), **bit-level partial programming** (any subset of bits across the whole write, which is how a NOR page program really fails), and watchdog resets.
  - Result:
    ```
    single cut (prefix)      759 runs, 0 failures, worst 3 boots
    single cut (bit-level)   759 runs, 0 failures, worst 3 boots
    single watchdog reset    759 runs, 0 failures, worst 3 boots
    every pair of cuts, 8K   666 runs, 0 failures, worst 4 boots
    small-OTA layout pairs   630 runs, 0 failures, worst 4 boots   PASS
    ```
- **Build:**
  ```
  gcc -O2 -DHOST_TEST -I../handoff -I../../src/ota_scheme -o final_handoff_rom final_handoff_rom.c ../../src/ota_scheme/ensure_ota_scheme.c
  ```
  Run it from `firmware/tests/final_rom` with `<stock dump> <bin>`.
- **0x80000 is moot.** It is not a B85 boot address, so the NV ZB_INFO sector at 0x80000 can never be booted. `verify_image.py`'s 0x80008 check is harmless but unnecessary.
- **Byte versus word mismatch, ROM vs our code.** The ROM checks a byte, while `is_valid_image_at_address`, `started_by_tuya_bootloader` and the SDK's `mcuBootAddrGet` (`ota.c:129-149`) check the full word. I traced every mixed state ("0x4B but not KNLT", and the reverse) through the code. In each one the device either re-runs the handoff idempotently or boots slot B. The bit-level simulation exercises these states. The SDK writes single bytes: 0x4B to the new slot and 0x00 to the old one (`ota.c:229-231`). A ROM-bootable slot is therefore always a full KNLT for `mcuBootAddrGet`, so `ota_init` will not post `BOOT_ADDR_ERROR` and disable OTA.

### N2. What runs in the Tuya-bootloader context (item 3)

**Verified against the built ELF and the bootloader disassembly.**
- **RAM code is loaded.** The Tuya bootloader copies a fixed 0x5000 bytes, and our RAM code is 0x1A00 bytes (`_ramcode_size_align_256_`). The CPU restarts at SRAM 0. cstartup sets the IC tag registers from our own constants and zeroes .bss correctly.
- **.data is garbage in this context.** It is copied from the absolute `_dstored_` = 0x2C574, which is physical, 0x8000 off.
- **The handoff touches no .data and no flash-resident constants.** I resolved every literal-pool word of the 16 functions reachable from `main` (`main`, `ensure_correct_ota_scheme`, `move_flash_data.clone.0`, `ram_code_flash_*`, `flash_mspi_*`, `flash_send_*`, `flash_wait_done`, `sleep_us`, `adc_*`). Only constants, MMIO and these RAM variables appear:
  - `g_flashIsZbit` @0x842D24 is in **.bss**, so it is 0 and the ADC/Zbit gate is skipped.
  - `g_flashWorkVol` (.data, garbage) and the `adc_*` RAM variables are only reached when `g_flashIsZbit != 0`.
- **No register-indirect branch** goes outside RAM code.
- **Interrupts are off.** CPU restart; nothing enables IRQs; `flash_mspi_*` restore the disabled state.
- **The `*(u32*)0x8008` read is correct in both contexts:**
  - Tuya context: no remap (0x63E = 0 for slot 0), so it reads the physical KNLT.
  - ROM slot B: it either reads the image offset 0x8008 (if data reads are remapped, which is undocumented) or physical 0x8008 (zeroed). Both are safe, and `verify_image.py` checks the former.
- **SYSTEM_RESET fall-through.** `main` at 0xD76–0xD80 writes 0x20 → 0x80006F, then branches to `flash_main` because SYSTEM_RESET is not `noreturn`. This relies on the reset taking effect within one instruction, exactly like romasku's main (which calls `drv_platform_init` after it).
- **Line-by-line differences from romasku** (`upstream/tuya-zigbee-switch/src/telink/main.c`, `ota_reformating/*`):
  1. `drv_platform_init` is called from a noinline flash function, not from RAM `main`. Only the normal path is affected.
  2. Already-matching sectors are skipped: the double-fault fix, proven by the simulation.
  3. A sector-wise loop with the same 256 B chunks, erase-then-write and verify-or-reset.
  4. `BIG_OTA_FLASH_ADDR` = SDK `FLASH_ADDR_OF_OTA_IMAGE` = 0x40000.
  5. The main loop feeds the watchdog around `ev_main` / `tl_zbTaskProcedure` (`app_task` runs inside `ev_main` via `ev_on_poll`).
  `ram_code_flash.c` is byte-identical apart from the header.

### N3. NV area after conversion (item 1)

- **No-bootloader 1 MB map** (`drv_nv.h:195,397-399`): module *i* at 0x80000 + 0x2000·i, two 4 KB sectors each. KEYPAIR is 2×16 KB at **0x8E000–0x96000**, so NV spans **0x80000–0x96000**. Also used:
  - 0xFC000: factory-reset flag; stock value 0xFF
  - 0xFD000: install code; stock 0xFF
  - 0xFE000: calibration; stock unchanged
  - 0xFF000: MAC
- **Stock Tuya data** (16 dumps): 0xD8000–0xF0000, 0xFB000, 0xFE000–0xFFFFF. Outside NV.
- **The stock download** at 0x70000–0x9C7A4 covers all of NV with our own image bytes. The Tuya bootloader never erases it; it only reads it in states 1–3 (disassembly 0x675E–0x6714).
- **Real header bytes after conversion** (printed by the harness): none is 0x5A5A, 0x7A7A or 0xFAFA. So `nv_sector_read` returns NOT_FOUND, and the first `nv_flashWriteNew` per module erases its sectors (`drv_nv.c` `nv_flashWriteNewHandler`, `nv_nwkFrameCountSaveToFlashHandler`).
- **`final_nv_garbage`** runs the SDK's real `drv_nv.c` on host against the converted flash for both images:
  - 300 boots of reads and writes on all 8 modules plus the frame counter;
  - 200 adversarial layouts with forged 0x5A5A/0x7A7A headers and random index entries;
  - Result: **0 lost items, garbage never read as valid, no write failures, no erases outside 0x80000–0x96000, no loops** (op budget), at most 8 erases per sector over 300 boots.
  - The only "failure" in a first draft was my harness stepping the frame counter by more than `UPDATE_FRAMECOUNT_THRES`. That is by design.
- **romasku** does nothing special for NV at first boot. Its only NV patch (`src/telink/patch_sdk/drv_nv.c`) keeps the APP module across a factory reset. Ours uses the stock `nv_resetToFactoryNew`, so a factory reset also wipes the radar settings and the boot/power counters. Harmless, and arguably desirable.

### N4. SDK 3.7.2.0 (item 4)

- The vendored `firmware/tl_zigbee_sdk` is identical to `upstream/sdk372/V3.7.2.0.zip`.
- `zigbee/lib/tc32/libzb_router.a` has sha256 `bc677ec6…f003b7`, equal to the file fetched from `raw.githubusercontent.com/telink-semi/telink_zigbee_sdk/V3.7.2.0/…`. romasku's `tools.mk:10-13` pins the same tag, so it is the same library.
- 3.7.1.2 → 3.7.2.0 diff ignoring whitespace and CRs:
  - formatting only for the 8258 paths of `drv_nv.c`, `ota.c`, `drv_flash.c`, `drv_hw.c`;
  - identical `cstartup_8258.S`, `boot_8258.link`, `drv_nv.h`, `chip_8258/flash.c`;
  - flash lock tables changed, but `FLASH_PROTECT_ENABLE` is undefined (= 0).

### N5. OTA delivery (item 7)

**Checked against the test setup's running z2m 2.14.2 container** (herdsman 10.10.0, zhc 26.115.1):
- `ota.zigbee_ota_override_index_location` is the correct 2.x key (`settings.schema.json:371`, `otaUpdate.js:80`). The live config is still `ota: {}`, so nothing is installed yet.
- The override index is merged ahead of Koenkk's; first match wins.
- Entry 1 needs:
  - imageType 54179 and manuf 4417;
  - modelId TS0601;
  - manufacturerName ∈ [`_TZE204_7gclukjs`];
  - current fileVersion ≤ 255. The test unit's module reported 74 (RESEARCH §9).
- So it cannot be offered to another TS0601, nor to the converted device (manuf 0x6565). sha512 is enforced.
- queryNextImageResponse takes its IDs and version from the file header, which matches the index (0x10013001 / 0x10023001).
- The stock OTA client accepts the format: slacky's field-proven from-tuya file also has outer 0x1141/0xD3A3 with embedded 0x6565. Its header string is blank; romasku's is "Telink OTA Image", like ours.
- The dist payloads are byte-identical to `firmware/bin/*.bin`.

### N6. First join (item 5)

- The device is factory-new (NV not found) and keeps its stock IEEE.
- The status policy (`app_radar.c:146-147, 293-303`) reports not-joined / joining until the first join. It then never reports 0 while it holds a network.
- The GD32's automatic `03 01` about 10 s after a non-1 status is ignored while not joined. While joined it is honoured only when idle, and only once.
- Steering retries every 5–10 s on NO_NETWORK (but see L2).

### N7. The flash really is 1 MB

The SDK hangs in `internalFlashSizeCheck` (`drv_hw.c:104-110`, `while(1)`, before the watchdog) if the flash JEDEC size is under 1 MB. The part is TLSR8258**F1**KET32 (1 MB), and every dump is 1 MB. Not a risk on this module.

### N8. Docs out of date

- PLAN.md still says "bootloader byte-identical to slacky's" and lists a 0x80000 ROM slot.
- RESEARCH §13 says "ROM checking 3 or 4 slots" and implies full-word checks.
- INSTALL says "possibly the same IEEE".

---

## Item 8: every test re-run by me

The build was done in a scratch copy, so the delivered `bin/` and `dist/` are untouched.

| What | Result |
|---|---|
| `firmware/build.sh` (v1.0.01 + v1.0.02, incl. `verify_image.py`, `check_elf.py`) | PASS. 15-function RAM-only handoff; report chain wired |
| `make -C firmware/tests` | 234 core + 1935 glue checks, 0 failed |
| `tests/handoff/test_handoff.c` (original) | PASS: 759/759 ×2, 666 pairs, 630 pairs |
| `final_rom/final_handoff_rom.c` (new: byte rules, bit-level cuts, watchdog) | PASS |
| `final_nv/final_nv_garbage.c` (new: real `drv_nv.c` on converted flash, both images) | PASS |
| `tools/check_frames_gd32.py` (needs `.venv/bin/python -P`, see L4) | PASS, 9 distinct frames all accepted |
| `redteam_fuzz.c` | 20,000 random sessions OK; libFuzzer 262,316 runs in 90 s, clean |

## What would change this verdict

- **A BLOCKER would need one of these:**
  - a stock Tuya bootloader on the test unit that differs from the 16 dumps. Check: Tuya bootloader sha256 e0eb8761…; only obtainable by SWS read.
  - a download address other than the record's source. That does not matter for the handoff, which reads only 0x8000.
  - a 512 KB flash.
  - None of these is indicated.
- **H1 is cleared** by adding the interview step to INSTALL.md.
