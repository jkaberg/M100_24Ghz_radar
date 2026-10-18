#ifndef HOST_ZCL_INCLUDE_H
#define HOST_ZCL_INCLUDE_H
#define ZCL_FRAME_SERVER_CLIENT_DIR         1
#define ZCL_CLUSTER_MS_OCCUPANCY_SENSING    0x0406
#define ZCL_DATA_TYPE_UINT16                0x21
int host_send_report(uint8_t srcEp, epInfo_t *dst, uint8_t disableDefaultRsp, uint8_t dir,
                     uint16_t cluster, uint16_t attr, uint8_t type, uint8_t *data);
#define zcl_sendReportCmd(a,b,c,d,e,f,g,h) host_send_report((a),(b),(c),(d),(e),(f),(g),(h))
#endif
