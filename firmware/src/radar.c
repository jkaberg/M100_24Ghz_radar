#include "radar.h"

/*
 * ZY-M100-24GV2 (_TZE204_7gclukjs). DP numbers and scaling match the z2m definition running on
 * a real unit and the RE of its MCU firmware (docs/gd32-7gclukjs-protocol.md):
 * distance in dm (signed, -3 = no target), ranges in cm (75 cm gates, <= 1000), timeout in s.
 * The MCU computes distance as (cm + 5) / 10 - 3, i.e. 10 cm steps with 30 cm taken off; the
 * module-side calibration (offset/scale) is there to correct for that and for mounting.
 */
static const radar_variant_t variants[] = {
    {
        .pid            = "7gclukjs",
        .dp_state       = 1,
        .dp_presence    = 104,
        .dp_distance    = 9,
        .dp_illuminance = 103,
        .dp_move_sens   = 2,
        .dp_pres_sens   = 102,
        .dp_range_min   = 3,
        .dp_range_max   = 4,
        .dp_timeout     = 105,
        .distance_mm    = 100,
        .distance_none  = -3,
        .range_mm       = 10,
        .sens_max       = 10,
        .range_max_cm   = 1000,
        .timeout_min_s  = 1,
        .timeout_max_s  = 1500,
    },
};

const radar_variant_t *radar_variant_find(const char *pid) {
    for (unsigned i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        const char *a = variants[i].pid;
        const char *b = pid;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return &variants[i];
        }
    }
    return 0;
}

void radar_init(radar_t *r, const radar_variant_t *v, uint16_t dist_interval_s, uint16_t dist_deadband_cm) {
    uint8_t *p = (uint8_t *)r;
    for (unsigned i = 0; i < sizeof(*r); i++) {
        p[i] = 0;
    }
    r->v = v;
    r->dist_interval_s = dist_interval_s;
    r->dist_deadband_cm = dist_deadband_cm;
    r->dist_scale_pct = RADAR_DIST_SCALE_DEF;
}

void radar_set_distance_calibration(radar_t *r, int16_t offset_cm, uint16_t scale_pct) {
    if (offset_cm > RADAR_DIST_OFFSET_MAX) {
        offset_cm = RADAR_DIST_OFFSET_MAX;
    } else if (offset_cm < -RADAR_DIST_OFFSET_MAX) {
        offset_cm = -RADAR_DIST_OFFSET_MAX;
    }
    if (scale_pct < RADAR_DIST_SCALE_MIN) {
        scale_pct = RADAR_DIST_SCALE_MIN;
    } else if (scale_pct > RADAR_DIST_SCALE_MAX) {
        scale_pct = RADAR_DIST_SCALE_MAX;
    }
    r->dist_offset_cm = offset_cm;
    r->dist_scale_pct = scale_pct;
    /* the next reading goes out with the new calibration */
    r->dist_sent = 0;
    r->dist_pending = 0;
}

void radar_set_distance_policy(radar_t *r, uint16_t interval_s, uint16_t deadband_cm) {
    r->dist_interval_s = interval_s;
    r->dist_deadband_cm = deadband_cm;
    r->dist_pending = 0;
    /* start fresh: the next reading after enabling goes out right away */
    r->dist_sent = 0;
}

void radar_invalidate(radar_t *r, uint16_t bits) {
    r->known &= ~bits;
}

static uint16_t absdiff(uint16_t a, uint16_t b) {
    return a > b ? a - b : b - a;
}

static int interval_passed(const radar_t *r, uint32_t now_ms) {
    return !r->dist_sent || (uint32_t)(now_ms - r->dist_sent_ms) >= (uint32_t)r->dist_interval_s * 1000;
}

static uint16_t distance_send(radar_t *r, uint32_t now_ms) {
    r->dist_report_cm = r->distance_cm;
    r->dist_sent = 1;
    r->dist_sent_ms = now_ms;
    r->dist_pending = 0;
    return RADAR_REPORT_DISTANCE;
}

static uint16_t distance_update(radar_t *r, uint32_t now_ms) {
    if (r->dist_interval_s == 0) {
        return 0;
    }

    if (r->dist_sent) {
        /* target appearing or disappearing (0 <-> non-zero) always counts as a change */
        int zero_flip = (r->distance_cm == 0) != (r->dist_report_cm == 0);

        /* back inside the deadband: nothing to say, and any pending report is cancelled */
        if (!zero_flip && absdiff(r->distance_cm, r->dist_report_cm) < r->dist_deadband_cm) {
            r->dist_pending = 0;
            return 0;
        }
    }

    if (interval_passed(r, now_ms)) {
        return distance_send(r, now_ms);
    }

    r->dist_pending = 1;
    return 0;
}

uint16_t radar_tick(radar_t *r, uint32_t now_ms) {
    if (r->dist_pending && r->dist_interval_s && interval_passed(r, now_ms)) {
        return distance_send(r, now_ms);
    }
    return 0;
}

static uint16_t set_u8(uint8_t *field, uint32_t v, uint16_t bit, radar_t *r) {
    uint8_t nv = v > 0xFF ? 0xFF : (uint8_t)v;
    if ((r->known & bit) && *field == nv) {
        return 0;
    }
    *field = nv;
    r->known |= bit;
    return bit;
}

