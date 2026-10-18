/*
 * Move the firmware into the "big OTA, no bootloader" layout the Telink ROM boots natively:
 * images at 0x00000 or 0x40000, used alternately by OTA. The ROM picks the first slot whose
 * 'KNLT' marker (offset 8) is valid, so an update only takes effect once it is complete.
 *
 * Stock Tuya modules come in two layouts:
 *  - bootloader at 0x0 and the app at 0x8000 (the TuyaMCU bridge firmware): after the stock OTA
 *    our image runs at 0x8000, called by the Tuya bootloader;
 *  - no bootloader: the stock OTA leaves our image at 0x40000 (fine) or 0x20000 ("small OTA").
 *
 * Order of work, so that a power cut at any point leaves a bootable image:
 *  1. copy the running image to the free slot and verify every page,
 *  2. only then zero the 'KNLT' marker of the bootloader at 0x0, then the one at 0x8000
 *     (program-only writes, no erase),
 *  3. reset; the ROM now finds the copy.
 * Before step 2 the old chain (Tuya bootloader -> our app at 0x8000) still boots and simply
 * runs this again.
 *
 * Adapted from romasku/tuya-zigbee-switch (src/telink/ota_reformating/ensure_ota_scheme.c,
 * LICENSE-romasku). Change: sectors whose destination already holds the same bytes are not erased.
 * Without that, a power cut right after step 2 cleared 0x0 but not 0x8000 would boot the copy at
 * 0x40000, see 0x8000 still marked valid, and erase the very slot it just booted from; a second
 * power cut there would leave nothing bootable.
 *
 * Runs from RAM: when started by the Tuya bootloader at 0x8000, flash-resident code is linked for
 * a different offset and cannot be called.
 */
#include "ensure_ota_scheme.h"

#ifdef HOST_TEST
#include "host_flash.h"
#else
#include "ram_code_flash.h"
#pragma pack(push, 1)
#include "chip_8258/flash.h"
#include "tl_common.h"
#pragma pack(pop)
#endif

#define COPY_CHUNK      256
#define SECTOR_SIZE     0x1000

static int _attribute_ram_code_sec_ ram_code_memcmp(const u8 *a, const u8 *b, unsigned int n) {
    while (n--) {
        if (*a != *b) {
            return 1;
        }
        a++;
        b++;
    }
    return 0;
}

static bool _attribute_ram_code_sec_ is_valid_image_at_address(u32 addr) {
    u32 flag = 0;

    ram_code_flash_read_page(addr + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&flag);
    return flag == TL_START_UP_FLAG_WHOLE;
}

static u32 _attribute_ram_code_sec_ get_firmware_size(u32 addr) {
    u32 size = 0;

    ram_code_flash_read_page(addr + IMAGE_SIZE_OFFSET, 4, (u8 *)&size);
    return size;
}

static bool _attribute_ram_code_sec_ sector_matches(u32 from, u32 to, u32 len) {
    u32 a[COPY_CHUNK / 4];
    u32 b[COPY_CHUNK / 4];

    for (u32 i = 0; i < len; i += COPY_CHUNK) {
        ram_code_flash_read_page(from + i, COPY_CHUNK, (u8 *)a);
        ram_code_flash_read_page(to + i, COPY_CHUNK, (u8 *)b);
        if (ram_code_memcmp((u8 *)a, (u8 *)b, COPY_CHUNK)) {
            return false;
        }
    }
    return true;
}

static void _attribute_ram_code_sec_ move_flash_data(u32 from, u32 to, u32 size) {
    u32 buf[COPY_CHUNK / 4];
    u32 verify[COPY_CHUNK / 4];

    for (u32 s = 0; s < size; s += SECTOR_SIZE) {
        u32 len = (size - s) < SECTOR_SIZE ? (size - s) : SECTOR_SIZE;

        /* already there (e.g. we are re-running after a power cut): leave the sector alone */
        if (sector_matches(from + s, to + s, (len + COPY_CHUNK - 1) & ~(COPY_CHUNK - 1))) {
            continue;
        }

        ram_code_flash_erase_sector(to + s);

        for (u32 i = 0; i < len; i += COPY_CHUNK) {
            ram_code_flash_read_page(from + s + i, COPY_CHUNK, (u8 *)buf);
            ram_code_flash_write_page(to + s + i, COPY_CHUNK, (u8 *)buf);
            ram_code_flash_read_page(to + s + i, COPY_CHUNK, (u8 *)verify);
            if (ram_code_memcmp((u8 *)verify, (u8 *)buf, COPY_CHUNK)) {
                SYSTEM_RESET();     /* the old chain still boots and retries */
            }
        }
    }
}

void _attribute_ram_code_sec_ ensure_correct_ota_scheme(void) {
    u32 current_addr;
    u32 copy_to_addr = BIG_OTA_FLASH_ADDR;
    bool uses_bootloader = is_valid_image_at_address(BOOTLOADER_MODE_MAIN_ADDR);

    if (uses_bootloader) {
        current_addr = BOOTLOADER_MODE_MAIN_ADDR;
    } else if (is_valid_image_at_address(SMALL_OTA_FLASH_ADDR)) {
        current_addr = SMALL_OTA_FLASH_ADDR;
    } else {
        return;     /* booted from 0x00000 or 0x40000: already right */
    }

    u32 firmware_size = get_firmware_size(current_addr);
    if (firmware_size == 0 || firmware_size > MAX_FIRMWARE_SIZE) {
        SYSTEM_RESET();
    }

    ram_code_flash_write_status(FLASH_TYPE_8BIT_STATUS, 0);    /* drop write protection */

    move_flash_data(current_addr, copy_to_addr, firmware_size);

    u32 unused_flag = 0;
    if (uses_bootloader) {
        /* bootloader first: from here on the ROM boots the verified copy at 0x40000 */
        ram_code_flash_write_page(0x0 + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
        ram_code_flash_write_page(BOOTLOADER_MODE_MAIN_ADDR + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
    } else {
        ram_code_flash_write_page(SMALL_OTA_FLASH_ADDR + FLASH_TLNK_FLAG_OFFSET, 4, (u8 *)&unused_flag);
    }

    SYSTEM_RESET();
}
