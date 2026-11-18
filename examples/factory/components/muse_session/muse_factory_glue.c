/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Factory glue for the vendored Muse chat session core, in the role the
// SDK's main/voice_muse_chat.c plays for the Voice PE build: it stands in
// for the Muse settings, Wi-Fi and Link hooks the session calls.
//
//   - Settings (token/host/VM) come from muse_vm's RAM cache — never NVS
//     directly: the session task's stack lives in PSRAM, where reading
//     flash would crash.
//   - Wi-Fi status comes from the factory wifi_service.
//   - Link is absent: both Hatch hooks report "no Link", so the session
//     always uses the app's own device token (resolve_vm's own-token path).
//   - The caption page matches MuseUi's reply label (260 px wide, 4 lines
//     of 20 px text). Keep these numbers in sync with MuseUi.cpp.
//
// UNTESTED ON HARDWARE — no board exists yet; compile verification only.

#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

#include <string.h>

#include "muse_credentials.h"
#include "wifi_service.h"

/* ---- Link (absent in the factory firmware) ---- */

bool muse_link_hatch_linked(void)
{
    return false;
}

bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name,
                        size_t name_cap, char **vm_token)
{
    (void)want_vm;
    (void)vm_id;
    (void)id_cap;
    (void)vm_name;
    (void)name_cap;
    (void)vm_token;
    return false;
}

/* ---- Settings (muse_vm's RAM cache) ---- */

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    muse_vm_cached_host(out, MUSE_HOST_MAX + 1);
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    muse_vm_cached_vm(out, MUSE_VM_MAX + 1);
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    muse_vm_cached_token(out, MUSE_TOKEN_MAX + 1);
}

size_t muse_settings_hatch_token_len(void)
{
    return muse_vm_cached_token_len();
}

/* ---- Wi-Fi (factory wifi_service) ---- */

bool muse_wifi_connected(void)
{
    wifi_service_info_t info = {};
    wifi_service_get_info(&info);
    return info.state == WIFI_SERVICE_CONNECTED;
}

/* ---- Caption page (MuseUi's reply label) ---- */

void muse_state_page(const char *text, int *cols, int *lines)
{
    (void)text;
    *cols = 26;
    *lines = 4;
}
