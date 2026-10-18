/*
 * Host test of the real app_radar.c against SDK stubs and a simulated radar MCU.
 * The simulated MCU follows the RE of the 7gclukjs GD32 firmware
 * (docs/gd32-7gclukjs-protocol.md): 115200 baud only, `0x03 00` at boot, empty 0x02 pings
 * every second while the status isn't 1, `0x03 01` 10 s after an unjoined status and on 1 -> 0,
 * DP writes echoed as 0x06, full dump on 0x28, DP9 once a second.
 *
 * Every frame the module sends is also written to tx_frames.bin so tools/check_frames_gd32.py
 * can push them through the real GD32 parser (unicorn emulation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_main.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); checks++; \
    if (_a != _b) { fails++; printf("FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

zcl_radarAttr_t       g_zcl_radarAttrs;
zcl_illuminanceAttr_t g_zcl_illuminanceAttrs;

/* ------------------------------------------------------------ virtual time + timers */
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

/* ------------------------------------------------------------ zigbee / nv / report stubs */
static bool joined, factory_new = true;
static int factory_resets;
bool zb_isDeviceJoinedNwk(void) { return joined; }
bool zb_isDeviceFactoryNew(void) { return factory_new; }
void zb_factoryReset(void) { factory_resets++; joined = false; factory_new = true; }
static bool bdb_idle = true;
bool bdb_isIdle(void) { return bdb_idle; }

/* one record per NV item id, like the SDK */
static uint8_t nv_store[256][16];
static int nv_len[256], nv_writes;
nv_sts_t nv_flashReadNew(uint8_t s, uint8_t id, uint8_t item, uint16_t len, uint8_t *buf) {
    (void)s; (void)id;
    if (nv_len[item] != len) return NV_ITEM_NOT_FOUND;
    memcpy(buf, nv_store[item], len);
    return NV_SUCC;
}
nv_sts_t nv_flashWriteNew(uint8_t s, uint16_t id, uint8_t item, uint16_t len, uint8_t *buf) {
    (void)s; (void)id;
    memcpy(nv_store[item], buf, len);
    nv_len[item] = len;
    nv_writes++;
    return NV_SUCC;
}

static int dist_reports;
static uint16_t dist_last;
int host_send_report(uint8_t ep, epInfo_t *dst, uint8_t ddr, uint8_t dir, uint16_t cl, uint16_t at, uint8_t ty, uint8_t *data) {
    (void)ep; (void)ddr; (void)dir; (void)ty;
    CHECK_EQ(dst->dstAddr.shortAddr, 0);
    CHECK_EQ(cl, 0x0406);
    CHECK_EQ(at, ATTRID_RADAR_DISTANCE);
    dist_reports++;
    memcpy(&dist_last, data, 2);
    return 0;
}

/* ------------------------------------------------------------ fake UART */
static uint32_t uart_rate = 9600;
static uint8_t to_module[8192];
static size_t tm_head, tm_tail;
static FILE *txlog;
static int tx_by_cmd[256];

static tuya_parser_t mcu_rx;
static void mcu_on_frame(const tuya_frame_t *f);
static int mcu_alive = 1;

void app_uart_init(uint32_t rate) { uart_rate = rate; }
uint32_t app_uart_baudrate(void) { return uart_rate; }
uint32_t app_uart_rx_dropped(void) { return 0; }
void app_uart_poll(void) {}
uint8_t app_uart_rx_byte(uint8_t *b) {
    if (tm_tail == tm_head) return 0;
    *b = to_module[tm_tail++ % sizeof(to_module)];
    return 1;
}
uint8_t app_uart_tx(uint8_t *data, uint8_t len) {
    uint8_t l = len;
    fwrite(&l, 1, 1, txlog);
    fwrite(data, 1, len, txlog);
    if (len > 5) tx_by_cmd[data[5]]++;
    /* the MCU only understands 115200 */
    if (uart_rate == 115200 && mcu_alive) {
        for (uint8_t i = 0; i < len; i++) {
            if (tuya_parser_feed(&mcu_rx, data[i])) mcu_on_frame(&mcu_rx.frame);
        }
    }
    return 1;
}

