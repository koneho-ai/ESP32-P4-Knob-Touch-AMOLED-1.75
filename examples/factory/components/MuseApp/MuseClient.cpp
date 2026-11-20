/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseClient.hpp"

#include <string.h>

#include "muse_chat_priv.h"
#include "muse_credentials.h"
#include "muse_provisioning.h"
#include "sdkconfig.h"

namespace esp_brookesia::apps {

MuseClient::MuseClient(Callbacks callbacks):
    _callbacks(std::move(callbacks))
{
}

MuseClient::~MuseClient()
{
    // The session task is a singleton that outlives the client; stop() is
    // the app's explicit disconnect, not destruction.
}

esp_err_t MuseClient::start()
{
    // Credentials first: the session reads the token from muse_vm's RAM
    // cache as soon as its task runs.
    muse_vm_init();
    muse_hatch_start();
    _started = true;
    return ESP_OK;
}

void MuseClient::stop()
{
    if (!_started) {
        return;
    }
    muse_hatch_turn_cancel();
    // muse_hatch_chat_forget() (muse_chat_priv.h) drops the Noise
    // connection and the cached VM credentials. The SDK keeps it private
    // because its gadget never disconnects; the factory app stops the
    // session on hide/unpair, so it needs the explicit disconnect. The
    // session task itself stays alive (muse_hatch_start() is idempotent)
    // and reconnects on the next testConnection()/configChanged().
    muse_hatch_chat_forget();
}

bool MuseClient::isProvisioned() const
{
    return muse_provisioning_is_provisioned();
}

bool MuseClient::isReady() const
{
    return muse_hatch_ready();
}

void MuseClient::testConnection()
{
    muse_hatch_test();
}

void MuseClient::configChanged()
{
    muse_hatch_config_changed();
}

muse_hatch_status_t MuseClient::status()
{
    muse_hatch_status_t status = {};
    muse_hatch_status(&status);
    return status;
}

void MuseClient::beginTurn()
{
    muse_hatch_turn_begin();
}

void MuseClient::feedAudio(const int16_t *pcm, size_t frames)
{
    muse_hatch_turn_audio(pcm, frames);
}

size_t MuseClient::feedSeedAudio(const int16_t *pcm, size_t frames)
{
    return muse_hatch_turn_audio_wait(pcm, frames, 100);
}

void MuseClient::endTurn()
{
    muse_hatch_turn_end();
}

void MuseClient::cancelTurn()
{
    muse_hatch_turn_cancel();
}

void MuseClient::pollEvents()
{
    char text[512];
    muse_hatch_ev_t ev;
    while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
        switch (ev) {
        case MUSE_HATCH_EV_HEARD:
            if (_callbacks.onHeard) {
                _callbacks.onHeard(text);
            }
            break;
        case MUSE_HATCH_EV_REPLY:
            if (_callbacks.onReply) {
                _callbacks.onReply(text);
            }
            break;
        case MUSE_HATCH_EV_DONE:
            if (_callbacks.onDone) {
                _callbacks.onDone(text);
            }
            break;
        case MUSE_HATCH_EV_ERROR:
            if (_callbacks.onError) {
                _callbacks.onError(text);
            }
            break;
        case MUSE_HATCH_EV_SENT:
            if (_callbacks.onSent) {
                _callbacks.onSent();
            }
            break;
        case MUSE_HATCH_EV_NONE:
        default:
            break;
        }
    }
}

bool MuseClient::checkIncoming()
{
    return muse_hatch_incoming();
}

// ---- Realtime voice calls (Phase 4.1) ----

bool MuseClient::callReady() const
{
#if CONFIG_MUSE_REALTIME_VOICE
    return muse_voice_call_ready();
#else
    return false;
#endif
}

void MuseClient::startCall()
{
#if CONFIG_MUSE_REALTIME_VOICE
    muse_voice_call_start();
#endif
}

void MuseClient::endCall()
{
#if CONFIG_MUSE_REALTIME_VOICE
    muse_voice_call_end();
#endif
}

bool MuseClient::feedCallAudio(const int16_t *pcm, size_t frames)
{
#if CONFIG_MUSE_REALTIME_VOICE
    return muse_voice_call_audio(pcm, frames);
#else
    (void)pcm;
    (void)frames;
    return false;
#endif
}

size_t MuseClient::readCallAudio(int16_t *pcm, size_t frames, bool *flushed)
{
#if CONFIG_MUSE_REALTIME_VOICE
    return muse_voice_call_read(pcm, frames, flushed);
#else
    (void)pcm;
    (void)frames;
    if (flushed) {
        *flushed = false;
    }
    return 0;
#endif
}

muse_voice_call_ev_t MuseClient::pollCallEvent(char *text, size_t cap)
{
#if CONFIG_MUSE_REALTIME_VOICE
    return muse_voice_call_event(text, cap);
#else
    (void)text;
    (void)cap;
    return MUSE_VOICE_CALL_EV_NONE;
#endif
}

bool MuseClient::callWorking() const
{
#if CONFIG_MUSE_REALTIME_VOICE
    return muse_voice_call_working();
#else
    return false;
#endif
}

size_t MuseClient::readReply(int16_t *pcm, size_t frames, int wait_ms)
{
    return muse_hatch_turn_read(pcm, frames, wait_ms);
}

bool MuseClient::captionAt(size_t played, char *out, size_t cap)
{
    return muse_hatch_turn_caption(played, out, cap);
}

float MuseClient::turnProgress(size_t played)
{
    int message = -1;
    return muse_turn_progress(played, &message);
}

muse_vm_refresh_result_t MuseClient::refreshTokens()
{
    return muse_vm_refresh_tokens_ex();
}

const char *MuseClient::nodeId() const
{
    return muse_vm_node_id();
}

} // namespace esp_brookesia::apps

// Provided by the app for the vendored call core (muse_voice_call.h): the
// Link node id the call names as its device. Called on the session task,
// whose stack is in PSRAM: it must not read flash — muse_vm_node_id()
// returns a RAM-cached string, so it qualifies. Defined unconditionally;
// with calls off nothing references it.
extern "C" const char *muse_voice_call_device_id(void)
{
    return muse_vm_node_id();
}
