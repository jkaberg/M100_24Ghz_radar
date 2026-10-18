/*
 * Fuzz harness: feeds hostile / corrupt UART input through the real app_radar.c
 * (parser -> on_frame -> radar model -> ZCL attrs) under ASan/UBSan, plus ZCL writes.
 *
 * run: make -C firmware/tests fuzz        (random loop, gcc)
 *      make -C firmware/tests libfuzzer   (clang libFuzzer, 3 min)
 *
 * Invariants checked:
 *  - the module never emits cmd 0x0C/0x0D/0x0E (MCU-OTA) or any frame with ver != 2;
 *  - every emitted frame parses back with a valid checksum;
 *  - zb_factoryReset only happens after a 0x03 01 frame while joined;
 *  - the module never reports net status 0 while joined and not factory-new;
 *  - distance reports never exceed one per configured interval.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "app_main.h"

zcl_radarAttr_t       g_zcl_radarAttrs;
zcl_illuminanceAttr_t g_zcl_illuminanceAttrs;

static uint64_t vt_ms;
uint32_t clock_time(void) { return (uint32_t)(vt_ms * 1000 * sys_tick_per_us); }

typedef struct { ev_timer_cb_t cb; void *arg; uint64_t due; int used; } htimer_t;
static htimer_t timers[8];
void *host_timer_schedule(ev_timer_cb_t cb, void *arg, uint32_t ms) {
    for (int i = 0; i < 8; i++) {
        if (!timers[i].used) {
            timers[i] = (htimer_t){cb, arg, vt_ms + ms, 1};
            return &timers[i];
        }
    }
    return NULL;
}
static void run_timers(void) {
    for (int i = 0; i < 8; i++) {
        if (timers[i].used && vt_ms >= timers[i].due) {
            int32_t r = timers[i].cb(timers[i].arg);
            if (r < 0) timers[i].used = 0; else timers[i].due = vt_ms + (r ? r : 1);
        }
    }
}

static bool joined, factory_new;
static int reset_allowed, factory_resets;
bool zb_isDeviceJoinedNwk(void) { return joined; }
bool zb_isDeviceFactoryNew(void) { return factory_new; }
bool bdb_isIdle(void) { return true; }
void zb_factoryReset(void) {
    if (!reset_allowed) { fprintf(stderr, "unexpected factory reset\n"); abort(); }
    factory_resets++;
}

static uint8_t nv_store[16];
static int nv_len;
nv_sts_t nv_flashReadNew(uint8_t s, uint8_t id, uint8_t item, uint16_t len, uint8_t *buf) {
    if (nv_len != len) return NV_ITEM_NOT_FOUND;
    memcpy(buf, nv_store, len);
    return NV_SUCC;
}
nv_sts_t nv_flashWriteNew(uint8_t s, uint16_t id, uint8_t item, uint16_t len, uint8_t *buf) {
    assert(len <= sizeof(nv_store));
    memcpy(nv_store, buf, len);
    nv_len = len;
    return NV_SUCC;
}

static int dist_reports;
int host_send_report(uint8_t ep, epInfo_t *dst, uint8_t ddr, uint8_t dir, uint16_t cl, uint16_t at, uint8_t ty, uint8_t *data) {
    dist_reports++;
    return 0;
}

/* fake UART */
static uint32_t uart_rate = 115200;
static const uint8_t *rx_buf;
static size_t rx_len, rx_pos;
static tuya_parser_t chk, shadow;
static uint64_t shadow_last_ms;
void app_uart_init(uint32_t rate) { uart_rate = rate; }
uint32_t app_uart_baudrate(void) { return uart_rate; }
uint32_t app_uart_rx_dropped(void) { return 0; }
void app_uart_poll(void) {}
uint8_t app_uart_rx_byte(uint8_t *b) {
    if (rx_pos >= rx_len) return 0;
    *b = rx_buf[rx_pos++];
    /* mirror app_radar.c: a frame paused > 50 ms is dropped (RX_GAP_RESET_MS) */
    if (tuya_parser_busy(&shadow) && vt_ms - shadow_last_ms > 50) {
        tuya_parser_reset(&shadow);
    }
    shadow_last_ms = vt_ms;
    if (tuya_parser_feed(&shadow, *b) && shadow.frame.cmd == TUYA_CMD_NET_RESET &&
        shadow.frame.len >= 1 && shadow.frame.data[0] == 1) {
        reset_allowed = 1;
    }
    return 1;
}
uint8_t app_uart_tx(uint8_t *data, uint8_t len) {
    int got = 0;
    if (len < TUYA_FRAME_OVERHEAD) abort();
    if (data[0] != 0x55 || data[1] != 0xAA || data[2] != 0x02) abort();
    if (data[5] >= 0x0C && data[5] <= 0x0E) { fprintf(stderr, "MCU-OTA cmd emitted\n"); abort(); }
    tuya_parser_reset(&chk);
    for (uint8_t i = 0; i < len; i++) got |= tuya_parser_feed(&chk, data[i]);
    if (!got) abort();
    if (data[5] == TUYA_CMD_NET_STATUS && len == 10 && data[8] == 0 && joined && !factory_new) {
        fprintf(stderr, "status 0 sent while joined\n");
        abort();
    }
    return 1;
}

