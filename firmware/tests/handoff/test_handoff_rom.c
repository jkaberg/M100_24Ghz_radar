/*
 * Same power-cut simulation as test_handoff.c, with the boot model the final red team verified
 * against the Telink B85 handbook and a disassembly of the stock Tuya bootloader:
 *   - the ROM and the Tuya bootloader check only the byte 0x4B at +8 (slots 0x0/0x20000/0x40000);
 *   - interrupted programming leaves any subset of bits set (not just a prefix);
 *   - the stock bootloader's 3 s watchdog can reset the MCU at any flash operation of the copy.
 *
 * run: make -C firmware/tests handoff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_flash.h"
#include "../../src/ota_scheme/ensure_ota_scheme.h"

#define FLASH_SIZE 0x100000
static u8 flash[FLASH_SIZE];
static u8 base_flash[FLASH_SIZE];
static u8 *img;
static u32 img_len;
static u8 tuya_boot[0x8000];
jmp_buf g_reset;

static long ops, cut_at = -1, cut2_at = -1;
static unsigned rng = 12345;
static int rom_has_80000;
static int bitmode, wdmode;

static int power_cut(void) {
    ops++;
    if (ops == cut_at || ops == cut2_at) {
        return 1;
    }
    return 0;
}

void ram_code_flash_read_page(unsigned long addr, unsigned long len, unsigned char *buf) {
    memcpy(buf, &flash[addr], len);
}
void ram_code_flash_write_page(unsigned long addr, unsigned long len, unsigned char *buf) {
    unsigned long n = len;
    int cut = power_cut();
    if (cut && bitmode) {                       /* cell-level: any subset of bits programmed */
        for (unsigned long i = 0; i < len; i++) {
            rng = rng * 1103515245 + 12345;
            flash[addr + i] &= (u8)(buf[i] | (rng >> 16));
        }
        longjmp(g_reset, 2);
    }
    if (cut) {                                  /* only a prefix made it */
        rng = rng * 1103515245 + 12345;
        n = (rng >> 16) % (len + 1);
        if (len <= 4) n = (rng >> 16) % len;    /* marker writes: always partial */
    }
    for (unsigned long i = 0; i < n; i++) {
        flash[addr + i] &= buf[i];              /* NOR program: 1 -> 0 only */
    }
    if (cut) longjmp(g_reset, 2);
}
void ram_code_flash_erase_sector(unsigned long addr) {
    addr &= ~0xFFFul;
    if (power_cut()) {
        if (wdmode) { memset(&flash[addr], 0xFF, 0x1000); longjmp(g_reset, 2); }  /* MCU reset, flash finishes */
        for (int i = 0; i < 0x1000; i++) { rng = rng * 1103515245 + 12345; flash[addr + i] &= rng >> 16; }
        longjmp(g_reset, 2);
    }
    memset(&flash[addr], 0xFF, 0x1000);
}
void ram_code_flash_write_status(int type, unsigned short data) { (void)type; (void)data; }

static int marker_ok(u32 a) { return flash[a + 8] == 0x4B; }
static int image_at(u32 a) { return memcmp(&flash[a], img, img_len) == 0; }

/* returns 1 = our firmware running from a ROM slot, 0 = rebooting, -1 = dead */
static int boot_once(void) {
    static const u32 cand[] = {0x0, 0x20000, 0x40000, 0x80000};
    u32 slot = 0xFFFFFFFF;
    for (int i = 0; i < 3 + rom_has_80000; i++) {
        if (marker_ok(cand[i])) { slot = cand[i]; break; }
    }
    if (slot == 0xFFFFFFFF) return -1;

    int via_bootloader = 0;
    if (slot == 0x0 && memcmp(&flash[0], tuya_boot, 8) == 0 && memcmp(&flash[12], tuya_boot + 12, 0x8000 - 12) == 0) {
        if (!marker_ok(0x8000)) return -1;     /* Tuya bootloader with no app to start */
        if (!image_at(0x8000)) return -1;      /* would start something that isn't our image */
        via_bootloader = 1;
    } else if (!image_at(slot)) {
        return -1;                              /* ROM jumped into something that isn't us */
    }

    if (setjmp(g_reset)) return 0;
    if (via_bootloader) {
        /* started_by_tuya_bootloader(): reads 0x8008, physical while running from 0x8000 */
        ensure_correct_ota_scheme();
        SYSTEM_RESET();
    }
    /* booted by the ROM from `slot`: the mapped read of 0x8008 hits slot + 0x8008 */
    if (memcmp(&flash[slot + 0x8008], "KNLT", 4) == 0) {
        ensure_correct_ota_scheme();
        SYSTEM_RESET();
    }
    ensure_correct_ota_scheme();               /* flash_main(): normal-path check */
    return 1;
}