/* ------------------------------------------------------------ simulated radar MCU */
static uint8_t mcu_seq;
static int mcu_status = -1;
static uint64_t mcu_first_status_ms, mcu_ping_ms;
static int mcu_pair_requests, mcu_pair_armed = 1;
static uint32_t mcu_dp[256];

static void mcu_send(uint8_t cmd, const uint8_t *data, uint16_t len) {
    uint8_t buf[300];
    size_t n;
    if (!mcu_alive) return;
    n = tuya_build(buf, sizeof(buf), mcu_seq++, cmd, data, len);
    for (size_t i = 0; i < n; i++) to_module[tm_head++ % sizeof(to_module)] = buf[i];
}
static void mcu_report(uint8_t dp, uint8_t type, uint32_t v) {
    uint8_t d[8];
    size_t n = tuya_dp_put(d, sizeof(d), dp, type, v);
    mcu_dp[dp] = v;
    mcu_send(TUYA_CMD_DP_REPORT_ASYNC, d, (uint16_t)n);
}
static void mcu_dump(void) {
    mcu_report(1, TUYA_DP_ENUM, mcu_dp[1]);
    mcu_report(104, TUYA_DP_ENUM, mcu_dp[104]);
    mcu_report(103, TUYA_DP_VALUE, mcu_dp[103]);
    mcu_report(2, TUYA_DP_VALUE, mcu_dp[2]);
    mcu_report(102, TUYA_DP_VALUE, mcu_dp[102]);
    mcu_report(3, TUYA_DP_VALUE, mcu_dp[3]);
    mcu_report(4, TUYA_DP_VALUE, mcu_dp[4]);
    mcu_report(105, TUYA_DP_VALUE, mcu_dp[105]);
    mcu_report(0xFF, TUYA_DP_BOOL, 0);  /* the template junk DP */
}
static void mcu_boot(void) {
    uint8_t z = 0;
    mcu_status = -1;
    mcu_pair_armed = 1;
    mcu_send(TUYA_CMD_NET_RESET, &z, 1);
}
static void mcu_on_frame(const tuya_frame_t *f) {
    static const char info[] = "{\"p\":\"7gclukjs\",\"v\":\"1.0.0\"}";
    tuya_dp_t dp;
    uint16_t off = 0;

    CHECK_EQ(f->ver, 0x02);     /* the real parser drops any other version */
    switch (f->cmd) {
        case TUYA_CMD_PRODUCT_INFO:
            mcu_send(TUYA_CMD_PRODUCT_INFO, (const uint8_t *)info, sizeof(info) - 1);
            break;
        case TUYA_CMD_NET_STATUS: {
            int old = mcu_status;
            mcu_send(TUYA_CMD_NET_STATUS, NULL, 0);
            if (f->len < 1) break;
            mcu_status = f->data[0];
            if (old < 0) mcu_first_status_ms = vt_ms;
            if (old == 1 && mcu_status == 0) { uint8_t one = 1; mcu_pair_requests++; mcu_send(TUYA_CMD_NET_RESET, &one, 1); }
            if (mcu_status == 1 && old != 1) mcu_dump();
            break;
        }
        case TUYA_CMD_DP_WRITE:
            while (tuya_dp_next(f->data, f->len, &off, &dp)) {
                uint32_t v = tuya_dp_u32(&dp);
                if (dp.type != TUYA_DP_VALUE) continue;
                if (dp.id == 2 || dp.id == 102) mcu_report(dp.id, TUYA_DP_VALUE, v);   /* echoes raw */
                else if (dp.id == 3 || dp.id == 4) mcu_report(dp.id, TUYA_DP_VALUE, (v > 1000 ? 1000 : v) / 75 * 75);
                else if (dp.id == 105) mcu_report(dp.id, TUYA_DP_VALUE, v);
            }
            break;
        case TUYA_CMD_DP_QUERY:
            mcu_dump();
            break;
        case 0x0C: case 0x0D: case 0x0E:
            CHECK(!"module must never start an MCU-OTA");
            break;
        default:
            break;
    }
}
static void mcu_tick(void) {
    if (mcu_status != 1 && vt_ms - mcu_ping_ms >= 1000) {
        mcu_ping_ms = vt_ms;
        mcu_send(TUYA_CMD_NET_STATUS, NULL, 0);
    }
    if (mcu_status >= 0 && mcu_status != 1 && mcu_pair_armed && vt_ms - mcu_first_status_ms >= 10000) {
        uint8_t one = 1;
        mcu_pair_armed = 0;
        mcu_pair_requests++;
        mcu_send(TUYA_CMD_NET_RESET, &one, 1);
    }
}

