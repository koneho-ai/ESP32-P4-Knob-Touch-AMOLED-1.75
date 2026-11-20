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
//   - Link (Phase 4.3) is present: the Hatch hooks report the
//     provisioning state and resolve the VM through the account API, so
//     the session's no-own-token fallback path works. (In practice the
//     app always has a token, so the fallback never runs and this only
//     changes the error string from "Not paired" to "Can't reach Muse's
//     server" when provisioned.)
//   - The caption page matches MuseUi's reply label (260 px wide, 4 lines
//     of 20 px text). Keep these numbers in sync with MuseUi.cpp.
//
// UNTESTED ON HARDWARE — no board exists yet; compile verification only.

#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "muse_account_api.h"
#include "muse_credentials.h"
#include "muse_provisioning.h"
#include "wifi_service.h"

/* ---- Link (Phase 4.3: present, visible-only) ---- */

bool muse_link_hatch_linked(void)
{
    return muse_provisioning_is_provisioned();
}

bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name,
                        size_t name_cap, char **vm_token)
{
    if (!vm_id || !id_cap || !vm_name || !name_cap || !vm_token) {
        return false;
    }
    char device_token[MUSE_TOKEN_MAX + 1];
    muse_vm_cached_token(device_token, sizeof(device_token));
    if (!device_token[0]) {
        return false;
    }
    muse_hatch_vm_t vm = {0};
    if (muse_hatch_api_find_vm(device_token, want_vm, &vm) != 0) {
        free(vm.vm_token);   /* NULL on failure; find_vm only sets it on success */
        return false;
    }
    strlcpy(vm_id, vm.vm_id, id_cap);
    strlcpy(vm_name, vm.vm_name, name_cap);
    *vm_token = vm.vm_token;   /* ownership passes to the session core (free()s it) */
    return true;
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
    *lines = 3;  // MuseUi's caption box: 3 lines under the shrunk avatar
}
