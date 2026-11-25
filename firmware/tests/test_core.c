/* Host unit tests for the SDK-free core: tuya_mcu.c and radar.c.  Build and run: make -C tests */
#include <stdio.h>
#include <string.h>
#include "tuya_mcu.h"
#include "radar.h"

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); checks++; \
    if (_a != _b) { fails++; printf("FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* Feed bytes; return number of complete frames seen, last one left in p->frame */
static int feed(tuya_parser_t *p, const uint8_t *b, size_t n) {
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        frames += tuya_parser_feed(p, b[i]);
    }
    return frames;
}

/* Build an MCU->module DP report frame carrying one 4-byte value DP */
static size_t dp_frame(uint8_t *buf, uint16_t seq, uint8_t cmd, uint8_t dp, uint8_t type, uint32_t v) {
    uint8_t data[16];
    size_t n = tuya_dp_put(data, sizeof(data), dp, type, v);
    return tuya_build(buf, 64, seq, cmd, data, (uint16_t)n);
}

static tuya_dp_t mkdp(uint8_t id, uint8_t type, uint32_t v, uint8_t *store) {
    tuya_dp_t dp;
    size_t n = tuya_dp_put(store, 8, id, type, v);
    uint16_t off = 0;
    tuya_dp_next(store, (uint16_t)n, &off, &dp);
    return dp;
}

static void test_build_and_parse(void) {
    tuya_parser_t p;
    uint8_t buf[64];

    tuya_parser_reset(&p);

    /* product-info query, as the stock module sends it: 55 AA 02 00 01 01 00 00 sum */
    size_t n = tuya_build(buf, sizeof(buf), 1, TUYA_CMD_PRODUCT_INFO, NULL, 0);
    CHECK_EQ(n, 9);
    CHECK_EQ(buf[0], 0x55);
    CHECK_EQ(buf[1], 0xAA);
    CHECK_EQ(buf[8], (0x55 + 0xAA + 0x02 + 0x00 + 0x01 + 0x01) & 0xFF);
    CHECK_EQ(feed(&p, buf, n), 1);
    CHECK_EQ(p.frame.cmd, TUYA_CMD_PRODUCT_INFO);
    CHECK_EQ(p.frame.seq, 1);
    CHECK_EQ(p.frame.len, 0);

    /* too small a buffer */
    CHECK_EQ(tuya_build(buf, 8, 1, 1, NULL, 0), 0);
}

static void test_parser_resync(void) {
    tuya_parser_t p;
    uint8_t buf[64], stream[256];
    size_t n, s = 0;

    tuya_parser_reset(&p);

    /* garbage, a stray 0x55, a frame with a bad checksum, then two good frames back to back */
    stream[s++] = 0x00;
    stream[s++] = 0x55;
    stream[s++] = 0x13;
    n = dp_frame(buf, 7, TUYA_CMD_DP_REPORT_ASYNC, 9, TUYA_DP_VALUE, 12);
    memcpy(stream + s, buf, n);
    stream[s + n - 1] ^= 0xFF;          /* corrupt checksum */
    s += n;
    n = dp_frame(buf, 8, TUYA_CMD_DP_REPORT_ASYNC, 9, TUYA_DP_VALUE, 13);
    memcpy(stream + s, buf, n);
    s += n;

    CHECK_EQ(feed(&p, stream, s), 1);
    CHECK_EQ(p.bad_sum, 1);
    CHECK_EQ(p.frame.seq, 8);

    tuya_dp_t dp;
    uint16_t off = 0;
    CHECK(tuya_dp_next(p.frame.data, p.frame.len, &off, &dp));
    CHECK_EQ(dp.id, 9);
    CHECK_EQ(tuya_dp_u32(&dp), 13);

    /* 55 55 AA ...: the second 0x55 must start the frame */
    tuya_parser_reset(&p);
    n = dp_frame(buf + 1, 9, TUYA_CMD_DP_REPORT_ASYNC, 1, TUYA_DP_ENUM, 2);
    buf[0] = 0x55;
    CHECK_EQ(feed(&p, buf, n + 1), 1);
    CHECK_EQ(p.frame.seq, 9);

    /* one corrupt length byte must not cost more than its own frame (red-team L1) */
    {
        uint8_t st[2048], fr[64], d[5] = {104, 4, 0, 1, 1};
        size_t sn = 0, k;
        int got;
        tuya_parser_t q;
        tuya_parser_reset(&q);
        q.bad_sum = q.too_long = 0;
        for (int i = 0; i < 20; i++) {
            k = tuya_build(fr, sizeof(fr), (uint16_t)i, 0x06, d, 5);
            if (i == 0) fr[7] = 0xF0;
            memcpy(st + sn, fr, k);
            sn += k;
        }
        got = feed(&q, st, sn);
        CHECK_EQ(got, 19);
    }

    /* declared length beyond our buffer is dropped, parser recovers afterwards */
    uint8_t big[] = {0x55, 0xAA, 0x02, 0x00, 0x01, 0x06, 0x01, 0x10};
    tuya_parser_reset(&p);
    CHECK_EQ(feed(&p, big, sizeof(big)), 0);
    CHECK_EQ(p.too_long, 1);
    n = dp_frame(buf, 10, TUYA_CMD_DP_REPORT_ASYNC, 9, TUYA_DP_VALUE, 5);
    CHECK_EQ(feed(&p, buf, n), 1);
}

static void test_dp_codec(void) {
    uint8_t data[32];
    size_t n = 0;

    n += tuya_dp_put(data + n, sizeof(data) - n, 104, TUYA_DP_BOOL, 1);
    n += tuya_dp_put(data + n, sizeof(data) - n, 1, TUYA_DP_ENUM, 2);
    n += tuya_dp_put(data + n, sizeof(data) - n, 9, TUYA_DP_VALUE, 0x01020304);
    CHECK_EQ(n, 5 + 5 + 8);
    CHECK_EQ(tuya_dp_put(data, 7, 9, TUYA_DP_VALUE, 1), 0);     /* does not fit */
    CHECK_EQ(tuya_dp_put(data, 8, 9, TUYA_DP_RAW, 1), 0);       /* unsupported type */

    tuya_dp_t dp;
    uint16_t off = 0;
    CHECK(tuya_dp_next(data, (uint16_t)n, &off, &dp));
    CHECK_EQ(dp.id, 104);
    CHECK_EQ(tuya_dp_u32(&dp), 1);
    CHECK(tuya_dp_next(data, (uint16_t)n, &off, &dp));
    CHECK_EQ(dp.id, 1);
    CHECK_EQ(tuya_dp_u32(&dp), 2);
    CHECK(tuya_dp_next(data, (uint16_t)n, &off, &dp));
    CHECK_EQ(dp.id, 9);
    CHECK_EQ(tuya_dp_u32(&dp), 0x01020304);
    CHECK(!tuya_dp_next(data, (uint16_t)n, &off, &dp));

    /* truncated unit is rejected */
    off = 0;
    CHECK(!tuya_dp_next(data, 6, &(uint16_t){5}, &dp));
    CHECK(!tuya_dp_next(data, 3, &off, &dp));
}

static void test_product_id(void) {
    const char *json = "{\"p\":\"7gclukjs\",\"v\":\"1.0.0\"}";
    char pid[16];

    CHECK_EQ(tuya_product_id((const uint8_t *)json, (uint16_t)strlen(json), pid, sizeof(pid)), 8);
    CHECK(strcmp(pid, "7gclukjs") == 0);
    CHECK(radar_variant_find(pid) != NULL);
    CHECK(radar_variant_find("e5m9c5hl") == NULL);
    CHECK(radar_variant_find("ya4ft0w") == NULL);
    CHECK(radar_variant_find("7gclukj") == NULL);
    CHECK(radar_variant_find("7gclukjsx") == NULL);

    /* the radar MCU sends the strings NUL-separated in flash, but over UART it is JSON; also
       make sure a short cap truncates instead of overflowing */
    CHECK_EQ(tuya_product_id((const uint8_t *)json, (uint16_t)strlen(json), pid, 4), 3);
    CHECK_EQ(tuya_product_id((const uint8_t *)"{\"v\":\"1\"}", 9, pid, sizeof(pid)), 0);
}

static radar_t mkradar(uint16_t interval, uint16_t deadband) {
    radar_t r;
    radar_init(&r, radar_variant_find("7gclukjs"), interval, deadband);
    return r;
}

static void test_duplicates_dropped(void) {
    radar_t r = mkradar(0, 25);
    uint8_t st[8];
    tuya_dp_t dp;

    /* first presence value is a change, the repeats are not */
    /* DP104 is an enum on the 7gclukjs MCU; accept bool too */
    dp = mkdp(104, TUYA_DP_ENUM, 1, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_PRESENCE);
    dp = mkdp(104, TUYA_DP_BOOL, 1, st);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(radar_on_dp(&r, &dp, 100 * i), 0);
    }
    dp = mkdp(104, TUYA_DP_BOOL, 0, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 1000), RADAR_CHG_PRESENCE);
    CHECK_EQ(r.presence, 0);

    dp = mkdp(1, TUYA_DP_ENUM, RADAR_STATE_MOVE, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_STATE);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);

    /* settings: first sighting is a change (mirror into attributes), repeats are not */
    dp = mkdp(3, TUYA_DP_VALUE, 75, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_RANGE_MIN);
    CHECK_EQ(r.range_min_cm, 75);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);
    dp = mkdp(4, TUYA_DP_VALUE, 300, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_RANGE_MAX);
    CHECK_EQ(r.range_max_cm, 300);
    dp = mkdp(105, TUYA_DP_VALUE, 15, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_TIMEOUT);
    dp = mkdp(103, TUYA_DP_VALUE, 5, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_ILLUMINANCE);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);

    /* unknown DP, oversized DP and missing variant are ignored */
    dp = mkdp(250, TUYA_DP_VALUE, 1, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);
    radar_t none;
    radar_init(&none, NULL, 10, 25);
    CHECK_EQ(radar_on_dp(&none, &dp, 0), 0);
}

