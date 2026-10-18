#include "tl_common.h"
#include "zcl_include.h"

#include "app_main.h"

/*
 * Behaviour of the radar MCU this is built around (RE of the 7gclukjs GD32 firmware,
 * docs/gd32-7gclukjs-protocol.md):
 *  - 115200 8N1, version byte 0x02, every DP report is cmd 0x06 with one DP, no ACK expected;
 *  - it sends `0x03 01` (pairing request) when a key is held, when it has been "not joined" for
 *    ~10 s, and when the status drops 1 -> 0. So once we have a network we keep saying "joined"
 *    while rejoining, otherwise the MCU would make us leave;
 *  - it pings with an empty 0x02 every second while the status isn't 1, and acks our 0x02 with
 *    an empty 0x02: answering every one of those would ping-pong, so status sends are rate limited;
 *  - never send it 0x0C/0x0D/0x0E: an MCU-OTA would stage an image it cannot install and leave it
 *    in a reset loop until power-cycled.
 */

#define NV_RADAR_MAGIC          0x5244
#define PROBE_INTERVAL_MS       1000
#define PROBE_PER_BAUD          3
#define ALIVE_CHECK_MS          (5 * 60 * 1000)
#define ALIVE_MISSES            3
#define STATUS_MIN_GAP_MS       2000
#define TX_GAP_MS               20
#define TXQ_LEN                 8
#define TXQ_FRAME_MAX           32
#define RX_GAP_RESET_MS         50      /* a frame never pauses this long between bytes */

typedef struct {
    uint16_t magic;
    uint16_t dist_interval_s;
    uint16_t dist_deadband_cm;
    uint16_t reserved;
} radar_nv_t;

typedef struct {
    uint8_t len;
    uint8_t buf[TXQ_FRAME_MAX];
} txq_item_t;

static tuya_parser_t parser;
static radar_t radar;
static char pid[16];

static uint8_t  baud_locked;
static uint8_t  probes;
static uint32_t probe_at_ms;
static uint8_t  alive_misses;
static uint8_t  alive_pending;
static uint32_t alive_at_ms;

static uint16_t seq;
static uint8_t  net_status = TUYA_NET_NOT_JOINED;
static uint32_t status_sent_ms;
static uint8_t  status_sent_once;

static txq_item_t txq[TXQ_LEN];
static uint8_t  txq_head, txq_tail;
static uint32_t tx_last_ms;

static uint32_t ms_now;
static uint32_t ms_tick;
static uint32_t rx_last_ms;
static uint8_t  leave_pending;

static uint16_t policy_interval_s = RADAR_DEFAULT_DIST_INTERVAL;
static uint16_t policy_deadband_cm = RADAR_DEFAULT_DIST_DEADBAND;

/* distance calibration: its own NV item, so the v1.0.01/02 policy item stays readable */
#define NV_CAL_MAGIC            0x4341
typedef struct {
    uint16_t magic;
    int16_t  offset_cm;
    uint16_t scale_pct;
    uint16_t reserved;
} radar_cal_nv_t;

static int16_t  cal_offset_cm = 0;
static uint16_t cal_scale_pct = RADAR_DIST_SCALE_DEF;

/* ---------------------------------------------------------------- time */

static uint32_t now_ms(void) {
    uint32_t t = clock_time();
    uint32_t d = (t - ms_tick) / (1000 * sys_tick_per_us);

    ms_now += d;
    ms_tick += d * 1000 * sys_tick_per_us;
    return ms_now;
}

/* ---------------------------------------------------------------- NV */

static void policy_load(void) {
    radar_nv_t nv;

    if (nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_USER_CFG, sizeof(nv), (uint8_t *)&nv) == NV_SUCC &&
        nv.magic == NV_RADAR_MAGIC) {
        policy_interval_s = nv.dist_interval_s > RADAR_MAX_DIST_INTERVAL ? RADAR_MAX_DIST_INTERVAL : nv.dist_interval_s;
        policy_deadband_cm = nv.dist_deadband_cm > RADAR_MAX_DIST_DEADBAND ? RADAR_MAX_DIST_DEADBAND : nv.dist_deadband_cm;
    }
    g_zcl_radarAttrs.dist_interval_s = policy_interval_s;
    g_zcl_radarAttrs.dist_deadband_cm = policy_deadband_cm;
}

static void calibration_apply(void) {
    radar_set_distance_calibration(&radar, cal_offset_cm, cal_scale_pct);
    /* show the clamped values */
    cal_offset_cm = radar.dist_offset_cm;
    cal_scale_pct = radar.dist_scale_pct;
    g_zcl_radarAttrs.dist_offset_cm = cal_offset_cm;
    g_zcl_radarAttrs.dist_scale_pct = cal_scale_pct;
}

