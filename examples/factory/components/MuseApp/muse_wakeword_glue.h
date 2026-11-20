/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Factory-new glue between the vendored muse_wakeword core (muse_wakeword
 * component, verbatim from the Muse gadget SDK) and the Brookesia MuseApp.
 *
 * Modeled on the SDK's esp32/components/muse/muse_wakeword_glue.c, which
 * cannot be ported as-is: it drives the SDK's muse_voice / muse_input /
 * muse_state tasks, none of which exist in the factory tree. This glue
 * keeps the same shape — a single wake task, a 320-frame tap stream
 * buffer, a 1-second pre-roll ring, a numbered-chunk history for the
 * gap-free handoff — and hands turns to MuseApp through callbacks that
 * post control events.
 *
 * Factory deviations from the SDK glue (each forced by the factory tree):
 * - Turns open through MuseWakewordCallbacks (control events) instead of
 *   synthetic PTT posts to a muse_input queue.
 * - The pre-roll seed is fed by the input task after beginTurn() (the
 *   session resets its mic buffer at turn start, so a seed fed earlier
 *   would be discarded) through the seed_* accessors, instead of
 *   muse_voice_seed_preroll().
 * - Listening is gated on app visibility (muse_wakeword_set_active) in
 *   addition to the opt-in: the wake word only works while MuseApp is
 *   on screen, since the Brookesia launcher releases the app's audio
 *   when it is hidden. There is no background listening.
 * - The tap accumulates the resampler's variable frame counts into exact
 *   320-frame chunks; the SDK tap asserts whole 320-frame chunks.
 *
 * Every entry point is safe to call with the feature compiled out
 * (CONFIG_MUSE_WAKEWORD_ENABLE off): the calls become no-ops, so call
 * sites need no conditionals. The one exception is muse_wakeword_init(),
 * whose stub returns ESP_ERR_NOT_SUPPORTED (the SDK stub returns ESP_OK;
 * the factory's single call site checks the result).
 *
 * Threading: the wake task owns the core and the runner. The input task
 * feeds audio through muse_wakeword_on_audio(). The control task owns turn
 * state and calls the setters. The pre-roll seed is handed to the input
 * task through the seed_* accessors (the wake task never touches the
 * session); the input task feeds it into the session after beginTurn().
 *
 * The wake word is opt-in and defaults OFF, per the SDK's privacy posture
 * ("flashing this firmware never starts listening on its own"). The opt-in
 * is persisted in NVS (namespace "muse_ww", key "enabled") and is turned
 * on from the MuseApp settings page (Phase 4.4). Listening additionally
 * requires the app to be visible (muse_wakeword_set_active).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"

namespace esp_brookesia::apps {

/** Frames in one wake-word chunk (20 ms at 16 kHz). Must match
 * MUSE_WW_CHUNK_FRAMES in the muse_wakeword component. */
#define MUSE_WAKEWORD_CHUNK_FRAMES 320

/** Backlog chunks offered to the input task (32 x 320 frames = 640 ms):
 * the 25-chunk tap plus the few read before the turn starts. A power of
 * two, so the history's slots stay continuous across the number wrap. */
#define MUSE_WAKEWORD_BACKLOG_CHUNKS 32

/** Callbacks the glue invokes from the wake task. The bool-returning
 * callbacks report whether the caller accepted the event; on false the
 * glue aborts the capture rather than leaving the core waiting. */
struct MuseWakewordCallbacks {
    /** A wake was heard and the core is waiting for speech: show a prompt. */
    std::function<void()> onHeard;
    /** Speech started after a wake: open a hands-free turn. The argument
     * is the disable generation at the moment the wake was heard; pass
     * it back to muse_wakeword_turn_wanted() before opening the turn. */
    std::function<bool(uint32_t disables)> onTurnStart;
    /** Capture ended with speech: submit the turn. */
    std::function<bool()> onTurnEnd;
    /** Capture ended unsent (silence, timeout, disabled): drop the turn. */
    std::function<bool()> onTurnCancel;
    /** An armed wake was cancelled before any turn opened: clear the prompt. */
    std::function<void()> onCancelled;
};

/** Status snapshot, same shape as the SDK glue's muse_wakeword_status_t. */
struct MuseWakewordStatus {
    bool enabled;
    bool listening;
    const char *runner;
    int state;   /* muse_ww_state_t, kept as int to stay dependency-free */
    uint32_t wakes;
    uint32_t cancels;
    uint32_t submits;
    uint32_t conversions;
    uint32_t suppressed_wakes;
    uint32_t dropped_frames;   /* tap overload evidence */
};

/** Create the runner, buffers and wake task. Safe to call once; a second
 * call returns ESP_OK immediately. */
esp_err_t muse_wakeword_init(const MuseWakewordCallbacks &callbacks);

/** Feed 16 kHz mono audio from the input task. Accepts any frame count;
 * the glue accumulates exact 320-frame chunks. Ignored while not
 * listening. */
void muse_wakeword_on_audio(const int16_t *pcm, size_t frames);

/** Note a real push-to-talk press (true) or release (false) from the UI.
 * A press cancels an armed wake; a press during a capture converts it to
 * a button-owned turn. Synthetic events are never reported here. */
void muse_wakeword_note_ptt(bool down);

/** Persist the opt-in and turn listening on or off. */
void muse_wakeword_set_enabled(bool on);

/** The persisted opt-in (false until init, or when compiled out). */
bool muse_wakeword_enabled(void);

/** The app is visible and may listen (no persistence). Listening requires
 * enabled && active; the wake task applies the combination. */
void muse_wakeword_set_active(bool active);

/** True when the runner is actually listening right now. */
bool muse_wakeword_listening(void);

/** Tell the core that Muse is speaking (playback suppresses the runner). */
void muse_wakeword_set_playback(bool on);

/** Tell the core a push-to-talk turn is in progress (suppresses the runner
 * and arms the wake task to reset its state when the turn ends). */
void muse_wakeword_set_ptt_turn(bool active);

/** True when a turn requested at the given disable generation is still
 * wanted (the user has not disabled listening since the wake was heard). */
bool muse_wakeword_turn_wanted(uint32_t disables);

/** Note that the current turn ended (submitted or cancelled) so the core
 * can close an open capture and go idle. */
void muse_wakeword_turn_ended(void);

/** Input task only, once per hands-free turn, before anything of it goes:
 * the chunks it had already tapped past the seed's end when the wake task
 * seeded the turn (the wake task can trail by up to a tap's worth), oldest
 * first, so the note continues without a gap. Pointers into the glue's
 * copy, valid until the next tap; *count_out says how many (0 once taken).
 * Returns false if some of them were already overwritten (sustained
 * overload): the note can't be continuous, so the turn must end unsent.
 * Returns true with 0 chunks when nothing is owed: the seed stands alone. */
bool muse_wakeword_take_backlog(
    const int16_t *chunks[], size_t max_chunks, size_t *count_out
);

/** True when a pre-roll seed is waiting to be fed into a turn. */
bool muse_wakeword_seed_ready(void);

/** The generation of the pending seed. The input task only feeds a seed
 * whose generation matches the turn the control task opened. */
uint32_t muse_wakeword_seed_generation(void);

/** The pending seed's audio (valid while muse_wakeword_seed_ready() is
 * true). `frames_out` receives the frame count (<= 16000). */
const int16_t *muse_wakeword_seed_data(size_t *frames_out);

/** Discard the pending seed after feeding or dropping it. */
void muse_wakeword_clear_seed(void);

/** Fill a status snapshot. */
void muse_wakeword_status(MuseWakewordStatus *out);

/** "executorch", or "off" (not compiled in): the runner in logs/status. */
const char *muse_wakeword_runner_kind(void);

}  // namespace esp_brookesia::apps