/* Replays the DP 9 pattern captured from a real unit (docs/findings.md, "Why it floods"):
   30 frames in 45 s, values 12/13/14 dm, often 5 identical frames within one second. */
static void test_distance_off_by_default(void) {
    radar_t r = mkradar(0, 25);
    uint8_t st[8];
    int reports = 0;
    static const uint8_t seq[] = {12, 12, 12, 12, 12, 12, 12, 13, 13, 12, 12, 14, 14, 13, 13, 12};

    for (int k = 0; k < 40; k++) {
        tuya_dp_t dp = mkdp(9, TUYA_DP_VALUE, seq[k % sizeof(seq)], st);
        if (radar_on_dp(&r, &dp, (uint32_t)k * 1500) & RADAR_REPORT_DISTANCE) {
            reports++;
        }
        if (radar_tick(&r, (uint32_t)k * 1500 + 700) & RADAR_REPORT_DISTANCE) {
            reports++;
        }
    }
    CHECK_EQ(reports, 0);
    CHECK_EQ(r.distance_cm, seq[39 % sizeof(seq)] * 10);
}

static void test_distance_policy(void) {
    radar_t r = mkradar(10, 25);   /* at most every 10 s, only when moved >= 25 cm */
    uint8_t st[8];
    tuya_dp_t dp;

    /* first reading goes out at once */
    dp = mkdp(9, TUYA_DP_VALUE, 12, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_REPORT_DISTANCE);
    CHECK_EQ(r.dist_report_cm, 120);

    /* jitter 120..140 cm stays inside the deadband: nothing, ever */
    for (uint32_t t = 1000; t < 60000; t += 1000) {
        dp = mkdp(9, TUYA_DP_VALUE, 12 + (t / 1000) % 3, st);
        CHECK_EQ(radar_on_dp(&r, &dp, t) & RADAR_REPORT_DISTANCE, 0);
        CHECK_EQ(radar_tick(&r, t + 500), 0);
    }

    /* a real move (120 -> 300 cm) after the interval goes out immediately */
    dp = mkdp(9, TUYA_DP_VALUE, 30, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 61000), RADAR_REPORT_DISTANCE);
    CHECK_EQ(r.dist_report_cm, 300);

    /* moving again within the interval is held back... */
    dp = mkdp(9, TUYA_DP_VALUE, 10, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 62000), 0);
    CHECK(r.dist_pending);
    dp = mkdp(9, TUYA_DP_VALUE, 8, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 65000), 0);
    CHECK_EQ(radar_tick(&r, 70999), 0);
    /* ...and the latest value goes out once the interval has passed */
    CHECK_EQ(radar_tick(&r, 71000), RADAR_REPORT_DISTANCE);
    CHECK_EQ(r.dist_report_cm, 80);
    CHECK(!r.dist_pending);
    CHECK_EQ(radar_tick(&r, 90000), 0);

    /* a held-back change that drifts back into the deadband is cancelled */
    dp = mkdp(9, TUYA_DP_VALUE, 20, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 72000), 0);
    CHECK(r.dist_pending);
    dp = mkdp(9, TUYA_DP_VALUE, 9, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 73000), 0);
    CHECK(!r.dist_pending);
    CHECK_EQ(radar_tick(&r, 200000), 0);

    /* the MCU reports "no target" as -3: that is 0 cm, never 65535 */
    radar_t nt = mkradar(0, 25);
    dp = mkdp(9, TUYA_DP_VALUE, 0xFFFFFFFDu, st);
    radar_on_dp(&nt, &dp, 0);
    CHECK_EQ(nt.distance_cm, 0);

    /* target lost (-3 -> 0 cm) counts even if the step is smaller than the deadband */
    dp = mkdp(9, TUYA_DP_VALUE, 0xFFFFFFFDu, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 200000), RADAR_REPORT_DISTANCE);
    CHECK_EQ(r.dist_report_cm, 0);

    /* switching reporting off drops anything pending; back on sends the next reading at once */
    dp = mkdp(9, TUYA_DP_VALUE, 25, st);
    radar_on_dp(&r, &dp, 201000);
    CHECK(r.dist_pending);
    radar_set_distance_policy(&r, 0, 25);
    CHECK_EQ(radar_tick(&r, 300000), 0);
    radar_set_distance_policy(&r, 30, 50);
    dp = mkdp(9, TUYA_DP_VALUE, 25, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 300001), RADAR_REPORT_DISTANCE);

    /* millisecond counter wrap-around */
    radar_t w = mkradar(10, 25);
    dp = mkdp(9, TUYA_DP_VALUE, 10, st);
    CHECK_EQ(radar_on_dp(&w, &dp, 0xFFFFF000u), RADAR_REPORT_DISTANCE);
    dp = mkdp(9, TUYA_DP_VALUE, 50, st);
    CHECK_EQ(radar_on_dp(&w, &dp, 0x00000100u), 0);           /* 4.3 s later: held */
    CHECK_EQ(radar_tick(&w, 0x00001800u), RADAR_REPORT_DISTANCE); /* 10.2 s later */
}