static void calibration_load(void) {
    radar_cal_nv_t nv;

    if (nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_DIST_CAL, sizeof(nv), (uint8_t *)&nv) == NV_SUCC &&
        nv.magic == NV_CAL_MAGIC) {
        cal_offset_cm = nv.offset_cm;
        cal_scale_pct = nv.scale_pct;
    }
}

static void calibration_save(void) {
    radar_cal_nv_t old;
    radar_cal_nv_t nv = {
        .magic = NV_CAL_MAGIC,
        .offset_cm = cal_offset_cm,
        .scale_pct = cal_scale_pct,
        .reserved = 0,
    };

    if (nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_DIST_CAL, sizeof(old), (uint8_t *)&old) == NV_SUCC &&
        old.magic == nv.magic && old.offset_cm == nv.offset_cm && old.scale_pct == nv.scale_pct) {
        return;
    }
    nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_DIST_CAL, sizeof(nv), (uint8_t *)&nv);
}

static void policy_save(void) {
    radar_nv_t old;
    radar_nv_t nv = {
        .magic = NV_RADAR_MAGIC,
        .dist_interval_s = policy_interval_s,
        .dist_deadband_cm = policy_deadband_cm,
        .reserved = 0,
    };

    /* rewriting the same values on every Zigbee write would only wear the flash */
    if (nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_USER_CFG, sizeof(old), (uint8_t *)&old) == NV_SUCC &&
        old.magic == nv.magic && old.dist_interval_s == nv.dist_interval_s &&
        old.dist_deadband_cm == nv.dist_deadband_cm) {
        return;
    }
    nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_USER_CFG, sizeof(nv), (uint8_t *)&nv);
}

/* ---------------------------------------------------------------- UART out */

static uint16_t next_seq(void) {
    seq = (seq >= 0xFFF0) ? 0 : seq + 1;
    return seq;
}

static void send_frame(uint16_t fseq, uint8_t cmd, const uint8_t *data, uint16_t len) {
    uint8_t next = (txq_head + 1) % TXQ_LEN;
    txq_item_t *it = &txq[txq_head];

    if (next == txq_tail) {
        return;     /* queue full: the MCU is not listening anyway */
    }

    it->len = (uint8_t)tuya_build(it->buf, sizeof(it->buf), fseq, cmd, data, len);
    if (it->len) {
        txq_head = next;
    }
}

static void txq_pump(uint32_t t) {
    if (txq_tail == txq_head || (uint32_t)(t - tx_last_ms) < TX_GAP_MS) {
        return;
    }
    if (app_uart_tx(txq[txq_tail].buf, txq[txq_tail].len)) {
        txq_tail = (txq_tail + 1) % TXQ_LEN;
        tx_last_ms = t;
    }
}

/* What we tell the MCU. A device that has a network is always "joined", whatever the stack is
 * doing (rejoin, init failure): a 1 -> 0 drop makes the MCU ask us to leave (see top of file). */
static uint8_t effective_status(void) {
    return zb_isDeviceFactoryNew() ? net_status : TUYA_NET_JOINED;
}

static void send_status(uint32_t t) {
    uint8_t st = effective_status();
    send_frame(next_seq(), TUYA_CMD_NET_STATUS, &st, 1);
    status_sent_ms = t;
    status_sent_once = 1;
}

/* ---------------------------------------------------------------- ZCL side */

static void report_distance(void) {
    epInfo_t dst;

    g_zcl_radarAttrs.distance_cm = radar.dist_report_cm;

    if (!zb_isDeviceJoinedNwk()) {
        return;
    }

    memset((uint8_t *)&dst, 0, sizeof(dst));
    dst.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
    dst.dstAddr.shortAddr = 0x0000;
    dst.dstEp = APP_ENDPOINT1;
    dst.profileId = HA_PROFILE_ID;

    zcl_sendReportCmd(APP_ENDPOINT1, &dst, TRUE, ZCL_FRAME_SERVER_CLIENT_DIR,
                      ZCL_CLUSTER_MS_OCCUPANCY_SENSING, ATTRID_RADAR_DISTANCE,
                      ZCL_DATA_TYPE_UINT16, (uint8_t *)&g_zcl_radarAttrs.distance_cm);
}

