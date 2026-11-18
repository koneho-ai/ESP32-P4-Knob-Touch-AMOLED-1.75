/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseUi.hpp"

#include <stdio.h>
#include <string.h>

#include "app_fonts.h"

namespace esp_brookesia::apps {

namespace {

constexpr uint32_t TICK_MS = 33;
constexpr int ORB_Y = -30;              // The orb sits above the centre, the words below it
constexpr int ORB_SIZE = 150;
constexpr int RING_SIZE = 210;
constexpr int CHIP_Y = 92;
constexpr int USER_Y = 292;
constexpr int REPLY_Y = 322;
constexpr int REPLY_W = 250;            // Fits the round screen down to y ~426
constexpr int REPLY_LINES = 4;          // muse_state_page's lines; keep in sync
constexpr int TEXT_W = 320;
constexpr int HINT_Y = 442;
constexpr int HINT_W = 200;
constexpr uint32_t TOAST_MS = 1200;

constexpr uint32_t NAVY = 0x070B18;
constexpr uint32_t TEXT_MAIN = 0xE8EAF0;
constexpr uint32_t TEXT_DIM = 0x7C8598;
constexpr uint32_t TEXT_USER = 0x8E98AA;

float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

uint32_t modeColor(MuseUi::Mode mode)
{
    switch (mode) {
    case MuseUi::Mode::Offline:
        return 0x6B7280;
    case MuseUi::Mode::NotProvisioned:
        return 0xC9A227;
    case MuseUi::Mode::Busy:
        return 0x5B8CFF;
    case MuseUi::Mode::Ready:
        return 0x4F7CFF;
    case MuseUi::Mode::Listening:
        return 0x2EE6A6;
    case MuseUi::Mode::Thinking:
        return 0xFFB84D;
    case MuseUi::Mode::Speaking:
        return 0xA78BFA;
    case MuseUi::Mode::Error:
    default:
        return 0xFF5C7A;
    }
}

void setHidden(lv_obj_t *object, bool hidden)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void setLabelTextIfChanged(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    const char *value = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, value) != 0) {
        lv_label_set_text(label, value);
    }
}

void copyText(char *destination, size_t capacity, const char *source)
{
    snprintf(destination, capacity, "%s", source ? source : "");
}

lv_obj_t *createPlain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return obj;
}

} // namespace

MuseUi::~MuseUi()
{
    destroy();
}

bool MuseUi::preload()
{
    _text_font = app_font_text_20();
    _large_font = app_font_text_30();
    return _text_font && _large_font;
}

