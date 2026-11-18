/*
 * Muse account credentials: NVS store plus the RAM cache the voice session
 * reads (see muse_credentials.h).
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */

#include "muse_credentials.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config_store.h"
#include "identity.h"
#include "vm_api.h"

static const char *TAG = "muse.vm";

static SemaphoreHandle_t s_lock;
static bool s_inited;
static char s_access[MUSE_VM_TOKEN_MAX + 1];
static char s_refresh[MUSE_VM_TOKEN_MAX + 1];
static char s_host[MUSE_VM_HOST_MAX + 1];
static char s_vm[MUSE_VM_ID_MAX + 1];
static char s_api_base[MUSE_VM_API_BASE_MAX + 1];

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void load_cache_locked(void)
{
    s_access[0] = s_refresh[0] = s_host[0] = s_vm[0] = s_api_base[0] = '\0';
    config_get_str("access_token", s_access, sizeof(s_access));
    config_get_str("refresh_token", s_refresh, sizeof(s_refresh));
    config_get_str("noise_host", s_host, sizeof(s_host));
    config_get_str("vm_id", s_vm, sizeof(s_vm));
    config_get_str("api_url_v2", s_api_base, sizeof(s_api_base));
}

void muse_vm_init(void)
{
    if (s_inited) {
        return;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    identity_init();
    lock();
    load_cache_locked();
    unlock();
    vm_api_set_base_url(s_api_base[0] ? s_api_base : NULL);
    vm_api_set_sdk_token(identity_sdk_token());
    s_inited = true;
    ESP_LOGI(TAG, "credentials %s (node %s)",
             muse_vm_is_provisioned() ? "present" : "absent", identity_node_id());
}

bool muse_vm_is_provisioned(void)
{
    if (!s_inited) {
        muse_vm_init();
    }
    lock();
    bool provisioned = s_access[0] != '\0';
    unlock();
    return provisioned;
}

size_t muse_vm_cached_token_len(void)
{
    lock();
    size_t len = strlen(s_access);
    unlock();
    return len;
}

void muse_vm_cached_token(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    lock();
    strlcpy(out, s_access, cap);
    unlock();
}

void muse_vm_cached_host(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    lock();
    strlcpy(out, s_host, cap);
    unlock();
}

void muse_vm_cached_vm(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    lock();
    strlcpy(out, s_vm, cap);
    unlock();
}

const char *muse_vm_node_id(void)
{
    return identity_node_id();
}

const char *muse_vm_sdk_token(void)
{
    return identity_sdk_token();
}

bool muse_vm_store_tokens(const char *access_token, const char *refresh_token,
                          const char *node_id, const char *api_url_v2,
                          const char *noise_host)
{
    if (!access_token || !access_token[0] || !refresh_token || !refresh_token[0]) {
        return false;
    }
    if (!s_inited) {
        muse_vm_init();
    }
    bool ok = config_set_str("access_token", access_token) &&
              config_set_str("refresh_token", refresh_token);
    if (node_id && node_id[0]) {
        ok = config_set_str("node_id", node_id) && ok;
    }
    if (api_url_v2 && api_url_v2[0]) {
        ok = config_set_str("api_url_v2", api_url_v2) && ok;
    }
    if (noise_host && noise_host[0]) {
        ok = config_set_str("noise_host", noise_host) && ok;
    }
    if (!ok) {
        ESP_LOGE(TAG, "storing device tokens failed");
        return false;
    }
    config_mark_setup_complete();
    identity_refresh_node_id();
    identity_note_paired_sdk_token();
    lock();
    load_cache_locked();
    unlock();
    vm_api_set_base_url(s_api_base[0] ? s_api_base : NULL);
    vm_api_set_sdk_token(identity_sdk_token());
    ESP_LOGI(TAG, "device tokens stored (node %s)", identity_node_id());
    return true;
}

muse_vm_refresh_result_t muse_vm_refresh_tokens_ex(void)
{
    if (!s_inited) {
        muse_vm_init();
    }
    char access[MUSE_VM_TOKEN_MAX + 1];
    char refresh[MUSE_VM_TOKEN_MAX + 1];
    char node_id[MUSE_VM_NODE_ID_MAX + 1];
    lock();
    strlcpy(access, s_access, sizeof(access));
    strlcpy(refresh, s_refresh, sizeof(refresh));
    unlock();
    strlcpy(node_id, identity_node_id(), sizeof(node_id));
    if (!access[0] && !refresh[0]) {
        return MUSE_VM_REFRESH_FAILED;
    }
    vm_device_tokens_t out = {};
    int rc = vm_api_refresh_device_token(access[0] ? access : NULL,
                                         refresh[0] ? refresh : NULL,
                                         node_id, &out, NULL);
    if (rc != 0) {
        if (rc == VM_API_ERR_REVOKED) {
            ESP_LOGW(TAG, "device tokens revoked; pair again");
            return MUSE_VM_REFRESH_REVOKED;
        }
        ESP_LOGW(TAG, "device token refresh failed (%d)", rc);
        return MUSE_VM_REFRESH_FAILED;
    }
    bool ok = muse_vm_store_tokens(out.access_token, out.refresh_token,
                                   NULL, NULL, NULL);
    vm_device_tokens_free(&out);
    return ok ? MUSE_VM_REFRESH_OK : MUSE_VM_REFRESH_FAILED;
}

bool muse_vm_refresh_tokens(void)
{
    return muse_vm_refresh_tokens_ex() == MUSE_VM_REFRESH_OK;
}

bool muse_vm_unpair(void)
{
    bool ok = config_unpair();
    identity_refresh_node_id();
    lock();
    load_cache_locked();
    unlock();
    return ok;
}

void muse_vm_set_preferred_vm(const char *vm_id)
{
    if (!vm_id) {
        vm_id = "";
    }
    if (config_set_str("vm_id", vm_id)) {
        lock();
        strlcpy(s_vm, vm_id, sizeof(s_vm));
        unlock();
    }
}
