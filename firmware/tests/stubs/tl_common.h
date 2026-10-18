/* Host stand-ins for the few Telink SDK symbols app_radar.c uses (tests only). */
#ifndef HOST_TL_COMMON_H
#define HOST_TL_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#define TRUE  1
#define FALSE 0
#define sys_tick_per_us 16

typedef int nv_sts_t;
#define NV_SUCC                 0
#define NV_ITEM_NOT_FOUND       1
#define NV_MODULE_APP           1
#define NV_ITEM_APP_USER_CFG    0x20
#define NV_ITEM_APP_DIST_CAL    0x22

typedef struct {
    uint8_t  dstAddrMode;
    struct { uint16_t shortAddr; } dstAddr;
    uint8_t  dstEp;
    uint16_t profileId;
} epInfo_t;
#define APS_SHORT_DSTADDR_WITHEP  2
#define HA_PROFILE_ID             0x0104

typedef int32_t (*ev_timer_cb_t)(void *arg);
void *host_timer_schedule(ev_timer_cb_t cb, void *arg, uint32_t ms);
#define TL_ZB_TIMER_SCHEDULE(cb, arg, ms) host_timer_schedule((cb), (arg), (ms))

uint32_t clock_time(void);
nv_sts_t nv_flashReadNew(uint8_t single, uint8_t id, uint8_t itemId, uint16_t len, uint8_t *buf);
nv_sts_t nv_flashWriteNew(uint8_t single, uint16_t id, uint8_t itemId, uint16_t len, uint8_t *buf);
bool zb_isDeviceJoinedNwk(void);
bool zb_isDeviceFactoryNew(void);
void zb_factoryReset(void);
bool bdb_isIdle(void);
#endif
