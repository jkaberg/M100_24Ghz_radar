/********************************************************************************************************
 * @file    app_cfg.h
 *
 * @brief   This is the header file for app_cfg
 *
 * @author  Zigbee Group
 * @date    2021
 *
 * @par     Copyright (c) 2021, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *			All rights reserved.
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 *******************************************************************************************************/

#pragma once

/* Enable C linkage for C++ Compilers: */
#if defined(__cplusplus)
extern "C" {
#endif

#include "app_types.h"

#define ON                      1
#define OFF                     0

#ifndef MCU_CORE_8258
#define MCU_CORE_8258   1
#endif

#define MY_RF_POWER_INDEX   RF_POWER_INDEX_P10p46dBm

/* for reporting */
/* default ZCL reporting (z2m may reconfigure) */
#define REPORTING_MAX       3600            /* 60 min heartbeat */

/**********************************************************************
 * Product Information
 */

#define ZCL_BASIC_MFG_NAME     {9,'R','a','d','a','r','-','Z','R','D'}
/* Matched by the z2m converter (z2m/zy-m100-zrd.mjs) */
#define ZCL_BASIC_MODEL_ID     {13,'Z','Y','-','M','1','0','0','-','Z','R','D','_','1'}


/**********************************************************************
 * Version configuration
 */
#include "version_cfg.h"

/* Debug mode config */
/* Off: the debug pin (PB5) is a module pad of unknown use on the radar PCB */
#define	UART_PRINTF_MODE                OFF
#define USB_PRINTF_MODE         		OFF

#define DEBUG_PKT                       OFF
#define DEBUG_PKT_FILTER                OFF
#define DEBUG_CMD                       OFF
#define DEBUG_DP                        OFF
#define DEBUG_TIME                      OFF
#define DEBUG_SAVE                      OFF
#define DEBUG_SCHEDULE                  OFF
#define DEBUG_PM                        OFF
#define DEBUG_REPORTING                 OFF
#define DEBUG_OTA                       OFF
#define DEBUG_STA_STATUS                OFF

/* HCI interface */
#define ZBHCI_UART                      OFF

/* PM */
#define PM_ENABLE						OFF

/* PA */
#define PA_ENABLE						OFF

/* BDB */
#define TOUCHLINK_SUPPORT				OFF
#define FIND_AND_BIND_SUPPORT			OFF

/* Board ID */
#define BOARD_826x_EVK                  0
#define BOARD_826x_DONGLE               1
#define BOARD_826x_DONGLE_PA            2
#define BOARD_8258_EVK                  3
#define BOARD_8258_EVK_V1P2             4//C1T139A30_V1.2
#define BOARD_8258_DONGLE               5
#define BOARD_8278_EVK                  6
#define BOARD_8278_DONGLE               7
#define BOARD_B91_EVK                   8
#define BOARD_B91_DONGLE                9
#define BOARD_TUYA_ZT3L                 10

/* Board define */
#if defined(MCU_CORE_826x)
#if !PA_ENABLE
    #define BOARD                       BOARD_826x_DONGLE
#else
    #define BOARD                       BOARD_826x_DONGLE_PA
#endif
    #define CLOCK_SYS_CLOCK_HZ          32000000
#elif defined(MCU_CORE_8258)
#if (CHIP_TYPE == TLSR_8258_1M)
    #define FLASH_CAP_SIZE_1M           1
#endif
    #define BOARD                       BOARD_TUYA_ZT3L
    #define CLOCK_SYS_CLOCK_HZ          48000000
    #define NV_ITEM_APP_USER_CFG        (NV_ITEM_APP_GP_TRANS_TABLE + 1)    // see sdk/proj/drivers/drv_nv.h
    #define NV_ITEM_APP_BOOT_CNT        (NV_ITEM_APP_GP_TRANS_TABLE + 2)    /* safe-mode boot counter */
    #define NV_ITEM_APP_DIST_CAL        (NV_ITEM_APP_GP_TRANS_TABLE + 3)    /* distance calibration */
#elif defined(MCU_CORE_8278)
    #define FLASH_CAP_SIZE_1M           1
    #define BOARD                       BOARD_8278_DONGLE//BOARD_8278_EVK
    #define CLOCK_SYS_CLOCK_HZ          48000000
#elif defined(MCU_CORE_B91)
    #define FLASH_CAP_SIZE_1M           1
    #define BOARD                       BOARD_B91_DONGLE//BOARD_B91_EVK
    #define CLOCK_SYS_CLOCK_HZ          48000000
#else
    #error "MCU is undefined!"
#endif

/* Board include */
#if (BOARD == BOARD_826x_EVK)
    #include "board_826x_evk.h"
#elif (BOARD == BOARD_826x_DONGLE)
    #include "board_826x_dongle.h"
#elif (BOARD == BOARD_826x_DONGLE_PA)
    #include "board_826x_dongle_pa.h"
#elif (BOARD == BOARD_8258_DONGLE)
    #include "board_8258_dongle.h"
#elif (BOARD == BOARD_8258_EVK)
    #include "board_8258_evk.h"
#elif (BOARD == BOARD_8258_EVK_V1P2)
    #include "board_8258_evk_v1p2.h"
#elif (BOARD == BOARD_8278_EVK)
    #include "board_8278_evk.h"
#elif (BOARD == BOARD_8278_DONGLE)
    #include "board_8278_dongle.h"
#elif (BOARD == BOARD_B91_EVK)
    #include "board_b91_evk.h"
#elif (BOARD == BOARD_B91_DONGLE)
    #include "board_b91_dongle.h"
#elif (BOARD == BOARD_TUYA_ZT3L)
    #include "board_tuya.h"
#endif


/* Voltage detect module */
/* If VOLTAGE_DETECT_ENABLE is set,
 * 1) if MCU_CORE_826x is defined, the DRV_ADC_VBAT_MODE mode is used by default,
 * and there is no need to configure the detection IO port;
 * 2) if MCU_CORE_8258 or MCU_CORE_8278 is defined, the DRV_ADC_VBAT_MODE mode is used by default,
 * we need to configure the detection IO port, and the IO must be in a floating state.
 * 3) if MCU_CORE_B91 is defined, the DRV_ADC_BASE_MODE mode is used by default,
 * we need to configure the detection IO port, and the IO must be connected to the target under test,
 * such as VCC.
 */
/* Off: mains powered, and the boot-time check would reset-loop on a bad reading. The pin below is
 * still used: on Zbit flash chips the SDK refuses every flash write (NV, OTA) unless an ADC
 * reading on it is above 2.2 V (platform/chip_8258/flash.c flash_mspi_write_ram). */
#define VOLTAGE_DETECT_ENABLE						OFF

#if defined(MCU_CORE_826x)
    #define VOLTAGE_DETECT_ADC_PIN                  0
#elif defined(MCU_CORE_8258) || defined(MCU_CORE_8278)
    /* The SDK drives this pin high and measures it (adc_vbat_pin_init), so it must float.
     * PC5 is not bonded on the TLSR8258F1KET32 (DS-TLSR8258 Table 1-3), hence not on any ZT3L
     * pad, and is what pvvx/ZigbeeTLc and devbis/z03mmc use on Zbit-flash devices. */
    #define VOLTAGE_DETECT_ADC_PIN                  GPIO_PC5
#elif defined(MCU_CORE_B91)
    #define VOLTAGE_DETECT_ADC_PIN                  ADC_GPIO_PB0
#endif


/* Watch dog module */
#define MODULE_WATCHDOG_ENABLE						ON

/* UART module */
#define	MODULE_UART_ENABLE							OFF

#if (ZBHCI_USB_PRINT || ZBHCI_USB_CDC || ZBHCI_USB_HID || ZBHCI_UART)
	#define ZBHCI_EN								1
#endif

/**********************************************************************
 * ZCL cluster support setting
 */
#define ZCL_GROUP_SUPPORT                           ON
#define ZCL_OTA_SUPPORT                             ON
#define ZCL_GP_SUPPORT                              ON
#define ZCL_OCCUPANCY_SENSING_SUPPORT               ON
#define ZCL_ILLUMINANCE_MEASUREMENT_SUPPORT         ON



/**********************************************************************
 * Stack configuration
 */
#include "stack_cfg.h"


/**********************************************************************
 * EV configuration
 */
typedef enum{
	EV_POLL_ED_DETECT,
	EV_POLL_HCI,
    EV_POLL_IDLE,
	EV_POLL_MAX,
}ev_poll_e;

/* Disable C linkage for C++ Compilers: */
#if defined(__cplusplus)
}
#endif
