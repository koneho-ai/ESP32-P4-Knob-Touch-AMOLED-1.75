/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

namespace esp_brookesia::apps {

/**
 * The Muse page for the round screen: a coloured orb in the middle, a ring
 * around it that spins while Muse thinks and grows with the voice while
 * listening or speaking, the state in a pill above and what is said below.
 *
 * Layout (466x466, all LVGL calls on the LVGL task only):
 *   status pill  y ~92      | orb 150px, centre y 203 | ring 210px
 *   user line    y ~292     | reply text y ~322, 260px wide, 4 lines
 *   hint         y ~442
 *
 * The reply label's geometry is the caption page the session core pages to
 * (muse_state_page in muse_session: 26 columns, 4 lines). Keep them in sync.
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MuseUi {
public:
    using ActionCallback = void (*)(void *context);

    enum class Mode : uint8_t {
        Offline,
        NotProvisioned, // No device tokens yet: pair in the Muse app
        Busy,           // Preparing or connecting
        Ready,
        Listening,
        Thinking,
        Speaking,
        Error,
    };

    MuseUi() = default;
    ~MuseUi();

    MuseUi(const MuseUi &) = delete;
    MuseUi &operator=(const MuseUi &) = delete;

    bool preload();
    bool create(
        lv_obj_t *parent,
        ActionCallback action_callback,
        void *action_context
    );
    void destroy();

    void setNetworkReady(bool ready);
    void setStatus(const char *status);
    void setMode(Mode mode);
    /* Loudness of the voice heard or spoken, 0 to 1 */
    void setLevel(float level);
    /* role "user" shows in the user line, anything else in the reply box */
    void setChatMessage(const char *role, const char *text);
    void clearChatMessages();
    /* Shown below the orb while nothing is being said */
    void setHint(const char *hint);
    /* Replaces the state above the orb for a moment */
    void showToast(const char *text);

    lv_obj_t *root() const
    {
        return _root;
    }

private:
    void destroyView();
    void applyMode();
    void updateTexts();
    void animate();
    static void tickCallback(lv_timer_t *timer);
    static void clickCallback(lv_event_t *event);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_orb = nullptr;
    lv_obj_t *_ring = nullptr;
    lv_obj_t *_chip = nullptr;
    lv_obj_t *_status_label = nullptr;
    lv_obj_t *_user_label = nullptr;
    lv_obj_t *_reply_label = nullptr;
    lv_obj_t *_hint_label = nullptr;
    lv_timer_t *_tick = nullptr;

    ActionCallback _action_callback = nullptr;
    void *_action_context = nullptr;
    const lv_font_t *_text_font = nullptr;
    const lv_font_t *_large_font = nullptr;

    Mode _mode = Mode::Offline;
    bool _mode_valid = false;

    float _level_target = 0;
    float _level = 0;
    int _spin = 0;
    uint32_t _toast_until = 0;
    char _status[64] = {};
    char _hint[96] = {};
    char _user_text[256] = {};
    char _reply_text[512] = {};
};

} // namespace esp_brookesia::apps