static uint16_t set_u16(uint16_t *field, uint32_t v, uint16_t bit, radar_t *r) {
    uint16_t nv = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
    if ((r->known & bit) && *field == nv) {
        return 0;
    }
    *field = nv;
    r->known |= bit;
    return bit;
}

uint16_t radar_on_dp(radar_t *r, const tuya_dp_t *dp, uint32_t now_ms) {
    const radar_variant_t *v = r->v;
    uint16_t chg = 0;
    uint32_t val;

    if (!v || dp->len == 0 || dp->len > 4) {
        return 0;
    }

    r->dp_frames++;
    val = tuya_dp_u32(dp);

    if (dp->id == v->dp_presence) {
        chg = set_u8(&r->presence, val ? 1 : 0, RADAR_CHG_PRESENCE, r);
    } else if (dp->id == v->dp_state) {
        chg = set_u8(&r->state, val, RADAR_CHG_STATE, r);
    } else if (dp->id == v->dp_distance) {
        /* signed on the wire: the 7gclukjs MCU sends -3 when there is no target, and 0..-2 for a
         * target closer than ~25 cm. 0 cm stays reserved for "no target", so a present target is
         * at least 1 cm after calibration. */
        int32_t raw = (int32_t)val;
        int32_t cm = 0;
        if (raw > v->distance_none) {
            if (raw > 100000) {
                raw = 100000;   /* corrupt input must not overflow the maths below */
            }
            cm = raw * v->distance_mm / 10;
            cm = cm * r->dist_scale_pct / 100 + r->dist_offset_cm;
            if (cm < 1) {
                cm = 1;
            } else if (cm > 0xFFFF) {
                cm = 0xFFFF;
            }
        }
        if (r->distance_cm != cm || !r->dist_sent) {
            r->distance_cm = (uint16_t)cm;
            chg = distance_update(r, now_ms);
        }
    } else if (dp->id == v->dp_illuminance) {
        chg = set_u16(&r->illuminance_lux, val, RADAR_CHG_ILLUMINANCE, r);
    } else if (dp->id == v->dp_move_sens) {
        chg = set_u8(&r->move_sens, val, RADAR_CHG_MOVE_SENS, r);
    } else if (dp->id == v->dp_pres_sens) {
        chg = set_u8(&r->pres_sens, val, RADAR_CHG_PRES_SENS, r);
    } else if (dp->id == v->dp_range_min) {
        chg = set_u16(&r->range_min_cm, val * v->range_mm / 10, RADAR_CHG_RANGE_MIN, r);
    } else if (dp->id == v->dp_range_max) {
        chg = set_u16(&r->range_max_cm, val * v->range_mm / 10, RADAR_CHG_RANGE_MAX, r);
    } else if (dp->id == v->dp_timeout) {
        chg = set_u16(&r->timeout_s, val, RADAR_CHG_TIMEOUT, r);
    }

    if (!chg) {
        r->dp_dropped++;
    }

    return chg;
}

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

size_t radar_build_setting(const radar_t *r, radar_setting_t s, uint32_t value, uint8_t *buf, size_t cap) {
    const radar_variant_t *v = r->v;
    uint8_t dp;
    uint32_t raw;

    if (!v) {
        return 0;
    }

    switch (s) {
        case RADAR_SET_MOVE_SENS:
            dp = v->dp_move_sens;
            raw = clamp(value, 0, v->sens_max);
            break;
        case RADAR_SET_PRES_SENS:
            dp = v->dp_pres_sens;
            raw = clamp(value, 0, v->sens_max);
            break;
        case RADAR_SET_RANGE_MIN:
            dp = v->dp_range_min;
            raw = clamp(value, 0, v->range_max_cm) * 10 / v->range_mm;
            break;
        case RADAR_SET_RANGE_MAX:
            dp = v->dp_range_max;
            raw = clamp(value, 0, v->range_max_cm) * 10 / v->range_mm;
            break;
        case RADAR_SET_TIMEOUT:
            dp = v->dp_timeout;
            raw = clamp(value, v->timeout_min_s, v->timeout_max_s);
            break;
        default:
            return 0;
    }

    if (dp == RADAR_DP_NONE) {
        return 0;
    }

    return tuya_dp_put(buf, cap, dp, TUYA_DP_VALUE, raw);
}

uint16_t radar_lux_to_zcl(uint32_t lux) {
    uint32_t n = 0, m, frac = 0, log2_q16, out;

    if (lux == 0) {
        return 0;
    }

    /* log2 by normalising to [1,2) in Q15 and squaring: one fraction bit per round */
    while ((lux >> n) > 1) {
        n++;
    }
    m = n > 15 ? lux >> (n - 15) : lux << (15 - n);
    for (int i = 15; i >= 0; i--) {
        m = (m * m) >> 15;
        if (m >= (2u << 15)) {
            m >>= 1;
            frac |= 1u << i;
        }
    }
    log2_q16 = (n << 16) | frac;

    /* 10000 * log10(x) = log2(x) * 3010.3; drop 4 fraction bits first so it fits 32 bits */
    out = ((log2_q16 >> 4) * 30103u) / (10u * 4096u) + 1;

    return out > 0xFFFE ? 0xFFFE : (uint16_t)out;
}