static void apply(uint16_t chg) {
    if (chg & RADAR_CHG_PRESENCE) {
        g_zcl_radarAttrs.occupancy = radar.presence ? 1 : 0;
    }
    if (chg & RADAR_CHG_STATE) {
        g_zcl_radarAttrs.state = radar.state;
    }
    if (chg & RADAR_CHG_ILLUMINANCE) {
        g_zcl_illuminanceAttrs.measuredValue = radar_lux_to_zcl(radar.illuminance_lux);
    }
    if (chg & RADAR_CHG_MOVE_SENS) {
        g_zcl_radarAttrs.move_sens = radar.move_sens;
    }
    if (chg & RADAR_CHG_PRES_SENS) {
        g_zcl_radarAttrs.pres_sens = radar.pres_sens;
    }
    if (chg & RADAR_CHG_RANGE_MIN) {
        g_zcl_radarAttrs.range_min_cm = radar.range_min_cm;
    }
    if (chg & RADAR_CHG_RANGE_MAX) {
        g_zcl_radarAttrs.range_max_cm = radar.range_max_cm;
    }
    if (chg & RADAR_CHG_TIMEOUT) {
        g_zcl_radarAttrs.timeout_s = radar.timeout_s;
    }
    if (chg & RADAR_REPORT_DISTANCE) {
        report_distance();
    }
}

void app_radar_attr_written(uint16_t attrId) {
    uint8_t dp[8];
    size_t n = 0;

    switch (attrId) {
        case ATTRID_RADAR_DIST_INTERVAL:
        case ATTRID_RADAR_DIST_DEADBAND:
            policy_interval_s = g_zcl_radarAttrs.dist_interval_s > RADAR_MAX_DIST_INTERVAL ?
                                RADAR_MAX_DIST_INTERVAL : g_zcl_radarAttrs.dist_interval_s;
            policy_deadband_cm = g_zcl_radarAttrs.dist_deadband_cm > RADAR_MAX_DIST_DEADBAND ?
                                 RADAR_MAX_DIST_DEADBAND : g_zcl_radarAttrs.dist_deadband_cm;
            g_zcl_radarAttrs.dist_interval_s = policy_interval_s;
            g_zcl_radarAttrs.dist_deadband_cm = policy_deadband_cm;
            radar_set_distance_policy(&radar, policy_interval_s, policy_deadband_cm);
            policy_save();
            return;
        case ATTRID_RADAR_DIST_OFFSET:
        case ATTRID_RADAR_DIST_SCALE:
            cal_offset_cm = g_zcl_radarAttrs.dist_offset_cm;
            cal_scale_pct = g_zcl_radarAttrs.dist_scale_pct;
            calibration_apply();
            calibration_save();
            return;
        case ATTRID_RADAR_MOVE_SENS:
            n = radar_build_setting(&radar, RADAR_SET_MOVE_SENS, g_zcl_radarAttrs.move_sens, dp, sizeof(dp));
            radar_invalidate(&radar, RADAR_CHG_MOVE_SENS);
            break;
        case ATTRID_RADAR_PRES_SENS:
            n = radar_build_setting(&radar, RADAR_SET_PRES_SENS, g_zcl_radarAttrs.pres_sens, dp, sizeof(dp));
            radar_invalidate(&radar, RADAR_CHG_PRES_SENS);
            break;
        case ATTRID_RADAR_RANGE_MIN:
            n = radar_build_setting(&radar, RADAR_SET_RANGE_MIN, g_zcl_radarAttrs.range_min_cm, dp, sizeof(dp));
            radar_invalidate(&radar, RADAR_CHG_RANGE_MIN);
            break;
        case ATTRID_RADAR_RANGE_MAX:
            n = radar_build_setting(&radar, RADAR_SET_RANGE_MAX, g_zcl_radarAttrs.range_max_cm, dp, sizeof(dp));
            radar_invalidate(&radar, RADAR_CHG_RANGE_MAX);
            break;
        case ATTRID_RADAR_TIMEOUT:
            n = radar_build_setting(&radar, RADAR_SET_TIMEOUT, g_zcl_radarAttrs.timeout_s, dp, sizeof(dp));
            radar_invalidate(&radar, RADAR_CHG_TIMEOUT);
            break;
        default:
            return;
    }

    if (n) {
        /* The MCU echoes the value it actually took as a 0x06 report, which corrects the
         * attribute if it rounded or rejected the write. */
        send_frame(next_seq(), TUYA_CMD_DP_WRITE, dp, (uint16_t)n);
    }
}

/* ---------------------------------------------------------------- UART in */

static int32_t leave_and_pairCb(void *arg) {
    (void)arg;
    zb_factoryReset();      /* leave; app_leaveCnfHandler starts steering again */
    leave_pending = 0;
    return -1;
}

