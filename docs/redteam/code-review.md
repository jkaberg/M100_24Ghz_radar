> Archived red-team report, written against the tree at the time. Everything it found has been
> addressed; see [verification.md](../verification.md). Paths may have moved since (e.g. `research/`
> → `docs/`, `fw/` → `.deps/dumps/`, tests under `firmware/tests/`).

# Red-team review: ZY-M100 ZRD firmware (TLSR8258 / ZT3L)

Date: 2026-10-18. Scope: `firmware/src` (new radar code, plus the slacky-derived glue), the parts of the
Telink SDK it relies on, and the GD32 behaviour in `research/gd32-7gclukjs-protocol.md`.

**The tree changed while this review ran.** At 06:31–06:34 UTC, `src/common/main.c`,
`src/common/comm_cfg.h` (`BOOT_LOADER_MODE` 1 → 0, new `ota_scheme/`) and `src/include/app_cfg.h`
(`VOLTAGE_DETECT_ENABLE` ON → OFF, ADC pin PB3 → PC5) were rewritten. Section 7 (flash map) covers
the slacky layout that was in place when the review started. The new OTA scheme needs its own
review. Everything else was checked against the 06:01–06:17 versions of the radar files, and they
did not change during the review.

Tools written (outside `src/`, nothing in `src/` modified):
- `firmware/tests/redteam_fuzz.c` runs the real `app_radar.c`, `tuya_mcu.c` and `radar.c` against
  stubs. It sends random bytes and checksum-valid frames with random cmd, length and payload into
  the UART RX path, and makes random ZCL writes to every radar attribute. It runs under
  ASan/UBSan, either with libFuzzer or as a plain random loop. Invariants it checks:
  - no 0x0C/0x0D/0x0E (MCU-OTA) frame is ever sent;
  - every frame sent is well-formed, with version 0x02 and a valid checksum;
  - status 0 is never sent while joined and not factory-new;
  - `zb_factoryReset()` runs only after a real `03 01` frame.

  Result: **331,094 libFuzzer runs over 150 s plus 3,000 long random sessions, with no crash, no
  sanitizer report and no broken invariant.** Build lines are in the file header.
- `firmware/tests/redteam_resync.c` demonstrates the parser re-sync loss described in finding M3.
- Disassembly of `app_radar.c`, `radar.c`, `tuya_mcu.c` and `app_uart.c` with the project's own
  tc32 flags (`-O2 -fpack-struct`), to check how packed fields are accessed.
- `tc32-elf-nm`/`objdump` on `out/zy_m100_zrd.elf`.

---

## BLOCKER

### B1. ZCL attribute reporting never runs: no occupancy, illuminance or state reports
`src/app_main.c:69-78` (`app_task`)

The project uses slacky's own `src/zcl/zcl_reporting.c`. In that file, nothing sends a report on
its own:
- `reportAttrTimerCb` only counts the min/max counters down.
- The reports are sent by `reportAttrs()`, and only `report_handler()` calls it.

Upstream calls `report_handler()` from its `app_task` (`upstream/.../app_main.c:149-151`,
`if(bdb_isIdle()) report_handler();`). Our `app_task` dropped that call, and nothing else calls it.
The built ELF confirms this: `report_handler` and `reportAttrs` are not in
`out/zy_m100_zrd.elf`. They were garbage-collected, and only `reportAttrTimerStart`/`Stop`/`Cb`
are left.

**Failure:**
- occupancy, presence/move state (0xF000), illuminance and the settings attributes are never
  reported, neither with the defaults from `bdb_defaultReportingCfg` nor with z2m's configuration;
- z2m sees the device as dead except for distance (sent directly by `report_distance`) and for
  explicit reads;
- the host tests can't catch this, because `test_glue.c` does not link `app_main.c`.

**Fix:** in `app_task`, add the same call upstream makes:

```c
if (bdb_isIdle()) {
    report_handler();
}
```

Add a check in the build, for example `nm out/*.elf | grep -q reportAttrs`, so that this can't
silently regress again.

