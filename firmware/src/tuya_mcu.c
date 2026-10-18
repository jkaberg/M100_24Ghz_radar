#include "tuya_mcu.h"

enum {
    ST_HDR1,
    ST_HDR2,
    ST_VER,
    ST_SEQ_H,
    ST_SEQ_L,
    ST_CMD,
    ST_LEN_H,
    ST_LEN_L,
    ST_DATA,
    ST_SUM,
};

void tuya_parser_reset(tuya_parser_t *p) {
    p->state = ST_HDR1;
    p->idx = 0;
    p->sum = 0;
}

int tuya_parser_feed(tuya_parser_t *p, uint8_t b) {
    tuya_frame_t *f = &p->frame;

    switch (p->state) {
        case ST_HDR1:
            if (b == TUYA_HDR1) {
                p->sum = b;
                p->state = ST_HDR2;
            }
            return 0;
        case ST_HDR2:
            if (b == TUYA_HDR2) {
                p->sum += b;
                p->state = ST_VER;
            } else if (b != TUYA_HDR1) {
                p->state = ST_HDR1;
            }
            /* 55 55 AA: stay in ST_HDR2, the second 55 may start the frame */
            return 0;
        case ST_VER:
            f->ver = b;
            break;
        case ST_SEQ_H:
            f->seq = (uint16_t)b << 8;
            break;
        case ST_SEQ_L:
            f->seq |= b;
            break;
        case ST_CMD:
            f->cmd = b;
            break;
        case ST_LEN_H:
            f->len = (uint16_t)b << 8;
            break;
        case ST_LEN_L:
            f->len |= b;
            p->sum += b;
            p->idx = 0;
            if (f->len > TUYA_MAX_DATA) {
                p->too_long++;
                tuya_parser_reset(p);
                return 0;
            }
            p->state = f->len ? ST_DATA : ST_SUM;
            return 0;
        case ST_DATA:
            f->data[p->idx++] = b;
            p->sum += b;
            if (p->idx == f->len) {
                p->state = ST_SUM;
            }
            return 0;
        case ST_SUM: {
            int ok = (b == p->sum);
            if (!ok) {
                p->bad_sum++;
            }
            tuya_parser_reset(p);
            return ok;
        }
        default:
            tuya_parser_reset(p);
            return 0;
    }

    p->sum += b;
    p->state++;
    return 0;
}

int tuya_parser_busy(const tuya_parser_t *p) {
    return p->state != ST_HDR1;
}

uint8_t tuya_checksum(const uint8_t *buf, size_t len) {
    uint8_t sum = 0;
    while (len--) {
        sum += *buf++;
    }
    return sum;
}

size_t tuya_build(uint8_t *buf, size_t cap, uint16_t seq, uint8_t cmd, const uint8_t *data, uint16_t len) {
    size_t total = (size_t)len + TUYA_FRAME_OVERHEAD;

    if (total > cap) {
        return 0;
    }

    buf[0] = TUYA_HDR1;
    buf[1] = TUYA_HDR2;
    buf[2] = TUYA_VER_MODULE;
    buf[3] = seq >> 8;
    buf[4] = seq & 0xFF;
    buf[5] = cmd;
    buf[6] = len >> 8;
    buf[7] = len & 0xFF;
    for (uint16_t i = 0; i < len; i++) {
        buf[8 + i] = data[i];
    }
    buf[total - 1] = tuya_checksum(buf, total - 1);

    return total;
}

int tuya_dp_next(const uint8_t *data, uint16_t len, uint16_t *off, tuya_dp_t *dp) {
    uint16_t o = *off;

    if ((uint32_t)o + 4 > len) {
        return 0;
    }

    dp->id = data[o];
    dp->type = data[o + 1];
    dp->len = ((uint16_t)data[o + 2] << 8) | data[o + 3];

    if ((uint32_t)o + 4 + dp->len > len) {
        return 0;
    }

    dp->val = &data[o + 4];
    *off = o + 4 + dp->len;

    return 1;
}

uint32_t tuya_dp_u32(const tuya_dp_t *dp) {
    uint32_t v = 0;
    uint16_t n = dp->len > 4 ? 4 : dp->len;

    for (uint16_t i = 0; i < n; i++) {
        v = (v << 8) | dp->val[i];
    }

    return v;
}

size_t tuya_dp_put(uint8_t *buf, size_t cap, uint8_t id, uint8_t type, uint32_t value) {
    uint8_t n;

    switch (type) {
        case TUYA_DP_BOOL:
        case TUYA_DP_ENUM:
            n = 1;
            break;
        case TUYA_DP_VALUE:
            n = 4;
            break;
        default:
            return 0;
    }

    if (cap < (size_t)n + 4) {
        return 0;
    }

    buf[0] = id;
    buf[1] = type;
    buf[2] = 0;
    buf[3] = n;
    for (uint8_t i = 0; i < n; i++) {
        buf[4 + i] = (value >> (8 * (n - 1 - i))) & 0xFF;
    }

    return (size_t)n + 4;
}

size_t tuya_product_id(const uint8_t *json, uint16_t len, char *pid, size_t cap) {
    /* look for "p":" and copy up to the closing quote */
    static const char key[] = "\"p\":\"";
    const uint16_t klen = sizeof(key) - 1;

    if (cap == 0) {
        return 0;
    }
    pid[0] = 0;

    for (uint16_t i = 0; i + klen <= len; i++) {
        uint16_t k = 0;
        while (k < klen && json[i + k] == (uint8_t)key[k]) {
            k++;
        }
        if (k != klen) {
            continue;
        }

        size_t n = 0;
        for (uint16_t j = i + klen; j < len && json[j] != '"'; j++) {
            if (n + 1 >= cap) {
                break;
            }
            pid[n++] = (char)json[j];
        }
        pid[n] = 0;
        return n;
    }

    return 0;
}