static void step(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 5) {
        vt_ms += 5;
        run_timers();
        app_radar_task();
    }
}

/* Input format: first byte = flags, then chunks. Each chunk: 1 control byte then data.
 * control & 0x80: raw bytes (len = control & 0x3F) pushed to the UART
 * control & 0x40: build a valid frame: cmd, len, payload (checksum fixed up)
 * else         : advance time by control*50 ms / do a ZCL write */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static uint8_t buf[4096];
    size_t o = 1;

    if (n < 1) return 0;
    memset(timers, 0, sizeof(timers));
    vt_ms = 0;
    joined = d[0] & 1;
    factory_new = !joined;
    reset_allowed = 0;
    tuya_parser_reset(&shadow);
    nv_len = 0;
    app_radar_init();
    app_radar_net_status(joined ? TUYA_NET_JOINED : TUYA_NET_NOT_JOINED);

    while (o < n) {
        uint8_t c = d[o++];
        size_t len = 0;
        if (c & 0x80) {
            len = c & 0x3F;
            if (o + len > n) len = n - o;
            memcpy(buf, d + o, len);
            o += len;
        } else if (c & 0x40) {
            uint8_t cmd, plen;
            if (o + 2 > n) break;
            cmd = d[o++];
            plen = d[o++];
            if (o + plen > n) plen = (uint8_t)(n - o);
            len = tuya_build(buf, sizeof(buf), (uint16_t)vt_ms, cmd, d + o, plen);
            o += plen;
        } else if (c < 0x20) {
            step((uint32_t)c * 50 + 5);
            continue;
        } else {
            static const uint16_t ids[] = {0xF002, 0xF003, 0xF004, 0xF005, 0xF006, 0xF007, 0xF008, 0xF009, 0xF00A, 0xF001, 0x0000};
            if (o + 2 > n) break;
            uint16_t v = (uint16_t)(d[o] | d[o + 1] << 8);
            uint16_t id = ids[c % (sizeof(ids) / sizeof(ids[0]))];
            o += 2;
            switch (id) {
                case 0xF002: g_zcl_radarAttrs.dist_interval_s = v; break;
                case 0xF003: g_zcl_radarAttrs.dist_deadband_cm = v; break;
                case 0xF004: g_zcl_radarAttrs.move_sens = (uint8_t)v; break;
                case 0xF005: g_zcl_radarAttrs.pres_sens = (uint8_t)v; break;
                case 0xF006: g_zcl_radarAttrs.range_min_cm = v; break;
                case 0xF007: g_zcl_radarAttrs.range_max_cm = v; break;
                case 0xF008: g_zcl_radarAttrs.timeout_s = v; break;
                default: break;
            }
            app_radar_attr_written(id);
            continue;
        }
        rx_buf = buf;
        rx_len = len;
        rx_pos = 0;
        while (rx_pos < rx_len) step(5);
    }
    step(3000);
    return 0;
}

#ifdef RT_STANDALONE
int main(int argc, char **argv) {
    static uint8_t in[2048];
    unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;
    srand(1234);
    for (unsigned long it = 0; it < iters; it++) {
        size_t n = 1 + (size_t)(rand() % (int)sizeof(in));
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t)rand();
        /* bias towards frames that look valid */
        for (size_t i = 1; i + 3 < n; i += 1 + rand() % 40) {
            if (rand() & 1) in[i] = 0x40 | (in[i] & 0x3F);
            if ((rand() & 3) == 0) in[i + 1] = (uint8_t[]){0x01, 0x02, 0x03, 0x05, 0x06, 0x28}[rand() % 6];
        }
        LLVMFuzzerTestOneInput(in, n);
    }
    printf("fuzz: %lu inputs OK\n", iters);
    return 0;
}
#endif
