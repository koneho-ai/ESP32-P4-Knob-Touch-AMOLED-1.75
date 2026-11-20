/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseSettingsUi.hpp"

#include <cstdio>
#include <cstring>

#include "muse_wakeword_glue.h"
#include "ui/round_ui.hpp"   // the Settings app's kit (Settings.cpp includes it the same way)

namespace esp_brookesia::apps {

MuseSettingsUi::~MuseSettingsUi()
{
    destroy();
}

bool MuseSettingsUi::create(lv_obj_t *parent, const Callbacks &callbacks)
{
    destroy();
    _callbacks = callbacks;

    _root = round_ui::create_plain(parent);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);

    // ---- The settings page ----
    _page = round_ui::create_plain(_root);
    lv_obj_set_size(_page, lv_pct(100), lv_pct(100));
    round_ui::create_title(_page, "Muse settings", titleBackCallback, this);
    _column = round_ui::create_column(_page);

    _status_note = round_ui::create_note(_column);
    lv_label_set_text(_status_note, "Not paired");

    round_ui::Row vm_row = round_ui::create_row(_column, nullptr, "VM", "Default", true);
    _vm_value = vm_row.value;
    lv_obj_add_event_cb(vm_row.obj, vmRowCallback, LV_EVENT_CLICKED, this);

    round_ui::Row token_row = round_ui::create_row(_column, nullptr, "Device token", "Not set", false);
    _token_value = token_row.value;

    round_ui::Row server_row = round_ui::create_row(_column, nullptr, "Server", "Default", false);
    _server_value = server_row.value;

    round_ui::create_button(_column, nullptr, "Test connection", false, testCallback, this);

    round_ui::create_section(_column, "Hands-free");
    round_ui::Row wake_row = round_ui::create_switch_row(_column, nullptr, "Wake word", wakeCallback, this);
    _wake_switch = wake_row.control;
    lv_obj_t *wake_note = round_ui::create_note(_column);
    lv_label_set_text(wake_note,
                      "Listens for \"Hey Muse\" so you can start a voice note without holding the talk button.");

    round_ui::create_section(_column, "Account");
    round_ui::create_button(_column, nullptr, "Sign out", true, signOutCallback, this);
    lv_obj_t *signout_note = round_ui::create_note(_column);
    lv_label_set_text(signout_note, "Forgets the Muse pairing on this device. Pair again in the Muse app to use it.");