/* the fastest setting: at most once a second, no deadband, but identical values still dropped */
static void test_distance_1s(void) {
    radar_t r = mkradar(1, 0);
    uint8_t st[8];
    tuya_dp_t dp;
    int reports = 0;
    /* the MCU's real pattern: ~1 frame/s, repeats of the same dm value */
    static const uint8_t dm[] = {12, 12, 12, 13, 13, 12, 12, 14, 14, 14, 13, 12};
    uint16_t last = 0xFFFF;

    for (unsigned k = 0; k < sizeof(dm); k++) {
        dp = mkdp(9, TUYA_DP_VALUE, dm[k], st);
        uint32_t t = k * 1000;
        uint16_t chg = radar_on_dp(&r, &dp, t) | radar_tick(&r, t + 999);
        if (chg & RADAR_REPORT_DISTANCE) {
            reports++;
            CHECK(r.dist_report_cm != last);      /* never the same value twice in a row */
            last = r.dist_report_cm;
        }
    }
    CHECK_EQ(reports, 6);                         /* 12,13,12,14,13,12: every change, no repeat */

    /* two changes inside one second: the second waits for the interval, then goes out */
    radar_t q = mkradar(1, 0);
    dp = mkdp(9, TUYA_DP_VALUE, 10, st);
    CHECK_EQ(radar_on_dp(&q, &dp, 0), RADAR_REPORT_DISTANCE);
    dp = mkdp(9, TUYA_DP_VALUE, 11, st);
    CHECK_EQ(radar_on_dp(&q, &dp, 300), 0);
    CHECK_EQ(radar_tick(&q, 999), 0);
    CHECK_EQ(radar_tick(&q, 1000), RADAR_REPORT_DISTANCE);
    CHECK_EQ(q.dist_report_cm, 110);
}

