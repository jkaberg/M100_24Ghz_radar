#include "app_main.h"
#include "factory_reset.h"

static bool boot_announce_sent = false;

/*
 * Safe mode: if the module boots SAFE_MODE_BOOTS times without SAFE_MODE_STABLE_MS of uptime in
 * between (crash loop, or deliberate quick power cycles), it starts without the radar code: Zigbee
 * and OTA only, so a bad image can always be replaced over the air. Attribute 0xF00F says so.
 */
#define SAFE_MODE_BOOTS         5
#define SAFE_MODE_STABLE_MS     (30 * 60 * 1000)

uint8_t g_safe_mode = 0;
static uint8_t boot_cnt = 0;

static int32_t bootCntClearCb(void *arg) {
    (void)arg;
    boot_cnt = 0;
    nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_BOOT_CNT, 1, &boot_cnt);
    return -1;
}

static void safe_mode_check(void) {
    if (nv_flashReadNew(1, NV_MODULE_APP, NV_ITEM_APP_BOOT_CNT, 1, &boot_cnt) != NV_SUCC || boot_cnt == 0xFF) {
        boot_cnt = 0;
    }
    boot_cnt++;
    g_safe_mode = boot_cnt >= SAFE_MODE_BOOTS;
    nv_flashWriteNew(1, NV_MODULE_APP, NV_ITEM_APP_BOOT_CNT, 1, &boot_cnt);
    TL_ZB_TIMER_SCHEDULE(bootCntClearCb, NULL, SAFE_MODE_STABLE_MS);
}

app_ctx_t g_appCtx = {
    .bdbFBTimerEvt = NULL,
    .factory_resetTimerEvt = NULL,
};

#ifdef ZCL_OTA
extern ota_callBack_t app_otaCb;

/* running firmware, as announced to the OTA server */
ota_preamble_t app_otaInfo = {
    .fileVer            = FILE_VERSION,
    .imageType          = IMAGE_TYPE,
    .manufacturerCode   = MANUFACTURER_CODE_TELINK,
};
#endif

const zdo_appIndCb_t appCbLst = {
    bdb_zdoStartDevCnf,             /* start device cnf cb */
    NULL,                           /* reset cnf cb */
    NULL,                           /* device announce indication cb */
    app_leaveIndHandler,            /* leave ind cb */
    app_leaveCnfHandler,            /* leave cnf cb */
    app_nwkUpdateIndicateHandler,   /* nwk update ind cb */
    NULL,                           /* permit join ind cb */
    NULL,                           /* nlme sync cnf cb */
    NULL,                           /* tc join ind cb */
    NULL,                           /* tc detects that the frame counter is near limit */
};

bdb_commissionSetting_t g_bdbCommissionSetting = {
    .linkKey.tcLinkKey.keyType = SS_GLOBAL_LINK_KEY,
    .linkKey.tcLinkKey.key = (uint8_t *)tcLinkKeyCentralDefault,
    .linkKey.distributeLinkKey.keyType = MASTER_KEY,
    .linkKey.distributeLinkKey.key = (uint8_t *)linkKeyDistributedMaster,
    .linkKey.touchLinkKey.keyType = MASTER_KEY,
    .linkKey.touchLinkKey.key = (uint8_t *)touchLinkKeyMaster,
    .touchlinkEnable = 0,
};

static void stack_init(void) {
    zb_init();
    zb_zdoCbRegister((zdo_appIndCb_t *)&appCbLst);
}

static void user_app_init(void) {
    af_powerDescPowerModeUpdate(POWER_MODE_RECEIVER_COMES_WHEN_STIMULATED);

    zcl_init(app_zclProcessIncomingMsg);
    af_endpointRegister(APP_ENDPOINT1, (af_simple_descriptor_t *)&app_ep1Desc, zcl_rx_handler, NULL);
    zcl_reportingTabInit();
    zcl_register(APP_ENDPOINT1, APP_EP1_CB_CLUSTER_NUM, (zcl_specClusterInfo_t *)g_appEp1ClusterList);

#if ZCL_GP_SUPPORT
    gp_init(APP_ENDPOINT1);
#endif

#if ZCL_OTA_SUPPORT
    ota_init(OTA_TYPE_CLIENT, (af_simple_descriptor_t *)&app_ep1Desc, &app_otaInfo, &app_otaCb);
#endif

    if (!g_safe_mode) {
        app_radar_init();
    }
}

static void app_task(void) {
    if (!g_safe_mode) {
        app_radar_task();
    }

    /* the SDK only sends attribute reports from here */
    if (bdb_isIdle()) {
        report_handler();
    }

    if (!boot_announce_sent && zb_isDeviceJoinedNwk()) {
        zb_zdoSendDevAnnance();
        boot_announce_sent = true;
    }

    factroyRst_handler();
}

extern volatile uint16_t T_evtExcept[4];

static void appSysException(void) {
    SYSTEM_RESET();
}

void user_init(bool isRetention) {
    (void)isRetention;

    stack_init();
    safe_mode_check();
    user_app_init();

    sys_exceptHandlerRegister(appSysException);

    ev_on_poll(EV_POLL_IDLE, app_task);

    /* 10 power cycles, each shorter than 2 s, leave the network (there may be no usable button) */
    factroyRst_init();

    if (bdb_preInstallCodeLoad(&g_appCtx.tcLinkKey.keyType, g_appCtx.tcLinkKey.key) == RET_OK) {
        g_bdbCommissionSetting.linkKey.tcLinkKey.keyType = g_appCtx.tcLinkKey.keyType;
        g_bdbCommissionSetting.linkKey.tcLinkKey.key = g_appCtx.tcLinkKey.key;
    }

    /*
     * Default reporting until the coordinator configures its own:
     *  - occupancy: on every change, hourly heartbeat;
     *  - presence/move state: on change but at most every 10 s;
     *  - illuminance: at most every 60 s and only on a ~25 % change (1000 in log units).
     * Distance is not here on purpose (see app_radar.c).
     */
    uint8_t change0 = 0;
    uint16_t changeLux = 1000;
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_OCCUPANCY_SENSING,
                            ZCL_ATTRID_OCCUPANCY, 0, REPORTING_MAX, &change0);
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_OCCUPANCY_SENSING,
                            ATTRID_RADAR_STATE, 10, REPORTING_MAX, &change0);
    bdb_defaultReportingCfg(APP_ENDPOINT1, HA_PROFILE_ID, ZCL_CLUSTER_MS_ILLUMINANCE_MEASUREMENT,
                            ZCL_ATTRID_MEASURED_VALUE, 60, REPORTING_MAX, (uint8_t *)&changeLux);

    bdb_init((af_simple_descriptor_t *)&app_ep1Desc, &g_bdbCommissionSetting, &g_zbBdbCb, 1);
    rf_setTxPower(MY_RF_POWER_INDEX);
}
