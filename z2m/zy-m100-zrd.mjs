// Zigbee2MQTT converter for the ZY-M100-24GV2 (_TZE204_7gclukjs) and 24GV3 (_TZE204_ya4ft0w4)
// radars, one file for both states:
//  - stock: z2m's built-in definition plus `ota: true`, so the custom firmware can be pushed to it;
//  - converted (modelId ZY-M100-ZRD_1): the custom firmware, everything on the Occupancy Sensing
//    cluster, radar values in attributes 0xF000-0xF00F (firmware/src/include/app_radar.h).
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';
import tuya from 'zigbee-herdsman-converters/devices/tuya';

const OCC = 'msOccupancySensing';
const UINT8 = 0x20;
const UINT16 = 0x21;
const INT16 = 0x29;
const ENUM8 = 0x30;
// settings only change when written, so "report on change" stays quiet
const onChange = {min: 0, max: 0xfffe, change: 1};

const stock = {
    ...tuya.definitions.find((d) => d.model === 'ZY-M100-24GV2'),
    fingerprint: [{modelID: 'TS0601', manufacturerName: '_TZE204_7gclukjs'}],
    ota: true,
};

// V3: z2m's definition also covers _TZE200_ya4ft0w4 and *_gkfbdvyx, which have another Zigbee
// module (docs/findings.md), so OTA is only enabled for the _TZE204_ya4ft0w4 fingerprint
const stockV3 = {
    ...tuya.definitions.find((d) => d.model === 'ZY-M100-24GV3'),
    fingerprint: [{modelID: 'TS0601', manufacturerName: '_TZE204_ya4ft0w4'}],
    ota: true,
};

const converted = {
    zigbeeModel: ['ZY-M100-ZRD_1'],
    model: 'ZY-M100-24GV2-ZRD',
    vendor: 'Tuya',
    description: '24 GHz presence sensor (ZY-M100-24GV2 / WZ-M100-W)',
    // z2m re-interviews a device after an OTA, but only reconfigures (binds + reporting) when the
    // definition version differs from the one it was last configured with. Bump this whenever
    // configure/reporting changes.
    version: '1.0.3',
    // same hardware, so show z2m's picture of the stock model
    icon: 'https://www.zigbee2mqtt.io/images/devices/ZY-M100-24GV2.png',
    extend: [
        m.occupancy({reportingConfig: {min: 0, max: 3600, change: 0}}),
        m.illuminance({reporting: {min: 60, max: 3600, change: 1000}}),
        m.enumLookup({
            name: 'presence_state',
            cluster: OCC,
            attribute: {ID: 0xf000, type: ENUM8},
            lookup: {none: 0, presence: 1, move: 2},
            description: 'none / presence (still) / move',
            access: 'STATE_GET',
            reporting: {min: 10, max: 3600, change: 0},
        }),
        m.numeric({
            name: 'target_distance',
            cluster: OCC,
            attribute: {ID: 0xf001, type: UINT16},
            description: 'Distance to the target. Only reported when distance_report_interval > 0',
            unit: 'm',
            scale: 100,
            precision: 2,
            access: 'STATE_GET',
        }),
        m.numeric({
            name: 'distance_report_interval',
            cluster: OCC,
            attribute: {ID: 0xf002, type: UINT16},
            description: 'Report target distance at most this often. 0 = off (default; this is what flooded the network)',
            unit: 's',
            valueMin: 0,
            valueMax: 3600,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'distance_deadband',
            cluster: OCC,
            attribute: {ID: 0xf003, type: UINT16},
            description: 'Only report distance after it moved this much',
            unit: 'cm',
            valueMin: 0,
            valueMax: 500,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'distance_offset',
            cluster: OCC,
            attribute: {ID: 0xf009, type: INT16},
            description: 'Calibration: added to every distance reading. Stand at a known distance and adjust until it matches',
            unit: 'cm',
            valueMin: -300,
            valueMax: 300,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'distance_scale',
            cluster: OCC,
            attribute: {ID: 0xf00a, type: UINT16},
            description: 'Calibration: distance is multiplied by this before the offset is added',
            unit: '%',
            valueMin: 50,
            valueMax: 200,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'move_sensitivity',
            cluster: OCC,
            attribute: {ID: 0xf004, type: UINT8},
            valueMin: 0,
            valueMax: 10,
            reporting: onChange,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'presence_sensitivity',
            cluster: OCC,
            attribute: {ID: 0xf005, type: UINT8},
            valueMin: 0,
            valueMax: 10,
            reporting: onChange,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'detection_distance_min',
            cluster: OCC,
            attribute: {ID: 0xf006, type: UINT16},
            description: 'The radar rounds to 0.75 m steps',
            unit: 'm',
            scale: 100,
            valueMin: 0,
            valueMax: 10,
            valueStep: 0.75,
            reporting: onChange,
            entityCategory: 'config',
        }),
        m.numeric({
            name: 'detection_distance_max',
            cluster: OCC,
            attribute: {ID: 0xf007, type: UINT16},
            description: 'The radar rounds to 0.75 m steps',
            unit: 'm',
            scale: 100,
            valueMin: 0.75,
            valueMax: 10,
            valueStep: 0.75,
            reporting: onChange,
            entityCategory: 'config',
        }),
        m.binary({
            name: 'safe_mode',
            cluster: OCC,
            attribute: {ID: 0xf00f, type: UINT8},
            valueOn: [true, 1],
            valueOff: [false, 0],
            description: 'Module started without the radar code (after repeated quick restarts). Zigbee and OTA still work; power-cycle to leave',
            access: 'STATE_GET',
            entityCategory: 'diagnostic',
        }),
        m.numeric({
            name: 'presence_timeout',
            cluster: OCC,
            attribute: {ID: 0xf008, type: UINT16},
            description: 'Max 1500 s on the 24GV2 and 15000 s on the 24GV3, the firmware clamps to the radar\'s limit',
            unit: 's',
            valueMin: 1,
            valueMax: 15000,
            reporting: onChange,
            entityCategory: 'config',
        }),
    ],
    ota: true,
};

export default [stock, stockV3, converted];
