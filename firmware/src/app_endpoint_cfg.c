#include "tl_common.h"
#include "zcl_include.h"

#include "app_main.h"

#define R               ACCESS_CONTROL_READ
#define RW              ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE
#define RR              ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE
#define RWR             ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE | ACCESS_CONTROL_REPORTABLE

const uint16_t app_ep1_inClusterList[] = {
    ZCL_CLUSTER_GEN_BASIC,
    ZCL_CLUSTER_GEN_IDENTIFY,
#ifdef ZCL_GROUP
    ZCL_CLUSTER_GEN_GROUPS,
#endif
    ZCL_CLUSTER_MS_OCCUPANCY_SENSING,
    ZCL_CLUSTER_MS_ILLUMINANCE_MEASUREMENT,
};

const uint16_t app_ep1_outClusterList[] = {
#ifdef ZCL_OTA
    ZCL_CLUSTER_OTA,
#endif
};

#define APP_EP1_IN_CLUSTER_NUM   (sizeof(app_ep1_inClusterList)/sizeof(app_ep1_inClusterList[0]))
#define APP_EP1_OUT_CLUSTER_NUM  (sizeof(app_ep1_outClusterList)/sizeof(app_ep1_outClusterList[0]))

const af_simple_descriptor_t app_ep1Desc = {
    HA_PROFILE_ID,                          /* Application profile identifier */
    HA_DEV_OCCUPANCY_SENSOR,                /* Application device identifier */
    APP_ENDPOINT1,                          /* Endpoint */
    1,                                      /* Application device version */
    0,                                      /* Reserved */
    APP_EP1_IN_CLUSTER_NUM,                 /* Application input cluster count */
    APP_EP1_OUT_CLUSTER_NUM,                /* Application output cluster count */
    (uint16_t *)app_ep1_inClusterList,      /* Application input cluster list */
    (uint16_t *)app_ep1_outClusterList,     /* Application output cluster list */
};

/* Basic */
zcl_basicAttr_t g_zcl_basicAttrs = {
    .zclVersion     = 0x03,
    .appVersion     = APP_RELEASE,
    .stackVersion   = (STACK_RELEASE|STACK_BUILD),
    .hwVersion      = HW_VERSION,
    .manuName       = ZCL_BASIC_MFG_NAME,
    .modelId        = ZCL_BASIC_MODEL_ID,
    .dateCode       = ZCL_BASIC_DATE_CODE,
    .powerSource    = POWER_SOURCE_MAINS_1_PHASE,
    .swBuildId      = ZCL_BASIC_SW_BUILD_ID,
    .deviceEnable   = TRUE,
};

const zclAttrInfo_t basic_attrTbl[] = {
    { ZCL_ATTRID_BASIC_ZCL_VER,             ZCL_DATA_TYPE_UINT8,    R,  (uint8_t*)&g_zcl_basicAttrs.zclVersion  },
    { ZCL_ATTRID_BASIC_APP_VER,             ZCL_DATA_TYPE_UINT8,    R,  (uint8_t*)&g_zcl_basicAttrs.appVersion  },
    { ZCL_ATTRID_BASIC_STACK_VER,           ZCL_DATA_TYPE_UINT8,    R,  (uint8_t*)&g_zcl_basicAttrs.stackVersion},
    { ZCL_ATTRID_BASIC_HW_VER,              ZCL_DATA_TYPE_UINT8,    R,  (uint8_t*)&g_zcl_basicAttrs.hwVersion   },
    { ZCL_ATTRID_BASIC_MFR_NAME,            ZCL_DATA_TYPE_CHAR_STR, R,  (uint8_t*)g_zcl_basicAttrs.manuName     },
    { ZCL_ATTRID_BASIC_MODEL_ID,            ZCL_DATA_TYPE_CHAR_STR, R,  (uint8_t*)g_zcl_basicAttrs.modelId      },
    { ZCL_ATTRID_BASIC_DATE_CODE,           ZCL_DATA_TYPE_CHAR_STR, R,  (uint8_t*)g_zcl_basicAttrs.dateCode     },
    { ZCL_ATTRID_BASIC_POWER_SOURCE,        ZCL_DATA_TYPE_ENUM8,    R,  (uint8_t*)&g_zcl_basicAttrs.powerSource },
    { ZCL_ATTRID_BASIC_DEV_ENABLED,         ZCL_DATA_TYPE_BOOLEAN,  RW, (uint8_t*)&g_zcl_basicAttrs.deviceEnable},
    { ZCL_ATTRID_BASIC_SW_BUILD_ID,         ZCL_DATA_TYPE_CHAR_STR, R,  (uint8_t*)&g_zcl_basicAttrs.swBuildId   },
    { ZCL_ATTRID_GLOBAL_CLUSTER_REVISION,   ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&zcl_attr_global_clusterRevision},
};

