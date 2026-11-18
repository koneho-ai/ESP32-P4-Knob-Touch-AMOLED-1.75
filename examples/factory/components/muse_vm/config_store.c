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

// Slimmed from the SDK's main/config_store.c (see VENDORED.md): same NVS
// helpers, new namespace "museapp", no NVS-encryption handling (the factory
// firmware does not encrypt NVS), no nvs_flash_init (main() owns that), no
// Wi-Fi keys, and no config_clear_setup() (forgetting Wi-Fi is the factory
// wifi_service's job).

#include "config_store.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "muse.config";
static const char *NS = "museapp";
static const char *SETUP_COMPLETE_KEY = "setup_complete";

bool config_get_str(const char *key, char *out, size_t buf_size) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = buf_size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

bool config_set_str(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool config_erase_key(const char *key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open for erase %s failed: %s", key,
                 esp_err_to_name(err));
        return false;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "erase %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

config_key_lookup_t config_key_lookup(const char *key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lookup %s failed to open NVS: %s", key,
                 esp_err_to_name(err));
        return CONFIG_KEY_LOOKUP_ERROR;
    }
    size_t len = 0;
    err = nvs_get_str(h, key, NULL, &len);
    nvs_close(h);
    if (err == ESP_OK) return CONFIG_KEY_FOUND;
    if (err == ESP_ERR_NVS_NOT_FOUND) return CONFIG_KEY_NOT_FOUND;
    ESP_LOGW(TAG, "lookup %s failed: %s", key, esp_err_to_name(err));
    return CONFIG_KEY_LOOKUP_ERROR;
}

static bool keys_are_absent(const char *const *keys, size_t key_count) {
    bool absent = true;
    for (size_t i = 0; i < key_count; i++) {
        config_key_lookup_t state = config_key_lookup(keys[i]);
        if (state != CONFIG_KEY_NOT_FOUND) {
            ESP_LOGE(TAG, "credential %s was not verified absent (state=%d)",
                     keys[i], (int)state);
            absent = false;
        }
    }
    return absent;
}

static bool clear_and_verify(const char *const *keys, size_t key_count) {
    bool erased = true;
    for (size_t i = 0; i < key_count; i++) {
        if (!config_erase_key(keys[i])) erased = false;
    }
    bool absent = keys_are_absent(keys, key_count);
    return erased && absent;
}

bool config_clear_pairing(void) {
    static const char *const keys[] = {
        "api_url",
        "api_url_v2",
        "noise_host",
        "access_token",
        "refresh_token",
        "auth_token",
        "vm_url",
        "vm_id",
        "username",
        "pair_sdk_hint",
        "node_id",   // goes with the tokens it names
    };
    return clear_and_verify(keys, sizeof(keys) / sizeof(keys[0]));
}

bool config_unpair(void) {
    // The marker goes last, and only once the pairing is verifiably gone:
    // boot treats tokens without the marker as a legacy pair and marks it
    // complete again, which would undo a half-finished unpair.
    return config_clear_pairing() && config_clear_setup_complete();
}

bool config_is_provisioned(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, "access_token", NULL, &len);
    if (err != ESP_OK || len <= 1) {
        len = 0;
        err = nvs_get_str(h, "auth_token", NULL, &len);
    }
    nvs_close(h);
    return err == ESP_OK && len > 1;
}

bool config_get_node_id(char *out, size_t size) {
    return config_get_str("node_id", out, size) && out[0];
}

bool config_setup_complete(void) {
    char value[2] = {0};
    return config_get_str(SETUP_COMPLETE_KEY, value, sizeof(value))
           && value[0] == '1';
}

bool config_mark_setup_complete(void) {
    return config_set_str(SETUP_COMPLETE_KEY, "1");
}

bool config_clear_setup_complete(void) {
    const char *keys[] = {SETUP_COMPLETE_KEY};
    return clear_and_verify(keys, sizeof(keys) / sizeof(keys[0]));
}