bool MuseUi::create(lv_obj_t *parent, ActionCallback action_callback, void *action_context)
{
    destroyView();
    if (!parent || !_text_font || !_large_font) {
        return false;
    }
    _action_callback = action_callback;
    _action_context = action_context;

    _root = createPlain(parent);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_center(_root);
    lv_obj_set_style_bg_color(_root, lv_color_hex(NAVY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_root, clickCallback, LV_EVENT_CLICKED, this);

    // The orb: a coloured circle with a soft glow of the same colour.
    _orb = createPlain(_root);
    lv_obj_set_size(_orb, ORB_SIZE, ORB_SIZE);
    lv_obj_align(_orb, LV_ALIGN_CENTER, 0, ORB_Y);
    lv_obj_set_style_radius(_orb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_orb, lv_color_hex(modeColor(_mode)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_orb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(_orb, 42, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(_orb, lv_color_hex(modeColor(_mode)), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(_orb, LV_OPA_50, LV_PART_MAIN);

    // The ring: an arc around the orb; the tick spins it or grows it with the voice.
    _ring = lv_arc_create(_root);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(_ring, RING_SIZE, RING_SIZE);
    lv_obj_align(_ring, LV_ALIGN_CENTER, 0, ORB_Y);
    lv_arc_set_bg_angles(_ring, 0, 360);
    lv_obj_set_style_arc_opa(_ring, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(modeColor(_mode)), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(modeColor(_mode)), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(_ring, 0, LV_PART_KNOB);

    // The status pill above the orb.
    _chip = createPlain(_root);
    lv_obj_set_height(_chip, 34);
    lv_obj_align(_chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    lv_obj_set_style_radius(_chip, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_chip, lv_color_hex(0x1C2233), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_chip, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(_chip, 0, LV_PART_MAIN);
    _status_label = lv_label_create(_chip);
    lv_obj_set_style_text_font(_status_label, _text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(TEXT_MAIN), LV_PART_MAIN);
    lv_label_set_text(_status_label, _status);
    lv_obj_center(_status_label);
    lv_obj_set_width(_chip, LV_SIZE_CONTENT);

    // What was heard, one line, dim.
    _user_label = lv_label_create(_root);
    lv_obj_set_size(_user_label, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_align(_user_label, LV_ALIGN_TOP_MID, 0, USER_Y);
    lv_obj_set_style_text_font(_user_label, _text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(_user_label, lv_color_hex(TEXT_USER), LV_PART_MAIN);
    lv_obj_set_style_text_align(_user_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(_user_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(_user_label, _user_text);

    // What Muse says, up to 4 lines. This is the caption page the session
    // core pages to (muse_state_page: 26 columns, 4 lines).
    _reply_label = lv_label_create(_root);
    lv_obj_set_size(_reply_label, REPLY_W, REPLY_LINES * lv_font_get_line_height(_text_font));
    lv_obj_align(_reply_label, LV_ALIGN_TOP_MID, 0, REPLY_Y);
    lv_obj_set_style_text_font(_reply_label, _text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(_reply_label, lv_color_hex(TEXT_MAIN), LV_PART_MAIN);
    lv_obj_set_style_text_align(_reply_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(_reply_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(_reply_label, _reply_text);

    // What a tap does now, at the bottom.
    _hint_label = lv_label_create(_root);
    lv_obj_set_size(_hint_label, HINT_W, LV_SIZE_CONTENT);
    lv_obj_align(_hint_label, LV_ALIGN_TOP_MID, 0, HINT_Y);
    lv_obj_set_style_text_font(_hint_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_hint_label, lv_color_hex(TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_align(_hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(_hint_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(_hint_label, _hint);

    _mode_valid = false;
    _tick = lv_timer_create(tickCallback, TICK_MS, this);
    applyMode();
    updateTexts();
    return true;
}

void MuseUi::destroy()
{
    destroyView();
}

void MuseUi::destroyView()
{
    if (_tick) {
        lv_timer_delete(_tick);
        _tick = nullptr;
    }
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
    _orb = nullptr;
    _ring = nullptr;
    _chip = nullptr;
    _status_label = nullptr;
    _user_label = nullptr;
    _reply_label = nullptr;
    _hint_label = nullptr;
}

void MuseUi::setNetworkReady(bool ready)
{
    // The mode already carries the offline state; nothing extra to show.
    (void)ready;
}

void MuseUi::setStatus(const char *status)
{
    copyText(_status, sizeof(_status), status);
    updateTexts();
}

void MuseUi::setMode(Mode mode)
{
    if (_mode != mode) {
        _mode = mode;
        _mode_valid = false;
    }
    applyMode();
}

void MuseUi::setLevel(float level)
{
    _level_target = clamp01(level);
}

void MuseUi::setChatMessage(const char *role, const char *text)
{
    if (role && strcmp(role, "user") == 0) {
        copyText(_user_text, sizeof(_user_text), text);
    } else {
        copyText(_reply_text, sizeof(_reply_text), text);
    }
    updateTexts();
}

void MuseUi::clearChatMessages()
{
    _user_text[0] = '\0';
    _reply_text[0] = '\0';
    updateTexts();
}

void MuseUi::setHint(const char *hint)
{
    copyText(_hint, sizeof(_hint), hint);
    updateTexts();
}

void MuseUi::showToast(const char *text)
{
    if (!_status_label) {
        return;
    }
    setLabelTextIfChanged(_status_label, text);
    if (_chip) {
        lv_obj_set_width(_chip, LV_SIZE_CONTENT);
    }
    _toast_until = lv_tick_get() + TOAST_MS;
}

void MuseUi::applyMode()
{
    if (_mode_valid || !_orb || !_ring) {
        return;
    }
    _mode_valid = true;
    const uint32_t color = modeColor(_mode);
    lv_obj_set_style_bg_color(_orb, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(_orb, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, lv_color_hex(color), LV_PART_INDICATOR);
    // The ring only moves while listening, thinking or speaking.
    setHidden(_ring, _mode != Mode::Listening && _mode != Mode::Thinking &&
                     _mode != Mode::Speaking && _mode != Mode::Busy);
}

void MuseUi::updateTexts()
{
    if (!_root) {
        return;
    }
    // A toast owns the pill until it expires.
    if (_toast_until && lv_tick_get() < _toast_until) {
        // Leave the toast text alone; the tick restores the status.
    } else {
        _toast_until = 0;
        setLabelTextIfChanged(_status_label, _status);
        if (_chip) {
            lv_obj_set_width(_chip, LV_SIZE_CONTENT);
        }
    }
    setLabelTextIfChanged(_user_label, _user_text);
    setLabelTextIfChanged(_reply_label, _reply_text);
    setLabelTextIfChanged(_hint_label, _hint);
}

void MuseUi::animate()
{
    if (!_ring) {
        return;
    }
    // Ease the level towards its target; the ring follows it.
    _level += (_level_target - _level) * 0.35f;
    if (_level < 0.01f && _level_target == 0) {
        _level = 0;
    }
    switch (_mode) {
    case Mode::Thinking:
    case Mode::Busy: {
        // A spinner: a fixed 90-degree arc going around.
        _spin = (_spin + 9) % 360;
        lv_arc_set_angles(_ring, _spin, _spin + 90);
        break;
    }
    case Mode::Listening:
    case Mode::Speaking: {
        // The arc grows with the voice and drifts around slowly.
        _spin = (_spin + 2) % 360;
        const int span = 40 + static_cast<int>(_level * 260);
        lv_arc_set_angles(_ring, _spin, _spin + span);
        break;
    }
    default:
        break;
    }
    // A toast that expired hands the pill back to the status.
    if (_toast_until && lv_tick_get() >= _toast_until) {
        _toast_until = 0;
        setLabelTextIfChanged(_status_label, _status);
        if (_chip) {
            lv_obj_set_width(_chip, LV_SIZE_CONTENT);
        }
    }
}

void MuseUi::tickCallback(lv_timer_t *timer)
{
    auto *instance = static_cast<MuseUi *>(lv_timer_get_user_data(timer));
    if (instance) {
        instance->animate();
    }
}

void MuseUi::clickCallback(lv_event_t *event)
{
    auto *instance = static_cast<MuseUi *>(lv_event_get_user_data(event));
    if (instance && instance->_action_callback) {
        instance->_action_callback(instance->_action_context);
    }
}

} // namespace esp_brookesia::apps
