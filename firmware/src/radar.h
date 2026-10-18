/*
 * Radar model: turns the GD32's DP reports into attribute values and decides what is worth
 * sending over Zigbee. This is where the flood gets cut:
 *  - identical values are dropped (the stock module forwarded every repeat),
 *  - target distance only goes out when reporting is enabled, has moved by the deadband and
 *    the minimum interval has passed (the latest value is sent once the interval expires).
 *
 * Pure C with no SDK dependencies (host unit tests). Time is passed in as milliseconds.
 */
#ifndef RADAR_H_
#define RADAR_H_

#include "tuya_mcu.h"

/* Bits returned by radar_on_dp() / radar_tick() */
#define RADAR_CHG_PRESENCE      0x0001
#define RADAR_CHG_STATE         0x0002
#define RADAR_CHG_ILLUMINANCE   0x0004
#define RADAR_CHG_MOVE_SENS     0x0008
#define RADAR_CHG_PRES_SENS     0x0010
#define RADAR_CHG_RANGE_MIN     0x0020
#define RADAR_CHG_RANGE_MAX     0x0040
#define RADAR_CHG_TIMEOUT       0x0080
#define RADAR_REPORT_DISTANCE   0x0100  /* send dist_report_cm now */
#define RADAR_CHG_SETTINGS      (RADAR_CHG_MOVE_SENS | RADAR_CHG_PRES_SENS | RADAR_CHG_RANGE_MIN | \
                                 RADAR_CHG_RANGE_MAX | RADAR_CHG_TIMEOUT)

#define RADAR_STATE_NONE        0
#define RADAR_STATE_PRESENCE    1
#define RADAR_STATE_MOVE        2

#define RADAR_DP_NONE           0

#define RADAR_DIST_OFFSET_MAX   300     /* cm, either way */
#define RADAR_DIST_SCALE_MIN    50      /* % */
#define RADAR_DIST_SCALE_MAX    200
#define RADAR_DIST_SCALE_DEF    100

/* Which settings a ZCL write maps to */
typedef enum {
    RADAR_SET_MOVE_SENS,
    RADAR_SET_PRES_SENS,
    RADAR_SET_RANGE_MIN,        /* cm */
    RADAR_SET_RANGE_MAX,        /* cm */
    RADAR_SET_TIMEOUT,          /* s */
} radar_setting_t;

/* DP map of one hardware variant, keyed by the product ID the GD32 reports */
typedef struct {
    const char *pid;
    uint8_t dp_state;           /* enum none/presence/move */
    uint8_t dp_presence;        /* bool */
    uint8_t dp_distance;        /* value */
    uint8_t dp_illuminance;     /* value, lux */
    uint8_t dp_move_sens;       /* value */
    uint8_t dp_pres_sens;       /* value */
    uint8_t dp_range_min;       /* value */
    uint8_t dp_range_max;       /* value */
    uint8_t dp_timeout;         /* value, s */
    uint8_t distance_mm;        /* mm per raw distance unit */
    int8_t  distance_none;      /* raw distance at or below this = no target */
    uint8_t range_mm;           /* mm per raw range unit */
    uint8_t sens_max;
    uint16_t range_max_cm;
    uint16_t timeout_min_s;
    uint16_t timeout_max_s;
} radar_variant_t;

typedef struct {
    const radar_variant_t *v;

    /* live values; *_known is set once the MCU has reported the DP */
    uint8_t  presence;
    uint8_t  state;
    uint16_t distance_cm;
    uint16_t illuminance_lux;
    uint8_t  move_sens;
    uint8_t  pres_sens;
    uint16_t range_min_cm;
    uint16_t range_max_cm;
    uint16_t timeout_s;
    uint16_t known;             /* RADAR_CHG_* bits of values seen at least once */

    /* module-side settings (kept in NV by the glue) */
    uint16_t dist_interval_s;   /* 0 = never report distance */
    uint16_t dist_deadband_cm;
    int16_t  dist_offset_cm;    /* calibration: cm = raw_cm * scale / 100 + offset */
    uint16_t dist_scale_pct;

    /* distance report bookkeeping */
    uint8_t  dist_sent;         /* dist_report_cm is valid */
    uint8_t  dist_pending;      /* a change is waiting for the interval to pass */
    uint16_t dist_report_cm;    /* last value sent / to send */
    uint32_t dist_sent_ms;

    uint32_t dp_frames;         /* statistics */
    uint32_t dp_dropped;        /* DP units that changed nothing */
} radar_t;

const radar_variant_t *radar_variant_find(const char *pid);

void     radar_init(radar_t *r, const radar_variant_t *v, uint16_t dist_interval_s, uint16_t dist_deadband_cm);
uint16_t radar_on_dp(radar_t *r, const tuya_dp_t *dp, uint32_t now_ms);
uint16_t radar_tick(radar_t *r, uint32_t now_ms);
void     radar_set_distance_policy(radar_t *r, uint16_t interval_s, uint16_t deadband_cm);
/* Calibration of the reported distance, clamped to RADAR_DIST_OFFSET_MAX / RADAR_DIST_SCALE_*. */
void     radar_set_distance_calibration(radar_t *r, int16_t offset_cm, uint16_t scale_pct);
/* Forget the given RADAR_CHG_* values so the MCU's next report of them counts as a change.
 * Used after a Zigbee write: the attribute already holds the requested value, and the MCU's echo
 * (possibly rounded or rejected) must overwrite it even if it equals what we had before. */
void     radar_invalidate(radar_t *r, uint16_t bits);

/* Turn a setting written over Zigbee into a DP unit for cmd 0x04 (value is clamped to what the
 * variant accepts). Returns bytes written, 0 if the variant has no such DP. */
size_t   radar_build_setting(const radar_t *r, radar_setting_t s, uint32_t value, uint8_t *buf, size_t cap);

/* ZCL illuminance MeasuredValue = 10000 * log10(lux) + 1, 0 for 0 lux */
uint16_t radar_lux_to_zcl(uint32_t lux);

#endif /* RADAR_H_ */