---

## HIGH

None found beyond B1. The UART input path and the MCU protocol handling held up under fuzzing and
review.

---

## MEDIUM

### M1. A `03 01` from the MCU while the first join is still running makes the device leave mid-join
`src/app_radar.c:273-284`, together with `zb_appCb.c:198`

The device sends status `03` (JOINING) on a factory-new device. Per RE §5, the MCU then fires its
one-shot `03 01` 10–20 s after our first status frame, because status ≠ 1.

`on_frame` gates the leave only on `zb_isDeviceJoinedNwk()`. That check becomes true as soon as
NWK association succeeds, while BDB is still in `BDB_COMMISSION_STA_IN_PROGRESS` (transport key,
TCLK exchange, device announce).

**Failure:**
1. Steering starts at boot plus up to 4 s of jitter, and is retried every 5–10 s. So the
   association can land inside the MCU's 10–20 s window.
2. `leave_and_pairCb` then runs `zb_factoryReset()` 500 ms later and leaves in the middle of the
   join.
3. z2m sees join → leave → join, and the interview may fail. The second attempt usually succeeds,
   because the MCU's trigger is one-shot.

**Fix:** only treat `03 01` as "leave and pair" when all three hold:
- `!zb_isDeviceFactoryNew()`;
- BDB is idle (`bdb_isIdle()`, or `g_bdbAttrs.commissioningStatus != BDB_COMMISSION_STA_IN_PROGRESS`);
- no leave is already pending.

Ignore it otherwise.

### M2. A joined device can report status 0, which makes the MCU request pairing and the device leave
`src/app_radar.c:53, 133-138, 261, 352-366`, `src/zb_appCb.c:118-151`

The "a device with a network always says joined" override exists only inside
`app_radar_net_status()`. Two things bypass it:
- `send_status()` sends the raw `net_status`, and the PRODUCT_INFO handler, the 0x02 ping handler
  and the 5-minute alive check all use it.
- `net_status` starts as `TUYA_NET_NOT_JOINED`.

`zb_bdbInitCb()` calls `app_radar_net_status(TUYA_NET_JOINED)` only when
`status == BDB_INIT_STATUS_SUCCESS`. When a non-factory-new router fails to start
(`startDevCnf->status != SUCCESS`, `bdb.c:1273-1277`), the init callback arrives with `FAILURE`,
and `net_status` stays 0 for good.

**Failure:**
1. The first PRODUCT_INFO reply (about 1 s after boot) sends `02 00`.
2. The MCU's one-shot fires 10–20 s later and sends `03 01`.
3. `zb_isDeviceJoinedNwk()` can be true for a restored router, so the device calls
   `zb_factoryReset()` and leaves its network with no user action.

The same thing happens on any future path that sends a status before
`app_radar_net_status()` has run.

**Fix:** work out the status where it is sent:

```c
st = (net_status == TUYA_NET_JOINED || !zb_isDeviceFactoryNew()) ? TUYA_NET_JOINED : net_status;
```

Also call `app_radar_net_status()` from the init-failure branch. Together with M1's
`!zb_isDeviceFactoryNew()` guard, this closes the path.

### M3. Any time a slacky-derived leave confirm fires for a non-self address, the device marks itself factory-new and starts steering
`src/zb_appCb.c:292-320`

This is inherited from upstream, but the radar code now leans on it, because `zb_isDeviceFactoryNew()`
drives the status override. `app_leaveCnfHandler` does not check `pLeaveCnf->deviceAddr`, which is
an `extAddr_t` (`nwk.h:651-655`). On a router, an NLME-LEAVE.confirm for a child (removing a child
or end device) would:
- set factory-new = true while still on the network;
- start network steering, which on a joined router is a permit-join broadcast;
- turn off the "always joined" override until the next reboot. After that, BDB IN_PROGRESS events
  send status 3 to the MCU.

Whether the Telink router library ever raises a leave confirm for a child could not be checked:
the library is closed source.

