/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MacroPadUi.hpp"

#include <stdio.h>

#include "MacroPadIcons.hpp"

namespace esp_brookesia::apps {

namespace {

// Layout on the 466x466 round display. The grid is centered; the
// outermost key corners fall just outside the circle and are clipped
// by the panel (the icons sit well inside). The header rows and the
// footer stay on the wide chords, clear of the top/bottom arcs.
constexpr int kTitleY = 22;
constexpr int kUsbY = 54;
constexpr int kFooterY = 360;
constexpr int kReloadY = 394;
constexpr int kReloadW = 160;
constexpr int kReloadH = 36;
// The key face is the keycap photo, 72x72 in a 72x80 button (4px flat
// color bands top and bottom). With a label the image zooms to 80% so
// the text fits under it.
constexpr int kFacePx = 72;
constexpr int kFaceZoomLabel = 205; // 205/256 = 80%

constexpr uint32_t kColorBackground = 0x0E0E10;
constexpr uint32_t kColorTitle = 0xE8E8E8;
constexpr uint32_t kColorFooter = 0x9A9AA0;
constexpr uint32_t kColorUsbOk = 0x2E7D32;
constexpr uint32_t kColorUsbMissing = 0xB45309;
constexpr uint32_t kColorReloadBg = 0x2A2A2E;
constexpr uint32_t kColorReloadText = 0xE8E8E8;

// Pressed keycap: the cap color darkened, so the press reads even on
// the black caps (which go darker still).
uint32_t darken(uint32_t color)
{
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);
    r = (uint8_t)((r * 11) / 16);
    g = (uint8_t)((g * 11) / 16);
    b = (uint8_t)((b * 11) / 16);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

lv_obj_t *create_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

} // namespace

bool MacroPadUi::create(lv_obj_t *parent, KeyCallback key_cb, ReloadCallback reload_cb, void *ctx)
{
    _key_cb = key_cb;
    _reload_cb = reload_cb;
    _ctx = ctx;

    _root = create_plain(parent);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_root);
    lv_obj_set_style_bg_color(_root, lv_color_hex(kColorBackground), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "MACROPAD");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTitle), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kTitleY);

    _usb_dot = create_plain(_root);
    lv_obj_set_size(_usb_dot, 12, 12);
    lv_obj_set_style_radius(_usb_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_usb_dot, lv_color_hex(kColorUsbMissing), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_usb_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(_usb_dot, LV_ALIGN_TOP_MID, -62, kUsbY + 3);

    _usb_label = lv_label_create(_root);
    lv_label_set_text(_usb_label, "No USB host");
    lv_obj_set_style_text_font(_usb_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_usb_label, lv_color_hex(kColorFooter), LV_PART_MAIN);
    lv_obj_align(_usb_label, LV_ALIGN_TOP_MID, 8, kUsbY);

    for (int i = 0; i < MacroPadConfig::kKeyCount; i++) {
        int row = i / 5;
        int col = i % 5;
        int x = kGridX + col * (kKeyW + kKeyGap);
        int y = kGridY + row * (kKeyH + kKeyGap);
        KeyView &view = _keys[i];

        view.btn = lv_btn_create(_root);
        lv_obj_set_size(view.btn, kKeyW, kKeyH);
        lv_obj_set_pos(view.btn, x, y);
        lv_obj_set_style_radius(view.btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(view.btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(view.btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(view.btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(view.btn, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
        lv_obj_set_user_data(view.btn, this);
        lv_obj_add_event_cb(view.btn, keyEventCallback, LV_EVENT_PRESSED,
                            reinterpret_cast<void *>((intptr_t)i));
        // Release restores the image opacity (see keyEventCallback).
        lv_obj_add_event_cb(view.btn, keyReleaseCallback, LV_EVENT_RELEASED,
                            reinterpret_cast<void *>((intptr_t)i));
        lv_obj_add_event_cb(view.btn, keyReleaseCallback, LV_EVENT_PRESS_LOST,
                            reinterpret_cast<void *>((intptr_t)i));

        view.img = lv_img_create(view.btn);
        lv_obj_set_size(view.img, kFacePx, kFacePx);
        lv_obj_center(view.img);

        view.label = lv_label_create(view.btn);
        lv_obj_set_style_text_font(view.label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(view.label, lv_color_hex(0xE8E8E8), LV_PART_MAIN);
        lv_label_set_long_mode(view.label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(view.label, kKeyW - 4);
        lv_obj_set_style_text_align(view.label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(view.label, LV_ALIGN_BOTTOM_MID, 0, -3);
        lv_obj_add_flag(view.label, LV_OBJ_FLAG_HIDDEN);
    }

    _footer = lv_label_create(_root);
    lv_label_set_text(_footer, "");
    lv_obj_set_style_text_font(_footer, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_footer, lv_color_hex(kColorFooter), LV_PART_MAIN);
    lv_label_set_long_mode(_footer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_footer, 380);
    lv_obj_set_style_text_align(_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(_footer, LV_ALIGN_TOP_MID, 0, kFooterY);

    lv_obj_t *reload = lv_btn_create(_root);
    lv_obj_set_size(reload, kReloadW, kReloadH);
    lv_obj_align(reload, LV_ALIGN_TOP_MID, 0, kReloadY);
    lv_obj_set_style_radius(reload, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(reload, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(reload, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(reload, lv_color_hex(kColorReloadBg), LV_PART_MAIN);
    lv_obj_add_event_cb(reload, reloadEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *reload_label = lv_label_create(reload);
    lv_label_set_text(reload_label, "Reload config");
    lv_obj_set_style_text_font(reload_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(reload_label, lv_color_hex(kColorReloadText), LV_PART_MAIN);
    lv_obj_center(reload_label);

    return true;
}

void MacroPadUi::destroy()
{
    _root = nullptr;
    _usb_dot = nullptr;
    _usb_label = nullptr;
    _footer = nullptr;
    for (int i = 0; i < MacroPadConfig::kKeyCount; i++) {
        _keys[i] = KeyView{};
    }
}

void MacroPadUi::setConfig(const MacroPadConfig &config)
{
    if (!_root) {
        return;
    }
    for (int i = 0; i < MacroPadConfig::kKeyCount; i++) {
        const MacroPadKey &key = config.keys[i];
        KeyView &view = _keys[i];
        if (!view.btn) {
            continue;
        }
        lv_obj_set_style_bg_color(view.btn, lv_color_hex(key.color), LV_PART_MAIN);
        lv_obj_set_style_bg_color(view.btn, lv_color_hex(darken(key.color)),
                                  LV_STATE_PRESSED);
        const lv_image_dsc_t *dsc = macropad_icon_dsc(key.icon);
        if (dsc) {
            lv_img_set_src(view.img, dsc);
            lv_obj_clear_flag(view.img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(view.img, LV_OBJ_FLAG_HIDDEN);
        }
        bool assigned = key.action.type != MacroPadAction::Type::None;
        view.assigned = assigned;
        lv_obj_set_style_img_opa(view.img, assigned ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        if (key.label[0] != '\0') {
            lv_label_set_text(view.label, key.label);
            lv_obj_clear_flag(view.label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(view.label, assigned ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
            // Icon up a little and zoomed out so the label fits under it.
            lv_img_set_zoom(view.img, kFaceZoomLabel);
            lv_obj_align(view.img, LV_ALIGN_CENTER, 0, -7);
        } else {
            lv_obj_add_flag(view.label, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_zoom(view.img, LV_ZOOM_NONE);
            lv_obj_align(view.img, LV_ALIGN_CENTER, 0, 0);
        }
    }
}

void MacroPadUi::setUsbConnected(bool connected)
{
    if (!_root) {
        return;
    }
    lv_obj_set_style_bg_color(_usb_dot, lv_color_hex(connected ? kColorUsbOk : kColorUsbMissing),
                              LV_PART_MAIN);
    lv_label_set_text(_usb_label, connected ? "USB connected" : "No USB host");
}

void MacroPadUi::setFooter(const char *text)
{
    if (!_footer) {
        return;
    }
    lv_label_set_text(_footer, text ? text : "");
}

void MacroPadUi::keyEventCallback(lv_event_t *e)
{
    // The key index rides in the event user data; the UI object rides on
    // the button itself (see create()).
    intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    MacroPadUi *ui = static_cast<MacroPadUi *>(lv_obj_get_user_data(btn));
    if (!ui || index < 0 || index >= MacroPadConfig::kKeyCount) {
        return;
    }
    // Press feedback: the photo fills the button, so the darkened button
    // background barely shows — dim the photo itself while held.
    KeyView &view = ui->_keys[index];
    if (view.img) {
        lv_obj_set_style_img_opa(view.img, view.assigned ? LV_OPA_70 : LV_OPA_20,
                                 LV_PART_MAIN);
    }
    if (ui->_key_cb) {
        ui->_key_cb((int)index, ui->_ctx);
    }
}

void MacroPadUi::keyReleaseCallback(lv_event_t *e)
{
    intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    MacroPadUi *ui = static_cast<MacroPadUi *>(lv_obj_get_user_data(btn));
    if (!ui || index < 0 || index >= MacroPadConfig::kKeyCount) {
        return;
    }
    KeyView &view = ui->_keys[index];
    if (view.img) {
        lv_obj_set_style_img_opa(view.img, view.assigned ? LV_OPA_COVER : LV_OPA_40,
                                 LV_PART_MAIN);
    }
}

void MacroPadUi::reloadEventCallback(lv_event_t *e)
{
    MacroPadUi *ui = static_cast<MacroPadUi *>(lv_event_get_user_data(e));
    if (ui && ui->_reload_cb) {
        ui->_reload_cb(ui->_ctx);
    }
}

} // namespace esp_brookesia::apps
