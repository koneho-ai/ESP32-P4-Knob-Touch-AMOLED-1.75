/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>

#include "lvgl.h"

namespace esp_brookesia::apps {

/**
 * The Muse settings page (Phase 4.4), built with the factory Settings
 * app's round_ui kit: a scrolling column of cards under a back pill.
 *
 * Content mirrors the SDK's Hatch page (components/muse/muse_settings_ui.c
 * build_hatch_page / tick_hatch) where the factory has an equivalent, with
 * one factory-new piece — the SDK has no VM picker, only a free-text VM ID
 * row, so the picker here is new work on vm_api_fetch_vms:
 *
 *   - Status note: paired or not, the voice session's state (from
 *     muse_hatch_status, as the SDK's tick shows it), and whether Link is
 *     connected.
 *   - VM row: the preferred VM if one is set, else the VM the session last
 *     connected to, else "Default" — the SDK's rule. Opens the picker.
 *   - Device token row: presence only ("Set" / "Not set"). The token is a
 *     credential; none of it ever shows (the SDK shows the same).
 *   - Server row: the provisioned host, or "Default".
 *   - Test connection button: the SDK's muse_hatch_test, via MuseApp.
 *   - Wake word switch (the SDK's Sound-page switch, same note text):
 *     'Listens for "Hey Muse" so you can start a voice note without
 *     holding the talk button.' The switch flips the glue's opt-in
 *     directly — set_enabled is publish-only, so it is safe from the
 *     LVGL task — and refresh() re-syncs it, so a remote
 *     wakeword.disable (Link) shows up too.
 *   - Sign out button: a confirmation dialog, then MuseApp unpairs (the
 *     TokenRevoked sequence). Unlike the SDK's "Reset pairing" this does
 *     NOT forget Wi-Fi or restart: Wi-Fi belongs to the launcher's
 *     Settings app, and the device stays on.
 *
 * The VM picker is a second page (title "Choose a VM"): a "Default VM"
 * row (the SDK's empty VM ID — your account's default) plus one row per
 * VM from fetch_vms, the effective one marked with a check. The list
 * arrives from MuseApp's fetch worker through setVmList; the picker
 * shows "Loading…" until then.
 *
 * All methods run on the LVGL task only: MuseApp drives show/hide from
 * LVGL event handlers and the ui timer, and the fetch worker only fills
 * MuseApp's shared state — it never touches these objects.
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MuseSettingsUi {
public:
    struct Callbacks {
        // Back pill on the settings page: MuseApp hides settings and
        // shows the main view again.
        std::function<void()> onBack;
        // Test connection button: MuseApp runs it on the control task.
        std::function<void()> onTestConnection;
        // Sign out confirmed in the dialog: MuseApp unpairs.
        std::function<void()> onSignOut;
        // VM row tapped: MuseApp shows the picker and fetches the list.
        // Also the picker's Retry button.
        std::function<void()> onOpenVmPicker;
        // A picker row tapped ("" = the account's default VM): MuseApp
        // stores the preference and reconnects.
        std::function<void(const char *vm_id)> onVmPicked;
    };

    struct VmEntry {
        char id[128] = {};
        char name[64] = {};
        bool is_default = false;
    };

    struct Status {
        bool provisioned = false;
        const char *hatch_state = "";    // muse_hatch_state_name()
        const char *hatch_detail = "";   // may be empty
        const char *connected_vm_id = "";  // the VM it last connected to
        const char *preferred_vm_id = "";  // empty = the account's default
        bool token_set = false;
        const char *host = "";           // empty = the default
        bool wakeword_on = false;
        bool link_connected = false;
    };

    static constexpr int kMaxVms = 8;   // VM_API_MAX_VMS

    MuseSettingsUi() = default;
    ~MuseSettingsUi();

    MuseSettingsUi(const MuseSettingsUi &) = delete;
    MuseSettingsUi &operator=(const MuseSettingsUi &) = delete;

    bool create(lv_obj_t *parent, const Callbacks &callbacks);
    void destroy();

    void show();   // settings page, picker closed
    void hide();
    bool visible() const;
    bool pickerVisible() const;

    // Rebuilds the status note and the row values from live state.
    void refresh(const Status &status);
    // Opens the picker page with a "Loading…" note; the list follows
    // through setVmList.
    void showVmPicker();
    // Rebuilds the picker rows (or the error note + Retry button).
    void setVmList(const VmEntry *vms, int count, const char *preferred_vm_id, bool error);
    void hideVmPicker();
    // Knob scrolls the visible column (the Settings app's pattern).
    void knob(int steps);

private:
    static void titleBackCallback(lv_event_t *event);
    static void pickerBackCallback(lv_event_t *event);
    static void vmRowCallback(lv_event_t *event);
    static void testCallback(lv_event_t *event);
    static void wakeCallback(lv_event_t *event);
    static void signOutCallback(lv_event_t *event);
    static void dialogCallback(lv_event_t *event);
    static void vmPickCallback(lv_event_t *event);
    static void retryCallback(lv_event_t *event);

    void setLabelText(lv_obj_t *label, const char *text);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_page = nullptr;
    lv_obj_t *_column = nullptr;
    lv_obj_t *_picker_page = nullptr;
    lv_obj_t *_picker_column = nullptr;
    lv_obj_t *_dialog = nullptr;

    lv_obj_t *_status_note = nullptr;
    lv_obj_t *_vm_value = nullptr;
    lv_obj_t *_token_value = nullptr;
    lv_obj_t *_server_value = nullptr;
    lv_obj_t *_wake_switch = nullptr;

    VmEntry _vms[kMaxVms] = {};
    int _vm_count = 0;
    char _preferred_vm[128] = {};
    char _status_text[192] = {};
    char _vm_text[128] = {};
    char _token_text[16] = {};
    char _server_text[64] = {};

    Callbacks _callbacks;
};

} // namespace esp_brookesia::apps
