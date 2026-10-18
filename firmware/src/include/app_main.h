#ifndef SRC_INCLUDE_APP_MAIN_H_
#define SRC_INCLUDE_APP_MAIN_H_

#include "tl_common.h"
#include "zcl_include.h"
#include "bdb.h"
#include "ota.h"
#include "gp.h"

#include "tuya_mcu.h"
#include "radar.h"
#include "app_endpoint_cfg.h"
#include "app_uart.h"
#include "app_radar.h"

typedef struct {
    uint8_t keyType;            /* SS_UNIQUE_LINK_KEY or SS_GLOBAL_LINK_KEY */
    uint8_t key[16];
} app_linkKey_info_t;

typedef struct {
    ev_timer_event_t *bdbFBTimerEvt;
    ev_timer_event_t *factory_resetTimerEvt;
    app_linkKey_info_t tcLinkKey;
} app_ctx_t;

extern app_ctx_t g_appCtx;
extern bdb_appCb_t g_zbBdbCb;
extern bdb_commissionSetting_t g_bdbCommissionSetting;
extern const zcl_specClusterInfo_t g_appEp1ClusterList[];
extern const af_simple_descriptor_t app_ep1Desc;

void app_zclProcessIncomingMsg(zclIncoming_t *pInHdlrMsg);
status_t app_basicCb(zclIncomingAddrInfo_t *pAddrInfo, uint8_t cmdId, void *cmdPayload);
status_t app_identifyCb(zclIncomingAddrInfo_t *pAddrInfo, uint8_t cmdId, void *cmdPayload);

void app_leaveCnfHandler(nlme_leave_cnf_t *pLeaveCnf);
void app_leaveIndHandler(nlme_leave_ind_t *pLeaveInd);
bool app_nwkUpdateIndicateHandler(nwkCmd_nwkUpdate_t *pNwkUpdate);
void app_otaProcessMsgHandler(uint8_t evt, uint8_t status);

#endif /* SRC_INCLUDE_APP_MAIN_H_ */