/* ------------------------------------------------------------ driver */
static void run(uint64_t ms, void (*each_second)(uint64_t s)) {
    uint64_t end = vt_ms + ms;
    while (vt_ms < end) {
        vt_ms += 5;
        mcu_tick();
        if (each_second && vt_ms % 1000 == 0) each_second(vt_ms / 1000);
        run_timers();
        app_radar_task();
    }
}

static int tx_count(uint8_t cmd) { return tx_by_cmd[cmd]; }
static void tx_reset(void) { memset(tx_by_cmd, 0, sizeof(tx_by_cmd)); }

/* DP pattern captured from a real unit: 1 report a second, values 12..14 dm, someone in the room */
static const uint8_t flood_dm[] = {12, 12, 12, 13, 13, 12, 12, 14, 14, 13, 13, 12, 12, 12, 13, 13};
static void flood(uint64_t s) {
    mcu_report(9, TUYA_DP_VALUE, flood_dm[s % sizeof(flood_dm)]);
    if (s % 7 == 0) mcu_report(1, TUYA_DP_ENUM, (s / 7) % 2 ? 2 : 1);
    if (s % 10 == 0) mcu_dump();        /* the "every 10 s for 6 rounds" re-dump */
}
static int walk_dm;
static void walk(uint64_t s) {
    /* someone walking back and forth 0.5 - 6 m */
    walk_dm = 5 + (int)((s * 3) % 110);
    mcu_report(9, TUYA_DP_VALUE, walk_dm > 60 ? 120 - walk_dm : walk_dm);
}

