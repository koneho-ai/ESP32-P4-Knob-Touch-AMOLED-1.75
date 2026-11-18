/*
 * SPDX-FileCopyrightText: 2026 koneho-ai
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MuseClient.hpp"

#include <string.h>

#include "muse_credentials.h"
#include "muse_provisioning.h"

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

size_t MuseClient::readReply(int16_t *pcm, size_t frames, int wait_ms)
{
    return muse_hatch_turn_read(pcm, frames, wait_ms);
}

bool MuseClient::captionAt(size_t played, char *out, size_t cap)
{
    return muse_hatch_turn_caption(played, out, cap);
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
