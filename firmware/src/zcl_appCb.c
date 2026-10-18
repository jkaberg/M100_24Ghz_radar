#include "tl_common.h"
#include "zcl_include.h"

#include "app_main.h"

#ifdef ZCL_IDENTIFY
static ev_timer_event_t *identifyTimerEvt = NULL;
#endif

#ifdef ZCL_WRITE
static void app_zclWriteReqCmd(uint8_t endPoint, uint16_t clusterId, zclWriteCmd_t *pWriteReqCmd) {
    zclWriteRec_t *rec = pWriteReqCmd->attrList;

    if (endPoint != APP_ENDPOINT1 || clusterId != ZCL_CLUSTER_MS_OCCUPANCY_SENSING) {
        return;
    }

    /* the SDK has already stored the values; act on them */
    for (uint8_t i = 0; i < pWriteReqCmd->numAttr; i++) {
        app_radar_attr_written(rec[i].attrID);
    }
}
#endif

void app_zclProcessIncomingMsg(zclIncoming_t *pInHdlrMsg) {
    uint16_t cluster = pInHdlrMsg->msg->indInfo.cluster_id;
    uint8_t endPoint = pInHdlrMsg->msg->indInfo.dst_ep;

    switch (pInHdlrMsg->hdr.cmd) {
#ifdef ZCL_WRITE
        case ZCL_CMD_WRITE:
        case ZCL_CMD_WRITE_UNDIVIDED:
        case ZCL_CMD_WRITE_NO_RSP:
            app_zclWriteReqCmd(endPoint, cluster, pInHdlrMsg->attrCmd);
            break;
#endif
        default:
            break;
    }
}

#ifdef ZCL_BASIC
status_t app_basicCb(zclIncomingAddrInfo_t *pAddrInfo, uint8_t cmdId, void *cmdPayload) {
    if (cmdId == ZCL_CMD_BASIC_RESET_FAC_DEFAULT) {
        /* nothing to reset on the ZCL side; radar settings live in the MCU */
    }
    return ZCL_STA_SUCCESS;
}
#endif

#ifdef ZCL_IDENTIFY
static int32_t app_zclIdentifyTimerCb(void *arg) {
    if (g_zcl_identifyAttrs.identifyTime <= 0) {
        identifyTimerEvt = NULL;
        return -1;
    }
    g_zcl_identifyAttrs.identifyTime--;
    return 0;
}

static void app_zclIdentifyCmdHandler(uint8_t endpoint, uint16_t srcAddr, uint16_t identifyTime) {
    /* no LED to blink on this hardware: just keep the attribute counting down */
    g_zcl_identifyAttrs.identifyTime = identifyTime;

    if (identifyTime == 0) {
        if (identifyTimerEvt) {
            TL_ZB_TIMER_CANCEL(&identifyTimerEvt);
        }
    } else if (!identifyTimerEvt) {
        identifyTimerEvt = TL_ZB_TIMER_SCHEDULE(app_zclIdentifyTimerCb, NULL, 1000);
    }
}

status_t app_identifyCb(zclIncomingAddrInfo_t *pAddrInfo, uint8_t cmdId, void *cmdPayload) {
    if (pAddrInfo->dstEp == APP_ENDPOINT1 && pAddrInfo->dirCluster == ZCL_FRAME_CLIENT_SERVER_DIR) {
        if (cmdId == ZCL_CMD_IDENTIFY) {
            zcl_identifyCmd_t *pIdentify = (zcl_identifyCmd_t *)cmdPayload;
            app_zclIdentifyCmdHandler(pAddrInfo->dstEp, pAddrInfo->srcAddr, pIdentify->identifyTime);
        }
    }
    return ZCL_STA_SUCCESS;
}
#endif
