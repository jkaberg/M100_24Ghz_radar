#ifndef SRC_INCLUDE_APP_ENDPOINT_CFG_H_
#define SRC_INCLUDE_APP_ENDPOINT_CFG_H_

#define APP_ENDPOINT1 0x01

typedef struct {
    uint8_t  zclVersion;
    uint8_t  appVersion;
    uint8_t  stackVersion;
    uint8_t  hwVersion;
    uint8_t  manuName[ZCL_BASIC_MAX_LENGTH];
    uint8_t  modelId[ZCL_BASIC_MAX_LENGTH];
    uint8_t  dateCode[ZCL_BASIC_MAX_LENGTH];
    uint8_t  powerSource;
    uint8_t  genDevClass;
    uint8_t  genDevType;
    uint8_t  deviceEnable;
    uint8_t  swBuildId[ZCL_BASIC_MAX_LENGTH];
} zcl_basicAttr_t;

typedef struct {
    uint16_t identifyTime;
} zcl_identifyAttr_t;

typedef struct {
    uint8_t nameSupport;
} zcl_groupAttr_t;

extern uint8_t APP_EP1_CB_CLUSTER_NUM;

extern zcl_basicAttr_t    g_zcl_basicAttrs;
extern zcl_identifyAttr_t g_zcl_identifyAttrs;
extern zcl_groupAttr_t    g_zcl_group1Attrs;

#endif /* SRC_INCLUDE_APP_ENDPOINT_CFG_H_ */