**Fix:** act only when `ZB_IEEE_ADDR_CMP(pLeaveCnf->deviceAddr, g_zbInfo.macPib.extAddress)` holds,
or when the address is all zeros. Otherwise ignore the confirm.

---

## LOW

### L1. One corrupt byte in a length field loses up to about 255 bytes of following frames
`src/tuya_mcu.c:60-63, 74-80`

The parser has no inter-byte timeout. If it fails a checksum, it does not re-scan the bytes it
consumed. If noise turns a length byte into 0xF0, the parser waits for 240 payload bytes and eats
the frames that follow.

`redteam_resync.c` shows it: of 20 back-to-back frames, 1 corrupt, **17 good frames are lost**.
A missed presence or state change stays wrong until the MCU's next dump. That takes about 10 s if
[0x20000390] is set, otherwise until our 5-minute PRODUCT_INFO + 0x28.

**Fix:** reset the parser when the gap between bytes is more than about 20 ms (the GD32 sends each
frame back-to-back). Also limit `len` to 128, the GD32's own maximum (RE §1). Optionally, on a bad
checksum, re-feed from the byte after the 0x55.

### L2. `ZCL_CMD_WRITE_NO_RSP` and write-undivided are applied but never forwarded
`src/zcl_appCb.c:31`

The SDK stores the value and calls the hook for `WRITE_NO_RSP` too (`zcl.c:710-711`, `toAppFlg`).
The app only acts on `ZCL_CMD_WRITE`. A "write no response" from HA or another controller changes
the attribute, but the change never reaches the MCU or NV, so the attribute and the radar disagree.

**Fix:** handle `ZCL_CMD_WRITE_NO_RSP` (and `WRITE_UNDIVIDED`) the same way.

### L3. NV write on every distance-policy write, even when the value has not changed
`src/app_radar.c:92-100, 206`

Each write costs about 24 B in the APP NV sector, and roughly 170 writes force a sector erase. An
automation that rewrites the interval every few seconds would wear out the 2-sector APP module
(100k cycles) in months.

**Fix:** save only if the clamped values differ from what is stored.

### L4. Several `03 01` frames within 500 ms queue several `zb_factoryReset()` calls
`src/app_radar.c:280`

Each `03 01` received while joined schedules another `leave_and_pairCb`. The second call runs
while the leave is still in progress, or on a factory-new device. This is harmless in the fuzzer
stub; the SDK's behaviour for it is unspecified.

**Fix:** keep a static "leave pending" event pointer and skip the call if it is already set.

### L5. Settings writes before the MCU is identified are dropped silently
`src/app_radar.c:209-236`

While `radar.v == NULL`, `radar_build_setting` returns 0 and nothing is sent, but the ZCL attribute
keeps the written value until the MCU's next dump overwrites it. The same applies when the
product ID is unknown (another TuyaMCU variant): occupancy then never updates, with no error.

**Fix:**
- when `radar.v` is NULL, roll the attribute back, or return an error from the write callback;
- consider falling back to the `7gclukjs` map when the PID is unknown and the DPs look the same.

### L6. The 10× power-cycle reset counts every kind of reboot
`src/app_main.c:97`, `src/common/factory_reset.c:77-85`

This is new compared with upstream, which never called `factroyRst_init`. The counter goes up on
every boot, including watchdog resets, `appSysException` → `SYSTEM_RESET`, and OTA reboots. It is
cleared only after 2 s of uptime.

A crash or watchdog loop that hits within 2 s of boot, ten times in a row, turns into a factory
reset. The device then leaves the network and loses its bindings and reporting configuration.

A flaky supply can do the same. With `VOLTAGE_DETECT_ENABLE` now OFF, the brown-out → reset-loop
chain through `voltage_detect` is gone, but a supply that keeps dropping out every second still
counts.

What it erases: `zb_factoryReset()` sends a Leave and resets the ZB NV to factory-new (closed
library). The APP NV item (distance policy) is not cleared. Each boot also costs two NV writes.

