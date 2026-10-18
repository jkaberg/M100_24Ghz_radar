/*
 * Entry point. Based on the Telink SDK sample main.c (Apache-2.0) and, for the boot order, on
 * romasku/tuya-zigbee-switch src/telink/main.c (see ota_scheme/NOTICE).
 *
 * 1. If the stock Tuya bootloader started us at 0x8000 (first boot after the update from stock),
 *    only RAM code may run: move the image into the ROM's A/B slot layout and reset.
 * 2. Otherwise normal start-up, then the same check once more for the "small OTA" stock layout.
 */
#include "zb_common.h"
#include "ota_scheme/ensure_ota_scheme.h"

extern void user_init(bool isRetention);

static _attribute_ram_code_sec_ bool started_by_tuya_bootloader(void) {
    return *((u32 *)(BOOTLOADER_MODE_MAIN_ADDR + FLASH_TLNK_FLAG_OFFSET)) == TL_START_UP_FLAG_WHOLE;
}

static __attribute__((noinline)) int flash_main(void) {
    startup_state_e state = drv_platform_init();

    ensure_correct_ota_scheme();

    u8 isRetention = (state == SYSTEM_DEEP_RETENTION) ? 1 : 0;

    os_init(isRetention);

    user_init(isRetention);

    drv_enable_irq();

#if (MODULE_WATCHDOG_ENABLE)
    drv_wd_setInterval(1000);
    drv_wd_start();
#endif

    while (1) {
        ev_main();

#if (MODULE_WATCHDOG_ENABLE)
        drv_wd_clear();
#endif

        tl_zbTaskProcedure();

#if (MODULE_WATCHDOG_ENABLE)
        drv_wd_clear();
#endif
    }

    return 0;
}

_attribute_ram_code_sec_ int main(void) {
    if (started_by_tuya_bootloader()) {
        /* The Tuya bootloader has done the platform set-up. Flash-resident code is linked for a
         * different offset here, so nothing but RAM code (no printf, no SDK calls). */
        ensure_correct_ota_scheme();
        SYSTEM_RESET();
    }

    return flash_main();
}
