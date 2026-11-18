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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent key/value config stored in NVS, namespace "museapp" — the Muse
 * app's own namespace, separate from the SDK's "homehub" (this firmware has
 * no Link component sharing the store).
 *
 * Keys: access_token, refresh_token, node_id, vm_id, api_url, api_url_v2,
 * noise_host, sdk_token, pair_sdk_hint, setup_complete. Device-level keys
 * (sdk_token) are preserved by config_clear_pairing() and config_unpair().
 * auth_token is a legacy alias from early Link firmware, kept only so an
 * erase finds it.
 *
 * NVS itself is initialized by the factory main() (nvs_flash_init); unlike
 * the SDK's config_store, this one never initializes or erases flash, and it
 * owns no Wi-Fi keys (Wi-Fi belongs to the factory wifi_service).
 *
 * All getters return true on success and write a NUL-terminated string
 * of up to (buf_size - 1) bytes into out. Missing keys return false.
 */

typedef enum {
    CONFIG_KEY_LOOKUP_ERROR = -1,
    CONFIG_KEY_NOT_FOUND = 0,
    CONFIG_KEY_FOUND = 1,
} config_key_lookup_t;

bool config_get_str(const char *key, char *out, size_t buf_size);
bool config_set_str(const char *key, const char *value);
bool config_erase_key(const char *key);
config_key_lookup_t config_key_lookup(const char *key);

// Pairing convenience: removes tokens, vm_url, vm_id, username, account-
// specific endpoint overrides. Device-level keys are preserved.
bool config_clear_pairing(void);

// Unpair: config_clear_pairing() plus the setup-complete marker, so the device
// can pair again. Returns true only after every key is durably erased and a
// readback reports it absent.
bool config_unpair(void);

bool config_is_provisioned(void);
// The node id stored with the device tokens (identity.h). False if none is
// stored (not set up, or set up by firmware that stored none).
bool config_get_node_id(char *out, size_t size);
bool config_setup_complete(void);
bool config_mark_setup_complete(void);
bool config_clear_setup_complete(void);

#ifdef __cplusplus
}
#endif
