/*
 * Tuya Zigbee serial protocol, module side.
 *
 * Frame: 55 AA <ver> <seq hi> <seq lo> <cmd> <len hi> <len lo> <data...> <sum>
 * sum = byte sum of everything before it, mod 256. Multi-byte fields are big-endian.
 * DP unit inside 0x04/0x05/0x06: <dpid> <type> <len hi> <len lo> <value...>
 *
 * Pure C with no SDK dependencies, so it builds and runs in the host unit tests.
 */
#ifndef TUYA_MCU_H_
#define TUYA_MCU_H_

#ifdef MCU_CORE_8258
#include "tl_common.h"      /* the SDK typedefs uint8_t etc. itself (app_types.h) */
#else
#include <stdint.h>
#include <stddef.h>
#endif

#define TUYA_HDR1               0x55
#define TUYA_HDR2               0xAA
#define TUYA_VER_MODULE         0x02    /* version byte the stock module sends */
#define TUYA_FRAME_OVERHEAD     9       /* header(2) ver seq(2) cmd len(2) sum */
#define TUYA_MAX_DATA           128     /* spec max frame is 62 bytes; product info is ~28 */

/* Command words, see Tuya "Zigbee serial port protocol" */
#define TUYA_CMD_FACTORY_RESET  0x00    /* M->C */
#define TUYA_CMD_PRODUCT_INFO   0x01    /* M->C, MCU answers {"p":"<pid>","v":"x.y.z"} */
#define TUYA_CMD_NET_STATUS     0x02    /* M->C, 1 byte */
#define TUYA_CMD_NET_RESET      0x03    /* C->M, 0 = soft reset, 1 = leave and pair */
#define TUYA_CMD_DP_WRITE       0x04    /* M->C */
#define TUYA_CMD_DP_REPORT      0x05    /* C->M, module ACKs with 1 byte */
#define TUYA_CMD_DP_REPORT_ASYNC 0x06   /* C->M, module ACKs, MCU retries if not */
#define TUYA_CMD_MCU_VERSION    0x0B    /* M->C */
#define TUYA_CMD_NET_STATUS_REQ 0x20    /* C->M, module answers 1 byte */
#define TUYA_CMD_TIME_REQ       0x24    /* C->M, module answers 8 bytes */
#define TUYA_CMD_GW_STATUS_REQ  0x25    /* C->M, module answers 1 byte */
#define TUYA_CMD_DP_QUERY       0x28    /* M->C, empty = all DPs */

#define TUYA_NET_NOT_JOINED     0x00
#define TUYA_NET_JOINED         0x01
#define TUYA_NET_ERROR          0x02
#define TUYA_NET_JOINING        0x03

#define TUYA_DP_RAW             0x00
#define TUYA_DP_BOOL            0x01
#define TUYA_DP_VALUE           0x02
#define TUYA_DP_STRING          0x03
#define TUYA_DP_ENUM            0x04
#define TUYA_DP_BITMAP          0x05

typedef struct {
    uint8_t  ver;
    uint16_t seq;
    uint8_t  cmd;
    uint16_t len;
    uint8_t  data[TUYA_MAX_DATA];
} tuya_frame_t;

typedef struct {
    uint8_t      state;
    uint16_t     idx;           /* bytes of payload collected */
    uint8_t      sum;
    tuya_frame_t frame;
    uint32_t     bad_sum;       /* statistics, handy when debugging on the bench */
    uint32_t     too_long;
} tuya_parser_t;

typedef struct {
    uint8_t        id;
    uint8_t        type;
    uint16_t       len;
    const uint8_t *val;
} tuya_dp_t;

void     tuya_parser_reset(tuya_parser_t *p);
/* Feed one byte. Returns 1 when p->frame holds a complete frame with a valid checksum. */
int      tuya_parser_feed(tuya_parser_t *p, uint8_t byte);
/* 1 while in the middle of a frame (the caller resets after an inter-byte gap) */
int      tuya_parser_busy(const tuya_parser_t *p);

uint8_t  tuya_checksum(const uint8_t *buf, size_t len);
/* Build a frame into buf. Returns the frame length, or 0 if it does not fit. */
size_t   tuya_build(uint8_t *buf, size_t cap, uint16_t seq, uint8_t cmd, const uint8_t *data, uint16_t len);

/* Walk the DP units in a 0x05/0x06 payload. Returns 1 and fills dp while there is a whole unit left. */
int      tuya_dp_next(const uint8_t *data, uint16_t len, uint16_t *off, tuya_dp_t *dp);
/* Big-endian value of a bool/value/enum/bitmap DP (1, 2 or 4 bytes). */
uint32_t tuya_dp_u32(const tuya_dp_t *dp);
/* Append one DP unit. Returns bytes written, or 0 if it does not fit. */
size_t   tuya_dp_put(uint8_t *buf, size_t cap, uint8_t id, uint8_t type, uint32_t value);

/* Pull the "p" field out of the product-info JSON. Returns its length (0 if absent). */
size_t   tuya_product_id(const uint8_t *json, uint16_t len, char *pid, size_t cap);

#endif /* TUYA_MCU_H_ */
