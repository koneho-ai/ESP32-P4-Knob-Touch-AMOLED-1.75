/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TinyUSB descriptors for the microSD mass-storage drive. This file used
 * to also describe the Windows-only extended screen (vendor + HID + audio
 * interfaces); that was removed on the muse-app branch to save flash, and
 * only the MSC descriptors remain.
 */

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tusb.h"

/* Espressif VID; the PID TinyUSB assigns to an MSC-only device */
#define USB_VID             0x303A
#define USB_PID_SD_DRIVE    0x4002

#define EPNUM_MSC_OUT       0x01
#define EPNUM_MSC_IN        0x81
#define MSC_EP_SIZE_HS      512
#define MSC_EP_SIZE_FS      64

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MSC,
};

/* ---------------------------------------------------------------------------
 * Device
 * ------------------------------------------------------------------------- */

static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID_SD_DRIVE,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static const tusb_desc_device_qualifier_t s_desc_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00,
};

/* ---------------------------------------------------------------------------
 * microSD drive
 * ------------------------------------------------------------------------- */

#define MSC_CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_desc_msc_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CONFIG_TOTAL_LEN, 0, 500),
    TUD_MSC_DESCRIPTOR(0, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE_HS),
};

static const uint8_t s_desc_msc_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CONFIG_TOTAL_LEN, 0, 500),
    TUD_MSC_DESCRIPTOR(0, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE_FS),
};

/* Other-speed descriptor: same layout, but the descriptor type is OTHER_SPEED_CONFIG */
static uint8_t s_desc_other_speed[MSC_CONFIG_TOTAL_LEN];

/* ---------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_desc_device;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&s_desc_qualifier;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return tud_speed_get() == TUSB_SPEED_HIGH ? s_desc_msc_hs : s_desc_msc_fs;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    const uint8_t *other = tud_speed_get() == TUSB_SPEED_HIGH ? s_desc_msc_fs : s_desc_msc_hs;
    memcpy(s_desc_other_speed, other, MSC_CONFIG_TOTAL_LEN);
    s_desc_other_speed[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return s_desc_other_speed;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc_str[64];
    static char serial[13];
    const char *str;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        desc_str[1] = 0x0409;
        chr_count = 1;
        desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
        return desc_str;
    case STRID_MANUFACTURER:
        str = "Waveshare";
        break;
    case STRID_PRODUCT:
        str = "ESP32-P4 Knob microSD";
        break;
    case STRID_SERIAL:
        if (serial[0] == '\0') {
            uint8_t mac[6] = {0};
            esp_efuse_mac_get_default(mac);
            snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        str = serial;
        break;
    case STRID_MSC:
        str = "microSD";
        break;
    default:
        return NULL;
    }

    chr_count = strlen(str);
    if (chr_count > 63) {
        chr_count = 63;
    }
    for (size_t i = 0; i < chr_count; i++) {
        desc_str[1 + i] = str[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