int main(void) {
    txlog = fopen("tx_frames.bin", "wb");
    tuya_parser_reset(&mcu_rx);
    mcu_dp[3] = 75; mcu_dp[4] = 300; mcu_dp[105] = 15; mcu_dp[103] = 5; mcu_dp[2] = 7; mcu_dp[102] = 7;

    /* --- S1: fresh device, steering; MCU boots at the same time */
    app_radar_init();
    mcu_boot();
    app_radar_net_status(TUYA_NET_JOINING);
    run(15000, NULL);
    CHECK_EQ(uart_rate, 115200);
    CHECK_EQ(mcu_status, TUYA_NET_JOINING);
    CHECK_EQ(mcu_pair_requests, 1);          /* MCU asked to pair while we were not joined... */
    CHECK_EQ(factory_resets, 0);             /* ...and we did not leave anything */
    CHECK(tx_count(TUYA_CMD_NET_STATUS) <= 8);  /* answering its 1 s pings, rate limited */

    joined = true;
    factory_new = false;
    app_radar_net_status(TUYA_NET_JOINED);
    run(2000, NULL);
    CHECK_EQ(mcu_status, 1);
    CHECK_EQ(g_zcl_radarAttrs.range_max_cm, 300);       /* from the dump on status -> 1 */
    CHECK_EQ(g_zcl_radarAttrs.timeout_s, 15);
    tx_reset();
    run(60000, NULL);
    CHECK(tx_count(TUYA_CMD_NET_STATUS) <= 1);           /* no 0x02 ping-pong once joined */

    /* --- S2: parent lost, rejoining: must keep saying "joined" or the MCU makes us leave */
    joined = false;
    app_radar_net_status(TUYA_NET_JOINING);
    app_radar_net_status(TUYA_NET_NOT_JOINED);
    run(30000, NULL);
    CHECK_EQ(mcu_status, 1);
    CHECK_EQ(mcu_pair_requests, 1);
    CHECK_EQ(factory_resets, 0);
    joined = true;
    app_radar_net_status(TUYA_NET_JOINED);

    /* --- S3: the flood, 10 minutes, distance reporting off (default) */
    tx_reset();
    run(600000, flood);
    CHECK_EQ(dist_reports, 0);
    CHECK_EQ(g_zcl_radarAttrs.distance_cm, 0);           /* attribute untouched when off */
    CHECK_EQ(g_zcl_radarAttrs.occupancy, 0);             /* DP104 never changed in this run */
    CHECK(g_zcl_radarAttrs.state == 1 || g_zcl_radarAttrs.state == 2);
    CHECK_EQ(g_zcl_illuminanceAttrs.measuredValue, radar_lux_to_zcl(5));
    mcu_report(104, TUYA_DP_ENUM, 1);
    run(100, NULL);
    CHECK_EQ(g_zcl_radarAttrs.occupancy, 1);

    /* --- S4: distance on, every 60 s at most, 25 cm deadband, 10 min of walking */
    g_zcl_radarAttrs.dist_interval_s = 60;
    g_zcl_radarAttrs.dist_deadband_cm = 25;
    app_radar_attr_written(ATTRID_RADAR_DIST_INTERVAL);
    CHECK_EQ(nv_writes, 1);
    run(600000, walk);
    CHECK(dist_reports >= 9 && dist_reports <= 11);
    printf("info: distance reports in 10 min at 60 s: %d (last %u cm)\n", dist_reports, dist_last);
    /* out-of-range values are clamped and persisted */
    g_zcl_radarAttrs.dist_interval_s = 60000;
    app_radar_attr_written(ATTRID_RADAR_DIST_INTERVAL);
    CHECK_EQ(g_zcl_radarAttrs.dist_interval_s, RADAR_MAX_DIST_INTERVAL);
    g_zcl_radarAttrs.dist_interval_s = 0;
    app_radar_attr_written(ATTRID_RADAR_DIST_INTERVAL);
    int before = dist_reports;
    run(120000, walk);
    CHECK_EQ(dist_reports, before);

    /* --- S5: settings written over Zigbee go to the MCU (clamped) and come back as reports */
    g_zcl_radarAttrs.move_sens = 99;
    app_radar_attr_written(ATTRID_RADAR_MOVE_SENS);
    g_zcl_radarAttrs.range_max_cm = 450;
    app_radar_attr_written(ATTRID_RADAR_RANGE_MAX);
    run(500, NULL);
    CHECK_EQ(mcu_dp[2], 10);
    CHECK_EQ(g_zcl_radarAttrs.move_sens, 10);
    CHECK_EQ(g_zcl_radarAttrs.range_max_cm, 450);       /* 450 is a 75 cm gate: kept */
    g_zcl_radarAttrs.range_max_cm = 500;
    app_radar_attr_written(ATTRID_RADAR_RANGE_MAX);
    run(500, NULL);
    CHECK_EQ(g_zcl_radarAttrs.range_max_cm, 450);       /* MCU rounds to its gate, we follow */

    /* --- S6: MCU reboot (0x03 00) changes nothing; key hold (0x03 01) while joined re-pairs */
    mcu_boot();
    run(3000, NULL);
    CHECK_EQ(factory_resets, 0);
    { uint8_t one = 1; mcu_send(TUYA_CMD_NET_RESET, &one, 1); }
    run(1000, NULL);
    CHECK_EQ(factory_resets, 1);

    /* --- S7: MCU silent for 20 min, then back: the module finds it again at 115200 */
    joined = true; factory_new = false;
    app_radar_net_status(TUYA_NET_JOINED);
    mcu_alive = 0;
    run(20 * 60000, NULL);
    mcu_alive = 1;
    tuya_parser_reset(&mcu_rx);
    run(30000, NULL);
    CHECK_EQ(uart_rate, 115200);
    tx_reset();
    mcu_report(104, TUYA_DP_ENUM, 0);
    run(200, NULL);
    CHECK_EQ(g_zcl_radarAttrs.occupancy, 0);

    /* --- S9 (review M1/L4): pairing requests during commissioning are ignored, a burst of them
     *     while idle-and-joined leaves only once */
    int resets0 = factory_resets;
    joined = true; factory_new = false; bdb_idle = false;
    { uint8_t one = 1; mcu_send(TUYA_CMD_NET_RESET, &one, 1); }
    run(2000, NULL);
    CHECK_EQ(factory_resets, resets0);
    bdb_idle = true; joined = true; factory_new = true;     /* associated, still factory-new */
    { uint8_t one = 1; mcu_send(TUYA_CMD_NET_RESET, &one, 1); }
    run(2000, NULL);
    CHECK_EQ(factory_resets, resets0);
    factory_new = false;
    for (int i = 0; i < 3; i++) { uint8_t one = 1; mcu_send(TUYA_CMD_NET_RESET, &one, 1); }
    run(2000, NULL);
    CHECK_EQ(factory_resets, resets0 + 1);

    /* --- S10 (review M2): a device with a network says "joined" even if the stack never told us */
    joined = true; factory_new = false;
    app_radar_net_status(TUYA_NET_NOT_JOINED);     /* e.g. init failure path */
    mcu_status = -1;
    { uint8_t z = 0; mcu_send(TUYA_CMD_NET_STATUS, &z, 0); }    /* MCU ping makes us answer */
    run(3000, NULL);
    CHECK_EQ(mcu_status, 1);

    /* --- S11 (review L1): a frame cut short by an MCU reset doesn't swallow the next ones */
    {
        uint8_t partial[] = {0x55, 0xAA, 0x02, 0x00, 0x10, 0x06, 0x00, 0x60, 0x09, 0x02};
        for (size_t i = 0; i < sizeof(partial); i++) to_module[tm_head++ % sizeof(to_module)] = partial[i];
        run(200, NULL);                                 /* silence > gap timeout */
        mcu_report(104, TUYA_DP_ENUM, 1);
        mcu_report(104, TUYA_DP_ENUM, 0);
        mcu_report(104, TUYA_DP_ENUM, 1);
        run(200, NULL);
        CHECK_EQ(g_zcl_radarAttrs.occupancy, 1);
    }

    /* --- S12: distance calibration written over Zigbee: applied, clamped, persisted */
    {
        joined = true; factory_new = false;
        g_zcl_radarAttrs.dist_interval_s = 1;
        g_zcl_radarAttrs.dist_deadband_cm = 0;
        app_radar_attr_written(ATTRID_RADAR_DIST_INTERVAL);
        g_zcl_radarAttrs.dist_offset_cm = 30;
        g_zcl_radarAttrs.dist_scale_pct = 100;
        app_radar_attr_written(ATTRID_RADAR_DIST_OFFSET);
        int before = nv_writes;
        app_radar_attr_written(ATTRID_RADAR_DIST_OFFSET);       /* same values: no flash write */
        CHECK_EQ(nv_writes, before);
        mcu_report(9, TUYA_DP_VALUE, 12);
        run(1500, NULL);
        CHECK_EQ(dist_last, 150);                               /* 120 cm from the MCU + 30 */
        CHECK_EQ(g_zcl_radarAttrs.distance_cm, 150);

        g_zcl_radarAttrs.dist_offset_cm = -999;                 /* out of range: clamped */
        g_zcl_radarAttrs.dist_scale_pct = 1000;
        app_radar_attr_written(ATTRID_RADAR_DIST_SCALE);
        CHECK_EQ(g_zcl_radarAttrs.dist_offset_cm, -RADAR_DIST_OFFSET_MAX);
        CHECK_EQ(g_zcl_radarAttrs.dist_scale_pct, RADAR_DIST_SCALE_MAX);

        /* restart: calibration and the interval/deadband policy both come back from NV */
        g_zcl_radarAttrs.dist_offset_cm = 25;
        g_zcl_radarAttrs.dist_scale_pct = 95;
        app_radar_attr_written(ATTRID_RADAR_DIST_OFFSET);
        memset(&g_zcl_radarAttrs, 0, sizeof(g_zcl_radarAttrs));
        app_radar_init();
        CHECK_EQ(g_zcl_radarAttrs.dist_offset_cm, 25);
        CHECK_EQ(g_zcl_radarAttrs.dist_scale_pct, 95);
        CHECK_EQ(g_zcl_radarAttrs.dist_interval_s, 1);
        run(5000, NULL);                                        /* relock to the MCU */
        mcu_report(9, TUYA_DP_VALUE, 20);
        run(1500, NULL);
        CHECK_EQ(dist_last, 215);                               /* 200 * 95 % + 25 */
    }

    /* --- S8 is enforced throughout by mcu_on_frame(): no 0x0C/0x0D/0x0E ever */

    fclose(txlog);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