**Fix:**
- count only power-on resets. Read the reset cause, or keep a RAM/retention flag so that
  watchdog/software resets do not increment;
- raise the window to about 5 s.

### L7. RX DMA length is not limited to the DMA's real capacity
`src/app_uart.c:15`

`uart_recbuff_init` programs `size = len >> 4 = 16`, which is 256 bytes including the 4-byte
length word, so 252 data bytes (checked in the disassembly of `uart_recbuff_init`). The callback
accepts `dma_len` up to 256. A 253–256 byte burst would copy stale bytes. This is memory-safe,
because `data[]` is 256 bytes.

**Fix:** reject `len > UART_DATA_LEN - 4`, or size the buffer as `UART_DATA_LEN + 4` rounded up
to 16.

### L8. The RX DMA buffer is only 4-byte aligned by chance
`src/app_uart.c:5`

With `-fpack-struct`, `uart_data_t` has alignment 1. The emitted `.align 2` comes from GCC's ARM
`DATA_ALIGNMENT` heuristic for structs at -O2. It would disappear with `-Os` or another compiler
change, and the 8258 DMA needs a 4-byte aligned buffer.

**Fix:** add `__attribute__((aligned(4)))` to `rx_dma`.

---

## NOTE (checked, no defect, or by design)

- **Watchdog.**
  - `drv_wd_setInterval(600)` comes out at 600·1000·48 >> 18 = 109, i.e. about 595 ms. The new
    `main.c` uses 1000, about 1 s.
  - It is cleared twice per main-loop pass. `drv_flash.c` clears it on every sector erase or
    write, so the OTA erase loop (`ota.c:1482`) and NV sector swaps are safe.
  - `app_radar_task` is bounded: 256 bytes per pass, no loops on input.
  - `drv_uart_tx_start` busy-waits only when the previous DMA TX has not completed. `txq_pump`
    keeps 20 ms between frames, and our longest frame is 17 B, which takes 1.5 ms at 115200 and
    17.7 ms at 9600, so it never waits.
  - The `else if` in `irq_handler` (`b85m/irq_handler.c:119-127`) leaves a pending TX status set
    and serves it on the next IRQ, so no TX-done event is lost.
- **ISR safety.**
  - `app_uartRecvCb` runs in IRQ context. The ring is single-producer/single-consumer with
    volatile 16-bit indices, which tc32 writes atomically, and the producer reads the consumer's
    index. That is correct.
  - Calling `drv_uart_init` again at runtime is safe in practice. It resets the UART and DMA
    registers and frees a pending TX ev_buf. If a TX were in flight this would be a use-after-free
    of the DMA source, but probing re-inits only ≥1 s after the last probe, and all queued frames
    finish within the 20 ms gap. No leak.
  - The 9600 probe never matters for 7gclukjs (115200 only, RE §0). It only runs after 3 missed
    5-minute alive checks. Its one useful side effect is that it is the only path that
    re-initialises a stuck UART. Keep a periodic re-init at 115200 even if 9600 is dropped.
    Consider a shorter alive interval: a stuck RX now means up to about 15 minutes of stale
    occupancy.
- **ev_buf.**
  - The UART driver holds at most one ev_buf. It is freed in the TX-done IRQ, or by re-init.
  - The DP ACKs (`app_radar.c:291`) are pointless, because the GD32 ignores 0x05/0x06 from the
    module (RE §2/§4). They take TX-queue slots and ev_bufs (12 per dump). They are harmless, but
    could be dropped. The TX queue silently drops frames when full (`app_radar.c:113`), and a
    flood of ACKs is the only thing that can fill it.
