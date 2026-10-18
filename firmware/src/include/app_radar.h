#ifndef SRC_INCLUDE_APP_RADAR_H_
#define SRC_INCLUDE_APP_RADAR_H_

/* Glue between the radar MCU (UART, tuya_mcu.c + radar.c) and the Zigbee stack. */

/* Custom attributes on the Occupancy Sensing cluster (0x0406), no manufacturer code */
#define ATTRID_RADAR_STATE              0xF000  /* enum8  none/presence/move, reportable */
#define ATTRID_RADAR_DISTANCE           0xF001  /* uint16 cm, sent by the module itself */
#define ATTRID_RADAR_DIST_INTERVAL      0xF002  /* uint16 s, 0 = distance reports off   */
#define ATTRID_RADAR_DIST_DEADBAND      0xF003  /* uint16 cm                            */
#define ATTRID_RADAR_MOVE_SENS          0xF004  /* uint8  0..10                         */
#define ATTRID_RADAR_PRES_SENS          0xF005  /* uint8  0..10                         */
#define ATTRID_RADAR_RANGE_MIN          0xF006  /* uint16 cm                            */
#define ATTRID_RADAR_RANGE_MAX          0xF007  /* uint16 cm                            */
#define ATTRID_RADAR_TIMEOUT            0xF008  /* uint16 s                             */
#define ATTRID_RADAR_DIST_OFFSET        0xF009  /* int16  cm added to every distance     */
#define ATTRID_RADAR_DIST_SCALE         0xF00A  /* uint16 % applied before the offset    */
#define ATTRID_RADAR_SAFE_MODE          0xF00F  /* uint8  1 = started without radar code */

#define RADAR_DEFAULT_DIST_INTERVAL     0       /* the flood source stays off unless asked for */
#define RADAR_DEFAULT_DIST_DEADBAND     25
#define RADAR_MAX_DIST_INTERVAL         3600
#define RADAR_MAX_DIST_DEADBAND         500

typedef struct {
    uint8_t  occupancy;         /* bitmap8, bit 0 */
    uint8_t  sensorType;
    uint8_t  state;
    uint16_t distance_cm;
    uint16_t dist_interval_s;
    uint16_t dist_deadband_cm;
    uint8_t  move_sens;
    uint8_t  pres_sens;
    uint16_t range_min_cm;
    uint16_t range_max_cm;
    uint16_t timeout_s;
    int16_t  dist_offset_cm;
    uint16_t dist_scale_pct;
} zcl_radarAttr_t;

typedef struct {
    uint16_t measuredValue;
    uint16_t minMeasuredValue;
    uint16_t maxMeasuredValue;
} zcl_illuminanceAttr_t;

extern zcl_radarAttr_t       g_zcl_radarAttrs;
extern zcl_illuminanceAttr_t g_zcl_illuminanceAttrs;
extern uint8_t               g_safe_mode;

void app_radar_init(void);
void app_radar_task(void);
/* Zigbee network state changed: TUYA_NET_* */
void app_radar_net_status(uint8_t status);
/* An attribute on the occupancy cluster was written over Zigbee (value already stored). */
void app_radar_attr_written(uint16_t attrId);

#endif /* SRC_INCLUDE_APP_RADAR_H_ */
