/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TinyUSB descriptors for the composite device: a USB HID keyboard and
 * consumer control (the macropad app) plus the microSD mass-storage
 * drive. All three interfaces are always in the configuration, so the
 * host enumerates them once at attach; the MSC interface reports
 * "medium not present" until the SD drive is enabled, and HID reports
 * only flow while the macropad app runs. This file used to also
 * describe the Windows-only extended screen (vendor + HID + audio
 * interfaces); that was removed on the muse-app branch to save flash,
 * and the HID here is a new, unrelated keyboard/consumer pair.
 */

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tusb.h"

/* Espressif VID; the PID changes with the configuration, so a host that
 * saw the MSC-only device re-enumerates this composite one cleanly. */
#define USB_VID             0x303A
#define USB_PID_COMPOSITE   0x4003

/* Interface numbers: HID instances follow this order (instance 0 is the
 * keyboard, instance 1 is consumer control). */
#define ITF_HID_KEYBOARD    0
#define ITF_HID_CONSUMER    1
#define ITF_MSC             2
#define ITF_COUNT           3

#define EPNUM_MSC_OUT       0x01
#define EPNUM_MSC_IN        0x81
#define EPNUM_HID_KB_IN     0x82
#define EPNUM_HID_CC_IN     0x83
#define MSC_EP_SIZE_HS      512
#define MSC_EP_SIZE_FS      64
#define HID_KB_EP_SIZE      8
#define HID_CC_EP_SIZE      2
/* Interrupt polling: 10 ms at full speed; at high speed the interval is
 * 2^(n-1) * 125 us, so 7 is 8 ms. */
#define HID_EP_INTERVAL_HS  7
#define HID_EP_INTERVAL_FS  10

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_HID_KB,
    STRID_HID_CC,
    STRID_MSC,
};

/* ---------------------------------------------------------------------------
 * HID report descriptors
 * ------------------------------------------------------------------------- */

/* Standard boot keyboard: 8-byte report (modifiers, reserved, 6 keys),
 * plus a 1-byte LED output report the host may send (ignored). */
static const uint8_t s_desc_hid_keyboard[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Minimum (224) */
    0x29, 0xE7,        /*   Usage Maximum (231) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) ; modifier byte */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x03,        /*   Input (Constant) ; reserved byte */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x05,        /*   Usage Maximum (5) */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute) ; LED report */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x03,        /*   Report Size (3) */
    0x91, 0x03,        /*   Output (Constant) ; LED report padding */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x65,        /*   Logical Maximum (101) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0x65,        /*   Usage Maximum (101) */
    0x81, 0x00,        /*   Input (Data, Array) ; key array (6 bytes) */
    0xC0,              /* End Collection */
};

/* Consumer control: 2-byte report carrying one 16-bit usage ID
 * (0 = released). Covers media keys and volume. */
static const uint8_t s_desc_hid_consumer[] = {
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03,  /*   Logical Maximum (1023) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03,  /*   Usage Maximum (1023) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x00,        /*   Input (Data, Array) */
    0xC0,              /* End Collection */
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
    .idProduct = USB_PID_COMPOSITE,
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
 * Composite configuration: HID keyboard + HID consumer + microSD drive
 * ------------------------------------------------------------------------- */

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_desc_config_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_HID_DESCRIPTOR(ITF_HID_KEYBOARD, STRID_HID_KB, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_desc_hid_keyboard), EPNUM_HID_KB_IN, HID_KB_EP_SIZE, HID_EP_INTERVAL_HS),
    TUD_HID_DESCRIPTOR(ITF_HID_CONSUMER, STRID_HID_CC, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_desc_hid_consumer), EPNUM_HID_CC_IN, HID_CC_EP_SIZE, HID_EP_INTERVAL_HS),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE_HS),
};

static const uint8_t s_desc_config_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_HID_DESCRIPTOR(ITF_HID_KEYBOARD, STRID_HID_KB, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_desc_hid_keyboard), EPNUM_HID_KB_IN, HID_KB_EP_SIZE, HID_EP_INTERVAL_FS),
    TUD_HID_DESCRIPTOR(ITF_HID_CONSUMER, STRID_HID_CC, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_desc_hid_consumer), EPNUM_HID_CC_IN, HID_CC_EP_SIZE, HID_EP_INTERVAL_FS),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE_FS),
};

/* Other-speed descriptor: same layout, but the descriptor type is OTHER_SPEED_CONFIG */
static uint8_t s_desc_other_speed[CONFIG_TOTAL_LEN];

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
    return tud_speed_get() == TUSB_SPEED_HIGH ? s_desc_config_hs : s_desc_config_fs;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    const uint8_t *other = tud_speed_get() == TUSB_SPEED_HIGH ? s_desc_config_fs : s_desc_config_hs;
    memcpy(s_desc_other_speed, other, CONFIG_TOTAL_LEN);
    s_desc_other_speed[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return s_desc_other_speed;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (instance == ITF_HID_KEYBOARD) {
        return s_desc_hid_keyboard;
    }
    if (instance == ITF_HID_CONSUMER) {
        return s_desc_hid_consumer;
    }
    return NULL;
}

/* The host never needs to read a report back from this device; STALL the request. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

/* Keyboard LED output (Caps Lock etc.) and consumer SET_REPORT: ignored. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
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
        str = "ESP32-P4 Knob";
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
    case STRID_HID_KB:
        str = "Keyboard";
        break;
    case STRID_HID_CC:
        str = "Consumer Control";
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
