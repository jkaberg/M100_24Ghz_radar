/*
 * Runs the SDK's real drv_nv.c (3.7.2.0, copied in by the Makefile) against the flash as it looks
 * right after the stock -> custom conversion:
 *   real stock ZT3L dump; our image at 0x8000 (Tuya bootloader install) and left at 0x70000
 *   (stock OTA download area, ~0x70000-0x9C7A4, i.e. over the whole no-bootloader NV area
 *   0x80000-0x96000); the handoff's copy at 0x40000; markers at 0x0008/0x8008 zeroed.
 * Then many "boots" of reads + writes over every NV module, read-back checks, erase counting and
 * an op budget to catch loops. Also an adversarial pass where every NV sector starts with a
 * forged-but-plausible header (0x5A5A / 0x7A7A, right idName, garbage index).
 *
 * run: make -C firmware/tests nv
 */
#include "tl_common.h"
#include <assert.h>

#define FS 0x100000
static u8 fl[FS];
static long erases[FS / 0x1000], ops;

static void chk(u32 a, u32 l) { if (a >= FS || a + l > FS) { printf("FAIL: NV accessed 0x%x+%u outside flash\n", a, l); exit(1); } }
static void budget(void) { if (++ops > 50000000) { printf("FAIL: op budget exceeded (loop?)\n"); exit(1); } }
void flash_read(u32 a, u32 l, u8 *b) { chk(a, l); budget(); memcpy(b, &fl[a], l); }
void flash_write(u32 a, u32 l, u8 *b) { chk(a, l); budget(); for (u32 i = 0; i < l; i++) fl[a + i] &= b[i]; }
void flash_erase(u32 a) { a &= ~0xFFFu; chk(a, 0x1000); budget(); memset(&fl[a], 0xFF, 0x1000); erases[a >> 12]++; }
bool flash_writeWithCheck(u32 a, u32 l, u8 *b) { flash_write(a, l, b); return memcmp(&fl[a], b, l) == 0; }
void *ev_buf_allocate(u16 s) { return malloc(s); }
void ev_buf_free(void *p) { free(p); }
static unsigned int crc_tab[256];
unsigned int xcrc32(const unsigned char *buf, int len, unsigned int init) {
    if (!crc_tab[1]) for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    unsigned int crc = init;
    while (len--) { crc = (crc >> 8) ^ crc_tab[(crc ^ *buf) & 0xff]; buf++; }
    return crc;
}

static unsigned rng = 1;
static unsigned rnd(void) { rng = rng * 1103515245 + 12345; return rng >> 8; }

/* module -> (item, size) used by the stack/app; sizes are plausible, not exact */
static const struct { u8 mod, item; u16 len; } items[] = {
    {NV_MODULE_ZB_INFO, NV_ITEM_ZB_INFO, 220},
    {NV_MODULE_ADDRESS_TABLE, NV_ITEM_ADDRESS_FOR_NEIGHBOR, 600},
    {NV_MODULE_APS, NV_ITEM_APS_SSIB, 180},
    {NV_MODULE_APS, NV_ITEM_APS_BINDING_TABLE_V2, 120},
    {NV_MODULE_ZCL, NV_ITEM_ZCL_REPORT, 300},
    {NV_MODULE_OTA, NV_ITEM_OTA_HDR_SERVERINFO, 80},
    {NV_MODULE_APP, NV_ITEM_APP_POWER_CNT, 1},
    {NV_MODULE_APP, 0x40, 1},           /* boot counter (app item) */
    {NV_MODULE_APP, 0x41, 24},          /* radar settings */
    {NV_MODULE_KEYPAIR, NV_ITEM_SS_KEY_PAIR, 60},
};
#define NI (sizeof(items) / sizeof(items[0]))
static u8 shadow[NI][1024]; static int have[NI];