static void test_distance_calibration(void) {
    radar_t r = mkradar(1, 0);
    uint8_t st[8];
    tuya_dp_t dp;

    /* neutral by default */
    CHECK_EQ(r.dist_offset_cm, 0);
    CHECK_EQ(r.dist_scale_pct, 100);
    dp = mkdp(9, TUYA_DP_VALUE, 12, st);
    radar_on_dp(&r, &dp, 0);
    CHECK_EQ(r.distance_cm, 120);

    /* undo the MCU's -30 cm */
    radar_set_distance_calibration(&r, 30, 100);
    dp = mkdp(9, TUYA_DP_VALUE, 12, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 5000), RADAR_REPORT_DISTANCE);   /* same raw, new value: reported */
    CHECK_EQ(r.dist_report_cm, 150);

    /* scale + offset: 200 cm * 110 % - 20 = 200 */
    radar_set_distance_calibration(&r, -20, 110);
    dp = mkdp(9, TUYA_DP_VALUE, 20, st);
    radar_on_dp(&r, &dp, 10000);
    CHECK_EQ(r.distance_cm, 200);

    /* no target stays 0 whatever the offset; a present target never reads 0 */
    radar_set_distance_calibration(&r, 300, 200);
    dp = mkdp(9, TUYA_DP_VALUE, 0xFFFFFFFDu, st);
    radar_on_dp(&r, &dp, 20000);
    CHECK_EQ(r.distance_cm, 0);
    radar_set_distance_calibration(&r, -300, 50);
    dp = mkdp(9, TUYA_DP_VALUE, 0, st);                 /* closer than ~25 cm */
    radar_on_dp(&r, &dp, 30000);
    CHECK_EQ(r.distance_cm, 1);
    dp = mkdp(9, TUYA_DP_VALUE, 0xFFFFFFFEu, st);       /* -2: also a target */
    radar_on_dp(&r, &dp, 40000);
    CHECK_EQ(r.distance_cm, 1);

    /* clamps */
    radar_set_distance_calibration(&r, 30000, 1);
    CHECK_EQ(r.dist_offset_cm, 300);
    CHECK_EQ(r.dist_scale_pct, 50);
    radar_set_distance_calibration(&r, -30000, 60000);
    CHECK_EQ(r.dist_offset_cm, -300);
    CHECK_EQ(r.dist_scale_pct, 200);

    /* corrupt huge value: no overflow, saturates */
    radar_set_distance_calibration(&r, 300, 200);
    dp = mkdp(9, TUYA_DP_VALUE, 0x7FFFFFFF, st);
    radar_on_dp(&r, &dp, 50000);
    CHECK_EQ(r.distance_cm, 0xFFFF);
}