- **Reporting engine.**
  - `changeLux` is a `uint16_t`. `zcl_reportCfgInfoEntryUpdate` copies `zcl_getDataTypeLen(UINT16)`
    = 2 bytes, and `BUILD_U16` reads them little-endian, which is correct. `change0` (u8) is
    ignored for BITMAP8/ENUM8, which are non-analog.
  - `ZCL_REPORTING_TABLE_NUM` is 32 (`stack_cfg.h:69`), plenty for 3 defaults plus about 10 z2m
    entries. The table is 1,185 B and is re-saved to NV on every Configure Reporting, about
    3 saves per 4 KB sector. That is fine.
  - **Distance 0xF001 cannot be made reportable**: it has no `ACCESS_CONTROL_REPORTABLE`, so
    `zcl_configureReporting` returns `UNSUPPORTED_ATTRIBUTE` (`zcl.c:1795`).
  - z2m can still ask for high rates in two ways, both deliberate user choices:
    - write `0xF002` (interval) = 1 with deadband 0, which gives at most 1 frame/s, capped by the
      GD32's own 1 Hz DP9;
    - configure a short max interval on 0xF004–F008.

    Consider a floor of a few seconds on 0xF002.
- **Packed structs.**
  - `-fpack-struct` is on. In `g_zcl_radarAttrs`, `distance_cm` is at offset 3 and the other u16
    fields are at odd offsets. The tc32 compiler emits byte loads and stores (`tloadrb`/`tstorerb`)
    for every direct access, as seen in the `apply()` and `app_radar_attr_written()` disassembly.
    The SDK accesses attributes through memcpy or byte pointers.
  - `set_u16(&r->illuminance_lux, …)` passes pointers to packed members. Those happen to sit at
    even offsets (8/12/14/16) in a 4-byte-aligned `radar`, so they are safe, but fragile. The
    other `uint16_t*` taken is `&off`, on the stack. No defect.
- **NV.**
  - `NV_ITEM_APP_USER_CFG` = `NV_ITEM_APP_GP_TRANS_TABLE + 1` = 0x2D. `NV_ITEM_APP_POWER_CNT` is
    0x2B. No collision with the SDK items, which stop at 0x2C; 0x80 is a separate item.
  - The item is the same ID as slacky's thermostat settings, but a size mismatch plus the magic
    check makes that harmless.
- **Flash map** (slacky `BOOT_LOADER_MODE 1` layout, now being replaced).
  - OTA client and bootloader both use `FLASH_ADDR_OF_OTA_IMAGE` = 0x8000 + (0xE6000 − 0x8000)/2
    = 0x77000, so they agree.
  - NV is 0xE6000–0xFC000. The old `main.c` migration erase of 0x70000 to <0xE6000 left NV alone,
    but it erased the MAC sector 0xFF000, so the IEEE address changes after migration.
  - The app (178 KB) fits in both the 444 KB bootloader-mode slot and the 256 KB normal-mode slot.
  - **Re-review the new `ota_scheme`/`BOOT_LOADER_MODE 0` change separately.** Normal mode puts NV
    at 0x80000 and the OTA image at 0x40000, so a device migrating from a bootloader-mode build
    would have its NV and image somewhere else.
- **Never-join / unintended-leave summary** (from RE §5).
  - A fresh device steers forever and ignores the MCU's `03 01` while not joined. It cannot get
    stuck.
  - Once joined, only these send status 0: M2's init-failure path, and a real leave.
  - The MCU's `03 01` triggers are: key held for 6 s or more (intended), the 10 s one-shot
    (M1, and M2 when joined) and 1→0 (only after our own leave).
  - An MCU reboot (60 s radar silence, 72 h uptime) is answered with status 1 on its first ping,
    so it does not trigger pairing.
- **Fuzzing.** No overflow, out-of-range index, infinite loop or UB in `tuya_parser_feed`,
  `tuya_dp_next`, `tuya_product_id`, `radar_on_dp`, `radar_lux_to_zcl` or `on_frame`. Overflow
  arithmetic on hostile DP values (e.g. `raw * distance_mm`) only wraps or clamps, which is
  harmless.
- **Removed slacky code.** Apart from B1, nothing needed is missing:
  - the time cluster is unneeded, since the MCU never asks for time (RE §2);
  - poll control is ED-only;
  - touchlink is off on purpose.