static void on_frame(const tuya_frame_t *f, uint32_t t) {
    switch (f->cmd) {
        case TUYA_CMD_PRODUCT_INFO: {
            const radar_variant_t *v;

            baud_locked = 1;
            alive_misses = 0;
            alive_pending = 0;
            alive_at_ms = t;
            tuya_product_id(f->data, f->len, pid, sizeof(pid));
            v = radar_variant_find(pid);
            if (v != radar.v) {
                radar_init(&radar, v, policy_interval_s, policy_deadband_cm);
                calibration_apply();
            }
            send_status(t);
            if (radar.v) {
                send_frame(next_seq(), TUYA_CMD_DP_QUERY, NULL, 0);
            }
            break;
        }
        case TUYA_CMD_NET_STATUS:
            /* empty ping (not joined yet) or the ack of our own 0x02: answer at most every 2 s */
            if (baud_locked && (uint32_t)(t - status_sent_ms) >= STATUS_MIN_GAP_MS) {
                send_status(t);
            }
            break;
        case TUYA_CMD_NET_RESET:
            send_frame(f->seq, TUYA_CMD_NET_RESET, NULL, 0);
            /* 0x00 is sent at every MCU boot: ignore. 0x01 is a pairing request. */
            if (f->len >= 1 && f->data[0] == 0x01) {
                /* We never report "not joined" once on a network, so with a network this can only
                 * be the user holding the key: leave and pair again. Not while commissioning is
                 * still running (the MCU's own one-shot request can land mid-join), and once. */
                if (zb_isDeviceJoinedNwk() && !zb_isDeviceFactoryNew() && bdb_isIdle() && !leave_pending) {
                    leave_pending = 1;
                    TL_ZB_TIMER_SCHEDULE(leave_and_pairCb, NULL, 500);
                }
                /* not joined: steering is already running (zb_appCb.c) */
            }
            break;
        case TUYA_CMD_DP_REPORT:
        case TUYA_CMD_DP_REPORT_ASYNC: {
            uint8_t ok = 1;
            uint16_t off = 0;
            tuya_dp_t dp;

            send_frame(f->seq, f->cmd, &ok, 1);
            while (tuya_dp_next(f->data, f->len, &off, &dp)) {
                apply(radar_on_dp(&radar, &dp, t));
            }
            break;
        }
        default:
            /* 0x0B version, 0x0C-0x0E MCU-OTA and anything else: never answered */
            break;
    }
}

/* ---------------------------------------------------------------- link supervision */

static void probe(uint32_t t) {
    if (baud_locked) {
        /* every few minutes check the MCU still answers; after a few misses search again */
        if ((uint32_t)(t - alive_at_ms) < ALIVE_CHECK_MS) {
            return;
        }
        alive_at_ms = t;
        if (alive_pending && ++alive_misses >= ALIVE_MISSES) {
            baud_locked = 0;
            probes = 0;
            return;
        }
        alive_pending = 1;
        send_frame(next_seq(), TUYA_CMD_PRODUCT_INFO, NULL, 0);
        return;
    }

    if ((uint32_t)(t - probe_at_ms) < PROBE_INTERVAL_MS) {
        return;
    }
    probe_at_ms = t;

    /* the 7gclukjs MCU runs at 115200; the Tuya spec also allows 9600 */
    if (probes && probes % PROBE_PER_BAUD == 0) {
        app_uart_init(app_uart_baudrate() == UART_BAUDRATE_115200 ? UART_BAUDRATE_9600 : UART_BAUDRATE_115200);
        tuya_parser_reset(&parser);
    }
    probes++;
    send_frame(next_seq(), TUYA_CMD_PRODUCT_INFO, NULL, 0);
}

/* ---------------------------------------------------------------- public */

void app_radar_init(void) {
    ms_tick = clock_time();
    ms_now = 0;

    policy_load();
    calibration_load();
    radar_init(&radar, NULL, policy_interval_s, policy_deadband_cm);
    calibration_apply();
    tuya_parser_reset(&parser);
    app_uart_init(UART_BAUDRATE_115200);

    g_zcl_radarAttrs.sensorType = 0x00;
    g_zcl_illuminanceAttrs.minMeasuredValue = 1;
    g_zcl_illuminanceAttrs.maxMeasuredValue = 0xFFFE;
}

void app_radar_net_status(uint8_t status) {
    if (status == net_status && status_sent_once) {
        return;
    }
    net_status = status;
    status = effective_status();
    if (baud_locked) {
        send_status(now_ms());
    }
}

void app_radar_task(void) {
    uint32_t t = now_ms();
    uint8_t b;
    uint16_t budget = 256;

    app_uart_poll();

    /* a frame cut short (MCU reset, line noise) must not swallow the next ones */
    if (tuya_parser_busy(&parser) && (uint32_t)(t - rx_last_ms) > RX_GAP_RESET_MS) {
        tuya_parser_reset(&parser);
    }

    while (budget-- && app_uart_rx_byte(&b)) {
        rx_last_ms = t;
        if (tuya_parser_feed(&parser, b)) {
            alive_pending = 0;
            alive_misses = 0;
            on_frame(&parser.frame, t);
        }
    }

    apply(radar_tick(&radar, t));
    probe(t);
    txq_pump(t);
}