static int scenario(const char *name, u8 *base, int boots) {
    memcpy(fl, base, FS); memset(erases, 0, sizeof(erases)); ops = 0; memset(have, 0, sizeof(have));
    int fails = 0; u32 fc = 0, fcr;
    for (int b = 0; b < boots; b++) {
        /* boot: everything is read first (stack restore), garbage must read as "not found" */
        for (unsigned i = 0; i < NI; i++) {
            u8 buf[1024];
            nv_sts_t st = nv_flashReadNew(1, items[i].mod, items[i].item, items[i].len, buf);
            if (have[i] && (st != NV_SUCC || memcmp(buf, shadow[i], items[i].len))) { if (fails++ < 5) printf("  %s boot %d: item %u/%u lost (st %d)\n", name, b, items[i].mod, items[i].item, st); }
            if (!have[i] && st == NV_SUCC) { if (fails++ < 5) printf("  %s boot %d: garbage read back as valid item %u/%u\n", name, b, items[i].mod, items[i].item); }
        }
        if (b > 0 && nv_nwkFrameCountFromFlash(&fcr) == NV_SUCC && fcr < fc) { if (fails++ < 5) printf("  %s: frame counter went back %u < %u\n", name, fcr, fc); }
        /* a few writes per boot (join, boot counters, settings), occasionally all of them */
        for (unsigned i = 0; i < NI; i++) {
            if (b == 0 || (rnd() % 4) == 0 || items[i].mod == NV_MODULE_APP) {
                for (int k = 0; k < items[i].len; k++) shadow[i][k] = rnd();
                nv_sts_t st = nv_flashWriteNew(1, items[i].mod, items[i].item, items[i].len, shadow[i]);
                if (st != NV_SUCC) { if (fails++ < 5) printf("  %s boot %d: write %u/%u failed st %d\n", name, b, items[i].mod, items[i].item, st); have[i] = 0; }
                else have[i] = 1;
            }
        }
        fc += 1 + rnd() % 1024;   /* the stack saves every <=1024 frames (UPDATE_FRAMECOUNT_THRES) */ nv_nwkFrameCountSaveToFlash(fc);
    }
    long maxe = 0; for (int s = 0x80; s < 0x96; s++) if (erases[s] > maxe) maxe = erases[s];
    long outside = 0; for (int s = 0; s < FS / 0x1000; s++) if ((s < 0x80 || s >= 0x96) && erases[s]) outside++;
    printf("%-44s boots %4d, failures %d, max erases/sector %ld (%.2f per boot), erased outside NV: %ld sectors, ops %ld\n",
           name, boots, fails, maxe, (double)maxe / boots, outside, ops);
    return fails || outside;
}

int main(int argc, char **argv) {
    static u8 stock[FS], base[FS];
    FILE *f = fopen(argv[1], "rb"); if (!f || fread(stock, 1, FS, f) != FS) return 2; fclose(f);
    f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    u8 *img = malloc(n); if (fread(img, 1, n, f) != (size_t)n) return 2; fclose(f);
    int bad = 0;

    memcpy(base, stock, FS);
    memset(&base[0x8000], 0xFF, (n + 0xFFF) & ~0xFFF); memcpy(&base[0x8000], img, n);
    memset(&base[0x70000], 0xFF, (n + 0xFFF) & ~0xFFF); memcpy(&base[0x70000], img, n);
    memset(&base[0x40000], 0xFF, (n + 0xFFF) & ~0xFFF); memcpy(&base[0x40000], img, n);
    memset(&base[0x0008], 0, 4); memset(&base[0x8008], 0, 4);

    /* what the NV sector headers look like right after the conversion */
    for (int m = 0; m < NV_MAX_MODULS; m++)
        for (int s = 0; s < 2; s++) {
            u32 a = MODULE_SECT_START(m, s);
            printf("module %d sect %d @0x%05x: usedFlag %02x%02x idName %02x opSect %02x\n", m, s, a, base[a + 1], base[a], base[a + 2], base[a + 3]);
        }
    bad |= scenario("converted flash (image garbage in NV)", base, 300);

    /* adversarial: plausible forged headers in every NV sector, garbage index after them */
    for (int pass = 0; pass < 200; pass++) {
        static u8 adv[FS];
        memcpy(adv, base, FS);
        rng = 7 + pass;
        for (int m = 0; m < NV_MAX_MODULS; m++)
            for (int s = 0; s < 2; s++) {
                u32 a = MODULE_SECT_START(m, s);
                u16 flag = (rnd() & 1) ? NV_SECTOR_VALID : NV_SECTOR_VALID_CHECKCRC;
                adv[a] = flag & 0xFF; adv[a + 1] = flag >> 8; adv[a + 2] = m; adv[a + 3] = s | (rnd() << 2);
                /* random index entries, some pointing into the context area with "valid" states */
                for (u32 k = 4; k < MODULE_INFO_SIZE(m); k += 8) {
                    u32 off = MODULE_CONTEXT_START(m, s, 0) + rnd() % 0x800;
                    memcpy(&adv[a + k], &off, 4);
                    adv[a + k + 4] = rnd(); adv[a + k + 5] = rnd() & 3; adv[a + k + 6] = items[rnd() % NI].item;
                    static const u8 st[] = {0x5A, 0x7A, 0x50, 0xFA, 0xFF, 0x00};
                    adv[a + k + 7] = st[rnd() % 6];
                }
            }
        char nm[64]; snprintf(nm, sizeof nm, "forged NV headers, seed %d", pass);
        int r = scenario(nm, adv, 40);
        bad |= r;
        if (pass > 3 && !r) { /* print only first few lines */ }
    }
    free(img);
    printf(bad ? "FAILED\n" : "PASS\n");
    return bad;
}