static void test_settings_out(void) {
    radar_t r = mkradar(0, 25);
    uint8_t buf[16];
    tuya_dp_t dp;
    uint16_t off;

    CHECK_EQ(radar_build_setting(&r, RADAR_SET_RANGE_MAX, 450, buf, sizeof(buf)), 8);
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(dp.id, 4);
    CHECK_EQ(dp.type, TUYA_DP_VALUE);
    CHECK_EQ(tuya_dp_u32(&dp), 450);    /* cm on the wire for this variant */

    /* clamped to the variant's limits */
    radar_build_setting(&r, RADAR_SET_MOVE_SENS, 99, buf, sizeof(buf));
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(dp.id, 2);
    CHECK_EQ(tuya_dp_u32(&dp), 10);
    radar_build_setting(&r, RADAR_SET_TIMEOUT, 0, buf, sizeof(buf));
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(dp.id, 105);
    CHECK_EQ(tuya_dp_u32(&dp), 1);
    radar_build_setting(&r, RADAR_SET_RANGE_MIN, 5000, buf, sizeof(buf));
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(tuya_dp_u32(&dp), 1000);

    CHECK_EQ(radar_build_setting(&r, RADAR_SET_PRES_SENS, 3, buf, 7), 0);   /* no room */
    radar_t none;
    radar_init(&none, NULL, 0, 0);
    CHECK_EQ(radar_build_setting(&none, RADAR_SET_PRES_SENS, 3, buf, sizeof(buf)), 0);
}

