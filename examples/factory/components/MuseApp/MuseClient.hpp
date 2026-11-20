/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "esp_err.h"
#include "muse_chat.h"
#include "muse_credentials.h"
#include "muse_voice_call.h"

namespace esp_brookesia::apps {

/**
 * Application-facing adapter for the vendored Muse chat session
 * (muse_session component) and the account store (muse_vm component).
 *
 * Board audio, resampling, UI, and application state stay outside this
 * class. This adapter only translates the session core's C API into the
 * app's control events, and owns the token-refresh retry.
 *
 * Audio rates: the session takes 16 kHz mono speech (muse_hatch_turn_audio)
 * and returns 16 kHz mono reply audio (muse_hatch_turn_read). The app
 * resamples between the codec's 24 kHz stereo and these 16 kHz mono ends.
 * The session itself upsamples to 24 kHz for the dictation upload, or sends
 * the note as a 16 kHz WAV voice note (the SDK's VOICE_NOTE default).
 *
 * Replies in v1 are TEXT: the SDK's start_tts plays silence paced to the
 * text ("Muse doesn't speak gadget replies" — a TTS API of your own would
 * plug in there). readReply() still returns the paced silence, which the
 * app plays so the captions advance at reading pace.
 *
 * Realtime calls (Phase 4.1) are different: Muse's speech arrives as real
 * 16 kHz audio over POST /voice/realtime, and the call task (MuseApp)
 * pumps it. Half-duplex on this board — the mic stays shut while Muse is
 * audibly talking, plus a 300 ms tail (see VENDORED.md).
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */
class MuseClient final {
public:
    struct Callbacks {
        // Callback payloads are copied by the app before returning.
        // All callbacks run on the task that calls pollEvents().
        std::function<void(const std::string &text)> onHeard;   // Transcript so far
        std::function<void(const std::string &text)> onReply;   // Reply text so far
        // Turn ended; empty reason = complete, nonempty = incomplete reason.
        std::function<void(const std::string &reason)> onDone;
        std::function<void(const std::string &error)> onError;  // Turn failed
        std::function<void()> onSent;                           // The VM acked the note
    };

    explicit MuseClient(Callbacks callbacks = {});
    ~MuseClient();

    MuseClient(const MuseClient &) = delete;
    MuseClient &operator=(const MuseClient &) = delete;
    MuseClient(MuseClient &&) = delete;
    MuseClient &operator=(MuseClient &&) = delete;

    // Loads the credentials and starts the session task (once). Safe to
    // call again; the session task is a singleton.
    esp_err_t start();
    // Drops the connection and the cached VM credentials. The session task
    // keeps running (it reconnects on the next turn or test).
    void stop();

    bool isProvisioned() const;
    // Token set and Wi-Fi up: a press can start a turn.
    bool isReady() const;
    // Asks the session task to (re)connect now.
    void testConnection();
    // Call when host/VM/token change: forgets the last result and the connection.
    void configChanged();
    muse_hatch_status_t status();

    // ---- Push-to-talk turns (voice task only in the SDK's terms: the app's
    // control task begins/ends turns, the input task feeds audio) ----
    void beginTurn();
    // 16 kHz mono speech, in order, from press to release.
    void feedAudio(const int16_t *pcm, size_t frames);
    void endTurn();
    // Abandons the turn (tap, or barge-in during the reply).
    void cancelTurn();

    // Drains turn events into the callbacks. Call regularly from the control
    // task while a turn is active (and briefly after it ends).
    void pollEvents();

    // True once a reply has come for a request whose turn ended before it;
    // it then starts that reply's turn, to be played like any reply.
    bool checkIncoming();

    // Reply speech as 16 kHz mono (silence-paced text in v1). Waits up to
    // wait_ms for some; returns frames read.
    size_t readReply(int16_t *pcm, size_t frames, int wait_ms);

    // The page of reply text holding what's being said after `played` frames
    // of reply speech. False until there's reply text to page.
    bool captionAt(size_t played, char *out, size_t cap);

    // Approximate reply progress at the same playback cursor as captions,
    // 0..1 (1 = this message has drained; the ring caps at 99% until the
    // whole turn succeeds). Call from the control task, like captionAt.
    float turnProgress(size_t played);

    // ---- Realtime voice calls (Phase 4.1; CONFIG_MUSE_REALTIME_VOICE) ----
    // Hands-free only: a tap starts a call, the next press ends it. All
    // audio is 16 kHz mono; the call resamples to/from the wire's 24 kHz.
    // start/end/audio/read/event MUST all run on one task — the app's call
    // task (see MuseApp::callAudio) — the call core keeps plain non-atomic
    // state and a press fence that assume a single reader. With the option
    // off these are safe no-ops (callReady() false, reads return 0).
    bool callReady() const;
    void startCall();
    void endCall();
    // Mic audio for the call, as captured. False if dropped.
    bool feedCallAudio(const int16_t *pcm, size_t frames);
    // Muse's speech, without waiting. *flushed: the caller barged in;
    // drop whatever is buffered downstream too.
    size_t readCallAudio(int16_t *pcm, size_t frames, bool *flushed);
    // Non-blocking event drain; events of earlier calls are dropped.
    muse_voice_call_ev_t pollCallEvent(char *text, size_t cap);
    // Live, and Muse still has a caller turn open — drives the WORKING pose.
    bool callWorking() const;

    // Refreshes the device-token pair via the account API (access leg, then
    // refresh leg) and stores the new pair. Blocking HTTPS: call from a task
    // that may block for seconds, never from the preempt callback or the
    // LVGL task. On OK the caller should call configChanged() +
    // testConnection(); on REVOKED the pairing is dead and the app should
    // unpair and ask for pairing again; on FAILED it should retry later.
    muse_vm_refresh_result_t refreshTokens();

    // The node id to show on the "not connected" screen.
    const char *nodeId() const;

private:
    Callbacks _callbacks;
    bool _started = false;
};

} // namespace esp_brookesia::apps
