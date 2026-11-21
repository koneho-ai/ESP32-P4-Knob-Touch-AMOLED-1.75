/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* TinyUSB configuration for the microSD mass-storage drive and the
 * macropad's USB HID keyboard + consumer control. The vendor and audio
 * classes the Windows-only extended screen used were removed with it on
 * the muse-app branch. */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/
#define CFG_TUSB_DEBUG              0

#define CFG_TUSB_RHPORT1_MODE       (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE      64

/* microSD drive */
#define CFG_TUD_MSC                 1
#define CFG_TUD_MSC_EP_BUFSIZE      (32 * 1024)

/* Macropad HID: instance 0 is the keyboard, instance 1 is consumer control */
#define CFG_TUD_HID                 2
#define CFG_TUD_HID_EP_BUFSIZE      64

#ifdef __cplusplus
}
#endif
