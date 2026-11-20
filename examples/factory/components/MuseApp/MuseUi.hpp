/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "muse_state.h"   // muse_mode_t, the avatar's animation mode

namespace esp_brookesia::apps {

/**
 * The Muse page for the round screen: the SDK's animated avatar face in the
 * middle, a 458 px progress ring around the bezel, a 14-segment mic level
 * meter under the avatar while listening, the state name at the top, and a
 * 3-line caption box at the bottom. While a reply (or the user transcript)
 * shows, the avatar shrinks to 256 px to make room, as the SDK's
 * heard-answer layout does.
 *
 * This follows the SDK's full layout (components/muse/muse_ui.c) for the
 * 466 px round boards: the procedural pixel-art avatar (muse_avatar,
 * vendored from the SDK) reaches LVGL through a custom image decoder that
 * scales the 64x64 grid up in 16-row strips, a 40 ms timer builds the pose
 * (mode, level, happiness) and re-renders, and a cell-diff invalidates only
 * the cells that changed. The ring shows phase progress while listening or
 * speaking and a 60-degree spinner while thinking; the meter is a
 * centre-out VU while listening.
 *
 * Talk is tap-anywhere (the whole page is the button, on SHORT_CLICKED so
 * a long press doesn't talk); a long press on the avatar pets it (the SDK's
 * happy reaction, 1.6 s). A long press anywhere else starts a realtime
 * voice call (Phase 4.1) via the hold callback — the SDK's tap/hold split
 * inverted, so tap-to-talk stays the primary path. Deliberate deviations
 * from the SDK: no volume swipe (the knob is the volume control), no
 * speaker/mute button, no settings or latest-reply tile (the Brookesia
 * launcher owns navigation), and one caption box for both the user
 * transcript and the reply page (the SDK has a separate live-caption line;
 * the two boxes would overlap here). The state label uses Montserrat 16 —
 * the factory LVGL has no Unscii, which the SDK uses.
 *
 * The caption box's geometry is the page the session core pages to
 * (muse_state_page in muse_session: 26 columns, 3 lines), and its pixel
 * width is the width the session's pixel pager wraps to — see
 * kCaptionWidthPx / captionGlyphPx, wired up by MuseApp::createUi.
 * Keep them in sync.
 *
 * All LVGL calls on the LVGL task only, except captionGlyphPx, which the
 * session task calls and which only measures a read-only built-in font
 * (or the app font once preload() has loaded it — it is cached forever).
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

    // The caption box's pixel width (the SDK's CAPTION_W): reply pages are
    // wrapped to it by muse_chat_set_caption_px, see captionGlyphPx.
    static constexpr int kCaptionWidthPx = 256;

    // A glyph's advance in the caption font, for the session's pixel pager.
    // Safe from any task: it only measures a read-only font.
    static int captionGlyphPx(uint32_t letter, uint32_t next);

    MuseUi() = default;
    ~MuseUi();

    MuseUi(const MuseUi &) = delete;
    MuseUi &operator=(const MuseUi &) = delete;

    bool preload();
    bool create(
        lv_obj_t *parent,
        ActionCallback action_callback,
        void *action_context,
        ActionCallback hold_callback = nullptr,
        void *hold_context = nullptr
    );
    void destroy();

    void setNetworkReady(bool ready);
    void setStatus(const char *status);
    void setMode(Mode mode);
    /* A call's Muse is busy on work it was asked for: the thinking avatar's
     * working variant (types on a little keyboard). Only read in THINKING. */
    void setWorking(bool working);
    /* Loudness of the voice heard or spoken, 0 to 1 */
    void setLevel(float level);
    /* Phase progress, 0 to 1: recording length while listening, reply
     * playback while speaking. Drives the ring. */
    void setProgress(float progress);
    /* role "user" shows as the transcript, anything else as the reply;
     * empty text clears both. The reply wins when both are set. */
    void setChatMessage(const char *role, const char *text);
    void clearChatMessages();
    /* Shown at the bottom while nothing is being said */
    void setHint(const char *hint);
    /* Replaces the state name for a moment */
    void showToast(const char *text);