    // ---- The VM picker page ----
    _picker_page = round_ui::create_plain(_root);
    lv_obj_set_size(_picker_page, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(_picker_page, LV_OBJ_FLAG_HIDDEN);
    round_ui::create_title(_picker_page, "Choose a VM", pickerBackCallback, this);
    _picker_column = round_ui::create_column(_picker_page);

    lv_obj_add_flag(_root, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void MuseSettingsUi::destroy()
{
    if (_dialog) {
        // The dialog lives on lv_layer_top, not under _root, so deleting
        // _root would strand it.
        round_ui::dialog_close(_dialog);
        _dialog = nullptr;
    }
    if (_root) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
    _page = nullptr;
    _column = nullptr;
    _picker_page = nullptr;
    _picker_column = nullptr;
    _status_note = nullptr;
    _vm_value = nullptr;
    _token_value = nullptr;
    _server_value = nullptr;
    _wake_switch = nullptr;
    _vm_count = 0;
}

void MuseSettingsUi::show()
{
    if (!_root) {
        return;
    }
    hideVmPicker();
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_HIDDEN);
}

void MuseSettingsUi::hide()
{
    if (_root) {
        lv_obj_add_flag(_root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool MuseSettingsUi::visible() const
{
    return _root && !lv_obj_has_flag(_root, LV_OBJ_FLAG_HIDDEN);
}

bool MuseSettingsUi::pickerVisible() const
{
    return visible() && _picker_page && !lv_obj_has_flag(_picker_page, LV_OBJ_FLAG_HIDDEN);
}

void MuseSettingsUi::setLabelText(lv_obj_t *label, const char *text)
{
    if (label && text) {
        lv_label_set_text(label, text);
    }
}

void MuseSettingsUi::refresh(const Status &status)
{
    if (!_root || !visible()) {
        return;
    }
    // Status note: paired or not, the session's state (the SDK's tick
    // shows state + detail the same way), and Link while paired.
    char text[sizeof(_status_text)] = {};
    if (!status.provisioned) {
        snprintf(text, sizeof(text), "Not paired\nPair in the Muse app");
    } else if (status.hatch_detail && status.hatch_detail[0]) {
        snprintf(text, sizeof(text), "Paired\n%s — %s\nLink %s", status.hatch_state, status.hatch_detail,
                 status.link_connected ? "connected" : "not connected");
    } else {
        snprintf(text, sizeof(text), "Paired\n%s\nLink %s", status.hatch_state,
                 status.link_connected ? "connected" : "not connected");
    }
    if (strcmp(text, _status_text) != 0) {
        snprintf(_status_text, sizeof(_status_text), "%s", text);
        setLabelText(_status_note, text);
    }

    // One set here, else the one it connected to (the SDK's rule).
    const char *vm = status.preferred_vm_id && status.preferred_vm_id[0] ? status.preferred_vm_id
                     : status.connected_vm_id && status.connected_vm_id[0] ? status.connected_vm_id
                                                                            : "Default";
    if (strcmp(vm, _vm_text) != 0) {
        snprintf(_vm_text, sizeof(_vm_text), "%s", vm);
        setLabelText(_vm_value, vm);
    }

    const char *token = status.token_set ? "Set" : "Not set";
    if (strcmp(token, _token_text) != 0) {
        snprintf(_token_text, sizeof(_token_text), "%s", token);
        setLabelText(_token_value, token);
    }

    const char *host = status.host && status.host[0] ? status.host : "Default";
    if (strcmp(host, _server_text) != 0) {
        snprintf(_server_text, sizeof(_server_text), "%s", host);
        setLabelText(_server_value, host);
    }

    round_ui::set_switch(_wake_switch, status.wakeword_on);
}

void MuseSettingsUi::showVmPicker()
{
    if (!_root) {
        return;
    }
    lv_obj_add_flag(_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_picker_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(_picker_column);
    lv_obj_t *loading = round_ui::create_note(_picker_column);
    lv_label_set_text(loading, "Loading VMs…");
}

void MuseSettingsUi::setVmList(const VmEntry *vms, int count, const char *preferred_vm_id, bool error)
{
    if (!_picker_column || !pickerVisible()) {
        return;
    }
    lv_obj_clean(_picker_column);
    _vm_count = 0;
    snprintf(_preferred_vm, sizeof(_preferred_vm), "%s", preferred_vm_id ? preferred_vm_id : "");

    if (error || !vms) {
        lv_obj_t *note = round_ui::create_note(_picker_column);
        lv_label_set_text(note, "Couldn't load VMs. Check the connection and try again.");
        round_ui::create_button(_picker_column, nullptr, "Retry", false, retryCallback, this);
        return;
    }
    if (count > kMaxVms) {
        count = kMaxVms;
    }
    for (int i = 0; i < count; i++) {
        _vms[i] = vms[i];
    }
    _vm_count = count;

    // The SDK's empty VM ID: your account's default VM.
    const bool default_picked = _preferred_vm[0] == '\0';
    round_ui::Row def_row = round_ui::create_row(_picker_column, default_picked ? LV_SYMBOL_OK : nullptr, "Default VM",
                                                 "Your account's default", true);
    lv_obj_set_user_data(def_row.obj, reinterpret_cast<void *>(static_cast<intptr_t>(0)));
    lv_obj_add_event_cb(def_row.obj, vmPickCallback, LV_EVENT_CLICKED, this);

    for (int i = 0; i < count; i++) {
        const bool picked = (_preferred_vm[0] != '\0' && strcmp(_vms[i].id, _preferred_vm) == 0) ||
                            (_preferred_vm[0] == '\0' && _vms[i].is_default);
        const char *title = _vms[i].name[0] ? _vms[i].name : _vms[i].id;
        round_ui::Row row = round_ui::create_row(_picker_column, picked ? LV_SYMBOL_OK : nullptr, title, _vms[i].id,
                                                 true);
        lv_obj_set_user_data(row.obj, reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)));
        lv_obj_add_event_cb(row.obj, vmPickCallback, LV_EVENT_CLICKED, this);
    }
}

void MuseSettingsUi::hideVmPicker()
{
    if (_picker_page) {
        lv_obj_add_flag(_picker_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (_page) {
        lv_obj_remove_flag(_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void MuseSettingsUi::knob(int steps)
{
    round_ui::scroll_column(pickerVisible() ? _picker_column : _column, steps);
}

void MuseSettingsUi::titleBackCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    if (ui->_callbacks.onBack) {
        ui->_callbacks.onBack();
    }
}

void MuseSettingsUi::pickerBackCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    ui->hideVmPicker();
}

void MuseSettingsUi::vmRowCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    if (ui->_callbacks.onOpenVmPicker) {
        ui->_callbacks.onOpenVmPicker();
    }
}

void MuseSettingsUi::testCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    if (ui->_callbacks.onTestConnection) {
        ui->_callbacks.onTestConnection();
    }
}

void MuseSettingsUi::wakeCallback(lv_event_t *event)
{
    // Publish-only (the wake task applies the transition), so this is
    // safe straight from the LVGL task. The event was registered on the
    // switch itself, so the current target is the switch.
    muse_wakeword_set_enabled(
        lv_obj_has_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED));
}

void MuseSettingsUi::signOutCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    if (ui->_dialog) {
        return;
    }
    ui->_dialog = round_ui::dialog_open(
        "Sign out", "This forgets the Muse pairing on this device. Pair again in the Muse app to use it.", "Sign out",
        true, dialogCallback, ui);
}

void MuseSettingsUi::dialogCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    const bool confirmed = round_ui::dialog_confirmed(event);
    round_ui::dialog_close(ui->_dialog);
    ui->_dialog = nullptr;
    if (confirmed && ui->_callbacks.onSignOut) {
        ui->_callbacks.onSignOut();
    }
}

void MuseSettingsUi::vmPickCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
    const char *id = "";
    if (index > 0 && index <= ui->_vm_count) {
        id = ui->_vms[index - 1].id;
    }
    ui->hideVmPicker();
    if (ui->_callbacks.onVmPicked) {
        ui->_callbacks.onVmPicked(id);
    }
}

void MuseSettingsUi::retryCallback(lv_event_t *event)
{
    auto *ui = static_cast<MuseSettingsUi *>(lv_event_get_user_data(event));
    if (ui->_callbacks.onOpenVmPicker) {
        ui->_callbacks.onOpenVmPicker();
    }
}

} // namespace esp_brookesia::apps