#define ZCL_BASIC_ATTR_NUM       (sizeof(basic_attrTbl) / sizeof(zclAttrInfo_t))

/* Identify */
zcl_identifyAttr_t g_zcl_identifyAttrs = {
    .identifyTime   = 0x0000,
};

const zclAttrInfo_t identify_attrTbl[] = {
    { ZCL_ATTRID_IDENTIFY_TIME,             ZCL_DATA_TYPE_UINT16,   RW, (uint8_t*)&g_zcl_identifyAttrs.identifyTime},
    { ZCL_ATTRID_GLOBAL_CLUSTER_REVISION,   ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&zcl_attr_global_clusterRevision},
};

#define ZCL_IDENTIFY_ATTR_NUM    (sizeof(identify_attrTbl) / sizeof(zclAttrInfo_t))

#ifdef ZCL_GROUP
/* Group */
zcl_groupAttr_t g_zcl_group1Attrs = {
    .nameSupport    = 0,
};

const zclAttrInfo_t group_attr1Tbl[] = {
    { ZCL_ATTRID_GROUP_NAME_SUPPORT,        ZCL_DATA_TYPE_BITMAP8,  R,  (uint8_t*)&g_zcl_group1Attrs.nameSupport},
    { ZCL_ATTRID_GLOBAL_CLUSTER_REVISION,   ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&zcl_attr_global_clusterRevision},
};

#define ZCL_GROUP_1ATTR_NUM      (sizeof(group_attr1Tbl) / sizeof(zclAttrInfo_t))
#endif

/*
 * Occupancy sensing, plus the radar's own attributes (app_radar.h). Occupancy, presence/move
 * state and the radar settings are reportable through the SDK (the settings only change when
 * written, so they stay quiet). Distance is not: app_radar.c sends it under its own
 * interval/deadband policy, so a reporting config from the coordinator can't re-open the flood.
 */
zcl_radarAttr_t g_zcl_radarAttrs = {
    .occupancy          = 0,
    .sensorType         = 0,
    .state              = 0,
    .distance_cm        = 0,
    .dist_interval_s    = RADAR_DEFAULT_DIST_INTERVAL,
    .dist_deadband_cm   = RADAR_DEFAULT_DIST_DEADBAND,
    .dist_offset_cm     = 0,
    .dist_scale_pct     = RADAR_DIST_SCALE_DEF,
};