    lv_obj_t *root() const
    {
        return _root;
    }

private:
    // SDK geometry (muse_ui.c, full layout, 466 px round screen).
    static constexpr int kScreenPx = 466;
    static constexpr int kTickMs = 40;            // SDK frame_ms
    static constexpr int kCanvasPx = 320;         // full-size avatar (64 cells x 5)
    static constexpr int kRingPx = 458;           // screen - 8
    static constexpr int kRingRange = 1000;
    static constexpr int kMeterSegs = 14;
    static constexpr int kMeterSegPx = 9;
    static constexpr int kMeterGapPx = 4;
    static constexpr int kCaptionLineSpace = 2;
    static constexpr int kArtBlankRows = 3;       // the art never reaches the grid's bottom rows
    static constexpr int kStateY = 40;
    static constexpr int kHintY = 442;
    static constexpr int kHintW = 200;
    static constexpr int kAnswerMs = 300;         // Muse making room for a reply, and back
    static constexpr uint32_t kToastMs = 1200;
    static constexpr float kHappySecs = 1.6f;     // SDK HAPPY_SECS
    static constexpr float kBootSecs = 1.4f;      // the avatar's wake-up animation

    static constexpr uint32_t kColorRingBg = 0x140f22;
    static constexpr uint32_t kColorMeterOff = 0x1d1733;
    static constexpr uint32_t kColorDim = 0x8b84a8;
    static constexpr uint32_t kColorCaption = 0xd8d2ff;
    static constexpr uint32_t kColorHint = 0x7C8598;

    static muse_mode_t avatarMode(Mode mode);
    // How far down a reply `w` px wide can reach on the round screen:
    // inside the ring, above the page dots (SDK muse_ui.c reply_bottom).
    static int replyBottom(int w, int ring_in);

    void destroyView();
    void applyMode();
    void updateTexts();
    void frameTick();
    void updateRing(float now);
    void updateMeter();
    void setMeterVisible(bool visible);
    void invalidateMeter();
    void moveAvatar(int px, int y);
    void moveAvatarStep(int32_t t);
    void setCanvasPx(int px);
    void onPet();
    float happiness() const;

    static void tickCallback(lv_timer_t *timer);
    static void rootEventCallback(lv_event_t *event);
    static void rootHoldCallback(lv_event_t *event);
    static void avatarEventCallback(lv_event_t *event);
    static void moveAnimCallback(void *var, int32_t t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_ring = nullptr;
    lv_obj_t *_avatar = nullptr;
    lv_obj_t *_meter[kMeterSegs] = {};
    lv_obj_t *_state_label = nullptr;
    lv_obj_t *_caption = nullptr;
    lv_obj_t *_hint_label = nullptr;
    lv_timer_t *_tick = nullptr;

    ActionCallback _action_callback = nullptr;
    void *_action_context = nullptr;
    ActionCallback _hold_callback = nullptr;
    void *_hold_context = nullptr;
    const lv_font_t *_text_font = nullptr;

    Mode _mode = Mode::Offline;
    muse_mode_t _avatar_mode = MUSE_MODE_IDLE;
    bool _mode_valid = false;
    bool _working = false;

    float _level_target = 0;
    float _level_smooth = 0;
    float _progress = 0;
    int _ring_value = -1;
    int _shown_lit = -1;
    uint32_t _shown_accent = 0;
    int64_t _mode_since_us = 0;
    int64_t _boot_started_us = 0;
    int64_t _boot_until_us = 0;
    int64_t _happy_until_us = 0;
    uint32_t _toast_until = 0;

    int _avatar_px = kCanvasPx;   // the canvas size now
    int _avatar_y = 0;            // its centre offset now
    int _big_y = 0;               // full-size centre offset (computed in create)
    int _heard_px = 0;            // shrunk size, while a reply shows
    int _heard_y = 0;             // shrunk centre offset
    int _caption_top = 0;         // caption box top, offset from the centre
    bool _caption_shown = false;
    bool _meter_visible = false;

    int _move_from_px = 0;        // shrink/grow animation endpoints
    int _move_to_px = 0;
    int _move_from_y = 0;
    int _move_to_y = 0;

    char _status[64] = {};
    char _hint[96] = {};
    char _user_text[256] = {};
    char _reply_text[512] = {};
};

} // namespace esp_brookesia::apps