static void test_invalidate(void) {
    radar_t r = mkradar(0, 25);
    uint8_t st[8];
    tuya_dp_t dp = mkdp(4, TUYA_DP_VALUE, 450, st);

    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_RANGE_MAX);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);
    radar_invalidate(&r, RADAR_CHG_RANGE_MAX);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_RANGE_MAX);     /* same value, still applied */
}

static void test_lux(void) {
    /* reference: round(10000*log10(lux) + 1) */
    struct { uint32_t lux; uint16_t zcl; } t[] = {
        {0, 0}, {1, 1}, {2, 3011}, {5, 6990}, {10, 10001}, {100, 20001},
        {1000, 30001}, {12345, 40916}, {65535, 48166}, {100000, 50001},
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int got = radar_lux_to_zcl(t[i].lux);
        int err = got - t[i].zcl;
        if (err < 0) err = -err;
        checks++;
        if (err > 3) {
            fails++;
            printf("FAIL lux %u -> %d, expected %u\n", t[i].lux, got, t[i].zcl);
        }
    }
    /* 10000*log10(2^32) + 1 = 96330, beyond uint16: clamped to the ZCL maximum */
    CHECK_EQ(radar_lux_to_zcl(0xFFFFFFFFu), 0xFFFE);
}

static void test_v3(void) {
    radar_t r;
    uint8_t st[8], buf[16];
    tuya_dp_t dp;
    uint16_t off;

    CHECK(radar_variant_find("ya4ft0w4") != NULL);
    radar_init(&r, radar_variant_find("ya4ft0w4"), 0, 25);

    /* DP104 is a "time since target" enum on this MCU, never presence */
    dp = mkdp(104, TUYA_DP_ENUM, 3, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), 0);

    /* presence follows the state DP */
    dp = mkdp(1, TUYA_DP_ENUM, RADAR_STATE_MOVE, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_STATE | RADAR_CHG_PRESENCE);
    CHECK_EQ(r.presence, 1);
    dp = mkdp(1, TUYA_DP_ENUM, RADAR_STATE_PRESENCE, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_STATE);
    CHECK_EQ(r.presence, 1);
    dp = mkdp(1, TUYA_DP_ENUM, RADAR_STATE_NONE, st);
    CHECK_EQ(radar_on_dp(&r, &dp, 0), RADAR_CHG_STATE | RADAR_CHG_PRESENCE);
    CHECK_EQ(r.presence, 0);

    /* distance in dm, 0 = no target */
    dp = mkdp(9, TUYA_DP_VALUE, 17, st);
    radar_on_dp(&r, &dp, 0);
    CHECK_EQ(r.distance_cm, 170);
    dp = mkdp(9, TUYA_DP_VALUE, 0, st);
    radar_on_dp(&r, &dp, 1000);
    CHECK_EQ(r.distance_cm, 0);

    /* sensitivity 0 is valid, timeout goes to 15000 s */
    radar_build_setting(&r, RADAR_SET_MOVE_SENS, 0, buf, sizeof(buf));
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(dp.id, 2);
    CHECK_EQ(tuya_dp_u32(&dp), 0);
    radar_build_setting(&r, RADAR_SET_TIMEOUT, 20000, buf, sizeof(buf));
    off = 0;
    tuya_dp_next(buf, 8, &off, &dp);
    CHECK_EQ(dp.id, 105);
    CHECK_EQ(tuya_dp_u32(&dp), 15000);
}

int main(void) {
    test_build_and_parse();
    test_parser_resync();
    test_dp_codec();
    test_product_id();
    test_duplicates_dropped();
    test_distance_off_by_default();
    test_distance_policy();
    test_distance_1s();
    test_distance_calibration();
    test_settings_out();
    test_invalidate();
    test_lux();
    test_v3();

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