const zclAttrInfo_t radar_attrTbl[] = {
    { ZCL_ATTRID_OCCUPANCY,                 ZCL_DATA_TYPE_BITMAP8,  RR, (uint8_t*)&g_zcl_radarAttrs.occupancy       },
    { ZCL_ATTRID_OCCUPANCY_SENSOR_TYPE,     ZCL_DATA_TYPE_ENUM8,    R,  (uint8_t*)&g_zcl_radarAttrs.sensorType      },
    { ATTRID_RADAR_STATE,                   ZCL_DATA_TYPE_ENUM8,    RR, (uint8_t*)&g_zcl_radarAttrs.state           },
    { ATTRID_RADAR_DISTANCE,                ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&g_zcl_radarAttrs.distance_cm     },
    { ATTRID_RADAR_DIST_INTERVAL,           ZCL_DATA_TYPE_UINT16,   RW, (uint8_t*)&g_zcl_radarAttrs.dist_interval_s },
    { ATTRID_RADAR_DIST_DEADBAND,           ZCL_DATA_TYPE_UINT16,   RW, (uint8_t*)&g_zcl_radarAttrs.dist_deadband_cm},
    { ATTRID_RADAR_MOVE_SENS,               ZCL_DATA_TYPE_UINT8,    RWR, (uint8_t*)&g_zcl_radarAttrs.move_sens       },
    { ATTRID_RADAR_PRES_SENS,               ZCL_DATA_TYPE_UINT8,    RWR, (uint8_t*)&g_zcl_radarAttrs.pres_sens       },
    { ATTRID_RADAR_RANGE_MIN,               ZCL_DATA_TYPE_UINT16,   RWR, (uint8_t*)&g_zcl_radarAttrs.range_min_cm    },
    { ATTRID_RADAR_RANGE_MAX,               ZCL_DATA_TYPE_UINT16,   RWR, (uint8_t*)&g_zcl_radarAttrs.range_max_cm    },
    { ATTRID_RADAR_TIMEOUT,                 ZCL_DATA_TYPE_UINT16,   RWR, (uint8_t*)&g_zcl_radarAttrs.timeout_s       },
    { ATTRID_RADAR_DIST_OFFSET,             ZCL_DATA_TYPE_INT16,    RW,  (uint8_t*)&g_zcl_radarAttrs.dist_offset_cm  },
    { ATTRID_RADAR_DIST_SCALE,              ZCL_DATA_TYPE_UINT16,   RW,  (uint8_t*)&g_zcl_radarAttrs.dist_scale_pct  },
    { ATTRID_RADAR_SAFE_MODE,               ZCL_DATA_TYPE_UINT8,    R,   (uint8_t*)&g_safe_mode                      },
    { ZCL_ATTRID_GLOBAL_CLUSTER_REVISION,   ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&zcl_attr_global_clusterRevision  },
};

#define ZCL_RADAR_ATTR_NUM       (sizeof(radar_attrTbl) / sizeof(zclAttrInfo_t))

/* Illuminance measurement */
zcl_illuminanceAttr_t g_zcl_illuminanceAttrs = {
    .measuredValue      = 0,
    .minMeasuredValue   = 1,
    .maxMeasuredValue   = 0xFFFE,
};

const zclAttrInfo_t illuminance_attrTbl[] = {
    { ZCL_ATTRID_MEASURED_VALUE,            ZCL_DATA_TYPE_UINT16,   RR, (uint8_t*)&g_zcl_illuminanceAttrs.measuredValue    },
    { ZCL_ATTRID_MIN_MEASURED_VALUE,        ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&g_zcl_illuminanceAttrs.minMeasuredValue },
    { ZCL_ATTRID_MAX_MEASURED_VALUE,        ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&g_zcl_illuminanceAttrs.maxMeasuredValue },
    { ZCL_ATTRID_GLOBAL_CLUSTER_REVISION,   ZCL_DATA_TYPE_UINT16,   R,  (uint8_t*)&zcl_attr_global_clusterRevision         },
};

#define ZCL_ILLUMINANCE_ATTR_NUM (sizeof(illuminance_attrTbl) / sizeof(zclAttrInfo_t))

const zcl_specClusterInfo_t g_appEp1ClusterList[] = {
    {ZCL_CLUSTER_GEN_BASIC,     MANUFACTURER_CODE_NONE, ZCL_BASIC_ATTR_NUM,     basic_attrTbl,      zcl_basic_register,     app_basicCb},
    {ZCL_CLUSTER_GEN_IDENTIFY,  MANUFACTURER_CODE_NONE, ZCL_IDENTIFY_ATTR_NUM,  identify_attrTbl,   zcl_identify_register,  app_identifyCb},
#ifdef ZCL_GROUP
    {ZCL_CLUSTER_GEN_GROUPS,    MANUFACTURER_CODE_NONE, ZCL_GROUP_1ATTR_NUM,    group_attr1Tbl,     zcl_group_register,     NULL},
#endif
    {ZCL_CLUSTER_MS_OCCUPANCY_SENSING,       MANUFACTURER_CODE_NONE, ZCL_RADAR_ATTR_NUM,       radar_attrTbl,       zcl_occupancySensing_register,   NULL},
    {ZCL_CLUSTER_MS_ILLUMINANCE_MEASUREMENT, MANUFACTURER_CODE_NONE, ZCL_ILLUMINANCE_ATTR_NUM, illuminance_attrTbl, zcl_illuminanceMeasure_register, NULL},
};

uint8_t APP_EP1_CB_CLUSTER_NUM = (sizeof(g_appEp1ClusterList)/sizeof(g_appEp1ClusterList[0]));
