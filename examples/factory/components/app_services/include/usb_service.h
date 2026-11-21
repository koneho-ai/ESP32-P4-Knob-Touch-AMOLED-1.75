/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The USB 2.0 high-speed port is wired to the adapter board's Type-C
 * connector in the orientation opposite to the USB-serial bridge, so the
 * host only sees this device after the plug is flipped.
 *
 * The port is one composite device: a HID keyboard + consumer control
 * (the macropad app) and the microSD card as a mass-storage drive. The
 * two functions are independent — either may run without the other —
 * and the stack stays attached while at least one is active. (It used
 * to also serve a Windows-only extended screen; that was removed on
 * the muse-app branch to save flash — see the branch notes in
 * VENDORED.md.)
 */
typedef enum {
    USB_SERVICE_MODE_OFF,       /*!< Device is detached from the bus */
    USB_SERVICE_MODE_HID,       /*!< HID keyboard + consumer only; no drive */
    USB_SERVICE_MODE_SD_DRIVE,  /*!< microSD card is exposed as a mass-storage drive (HID may also be active) */
} usb_service_mode_t;

typedef enum {
    USB_DRIVE_STATUS_OFF,           /*!< Not in drive mode */
    USB_DRIVE_STATUS_WAITING_HOST,  /*!< Attached, but no host has configured the device yet */
    USB_DRIVE_STATUS_CONNECTED,     /*!< A host is using the drive */
    USB_DRIVE_STATUS_EJECTED,       /*!< The host ejected the drive; it is safe to stop */
} usb_drive_status_t;

/**
 * @brief  Export the microSD card to the USB host
 *
 *         Asks storage_service to release the card first, so applications close
 *         their files before the host sees the drive. May run alongside HID.
 */
esp_err_t usb_service_start_sd_drive(void);

/**
 * @brief  Detach from the host, and give the microSD card back to applications
 *         after drive mode
 *
 *         Stops the drive only; if HID is active the device stays attached
 *         so the keyboard keeps working.
 */
esp_err_t usb_service_stop(void);

usb_service_mode_t usb_service_get_mode(void);
usb_drive_status_t usb_service_get_drive_status(void);

/**
 * @brief  Attach as a HID keyboard + consumer control (macropad)
 *
 *         Starts the USB stack if it is not already up for the drive.
 *         Safe to call while the drive is active.
 */
esp_err_t usb_service_start_hid(void);

/**
 * @brief  Stop the HID function; detaches only if the drive is not active
 */
esp_err_t usb_service_stop_hid(void);

bool usb_service_hid_active(void);

/**
 * @brief  Whether a host has configured the device, so HID reports will be read
 */
bool usb_hid_is_ready(void);

/**
 * @brief  Send one 8-byte keyboard report (modifier byte + 6 key codes)
 *
 *         Returns false when the endpoint is busy — the caller retries.
 *         An all-zero report releases every key. Call from any task;
 *         TinyUSB queues the report for the USB task.
 */
bool usb_hid_keyboard_report(uint8_t modifier, const uint8_t keys[6]);

/**
 * @brief  Send one 2-byte consumer report (one 16-bit usage ID)
 *
 *         Returns false when the endpoint is busy — the caller retries.
 *         Usage 0 releases the key.
 */
bool usb_hid_consumer_report(uint16_t usage);

#ifdef __cplusplus
}
#endif