static int run_device(int *boots) {
    for (*boots = 1; *boots <= 20; (*boots)++) {
        int r = boot_once();
        if (r != 0) return r;
    }
    return -2;      /* reboot loop */
}

static long count_ops(void) {
    int b;
    memcpy(flash, base_flash, FLASH_SIZE);
    ops = 0; cut_at = cut2_at = -1;
    int r = run_device(&b);
    if (r != 1) { printf("FAIL: fault-free run ends with %d after %d boots\n", r, b); exit(1); }
    return ops;
}

static int sweep(const char *name, int pairs) {
    long total = count_ops(), fails = 0, runs = 0;
    int worst = 0, b;
    for (long c1 = 1; c1 <= total; c1++) {
        for (long c2 = pairs ? c1 + 1 : 0; c2 <= (pairs ? total + 1 : 0); c2++) {
            memcpy(flash, base_flash, FLASH_SIZE);
            ops = 0; cut_at = c1; cut2_at = pairs ? c2 : -1;
            int r = run_device(&b);
            runs++;
            if (b > worst) worst = b;
            if (r != 1) {
                if (fails++ < 5) printf("  FAIL %s: cut at op %ld%s -> %s\n", name, c1,
                                        pairs ? " and a later one" : "", r == -1 ? "dead" : "reboot loop");
            }
        }
    }
    printf("%-46s flash ops %6ld, runs %8ld, failures %ld, worst boots %d\n", name, total, runs, fails, worst);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    FILE *f;
    int bad = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s <stock_dump.bin> <our_image.bin>\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb"); if (fread(base_flash, 1, FLASH_SIZE, f) != FLASH_SIZE) return 2; fclose(f);
    f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); img_len = ftell(f); rewind(f);
    img = malloc(img_len); if (fread(img, 1, img_len, f) != img_len) return 2; fclose(f);
    memcpy(tuya_boot, base_flash, 0x8000);

    /* stock OTA result: image left in the download area, installed at 0x8000 */
    u8 stock[FLASH_SIZE];
    memcpy(stock, base_flash, FLASH_SIZE);
    memcpy(&base_flash[0x70000], img, img_len);
    memcpy(&base_flash[0x8000], img, img_len);

    rom_has_80000 = 0;
    printf("ROM/Tuya-BL check byte 0x4B at +8; candidates 0x0 0x20000 0x40000\n");
    bad |= sweep("  single cut (prefix program), full image", 0);
    bitmode = 1;
    bad |= sweep("  single cut (bit-level program), full image", 0);
    bitmode = 0; wdmode = 1;
    bad |= sweep("  single watchdog reset (no garbage), full image", 0);
    wdmode = 0; bitmode = 1;

    /* every pair of cuts: shrink the image (its header size field too) to keep this tractable */
    u32 full = img_len;
    img_len = 0x2000;
    u8 *small = malloc(img_len);
    memcpy(small, img, img_len);
    memcpy(&small[0x18], &img_len, 4);
    img = small;
    memcpy(base_flash, stock, FLASH_SIZE);
    memcpy(&base_flash[0x70000], img, img_len);
    memcpy(&base_flash[0x8000], img, img_len);
    rom_has_80000 = 0;
    bad |= sweep("  every pair of cuts, 8 KiB image", 1);
    (void)full;

    /* no-bootloader stock layout, small-OTA slot: old app at 0x0 disabled, ours at 0x20000 */
    memset(base_flash, 0xFF, FLASH_SIZE);
    memcpy(&base_flash[0x0], stock + 0x8000, 0x30000);  /* some old app */
    base_flash[8] = 0;                                  /* stock OTA disabled the old slot */
    memcpy(&base_flash[0x20000], img, img_len);
    memset(tuya_boot, 0xA5, sizeof(tuya_boot));         /* no Tuya bootloader in this layout */
    bad |= sweep("  every pair of cuts, small-OTA layout", 1);

    printf(bad ? "FAILED\n" : "PASS\n");
    return bad;
}
