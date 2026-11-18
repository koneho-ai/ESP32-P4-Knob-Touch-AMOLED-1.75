/*
 * Muse account credentials: the NVS store (config_store.h, namespace
 * "museapp") plus a RAM cache of the values the voice session reads.
 *
 * Why a cache: the session task's stack lives in PSRAM, where reading flash
 * would crash (see the SDK's main/voice_muse_chat.c). Everything the session
 * asks for — device token, host, VM id — comes from this RAM cache, filled
 * at init and refreshed whenever pairing or a token refresh stores new
 * credentials. NVS is touched only from tasks with internal-RAM stacks
 * (the app's control task, the BLE pairing task).
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds shared with the session core (muse_settings.h in muse_session):
 * keep MUSE_VM_TOKEN_MAX >= MUSE_TOKEN_MAX (1023), the host/VM bounds equal. */
#define MUSE_VM_TOKEN_MAX 1023
#define MUSE_VM_HOST_MAX 63
#define MUSE_VM_ID_MAX 63
#define MUSE_VM_NODE_ID_MAX 31
#define MUSE_VM_API_BASE_MAX 255

/*
 * Call once, from a task with an internal-RAM stack (the app's init, or the
 * BLE pairing task before it needs the identity): loads the cache from NVS,
 * initializes the device identity, and points vm_api at the stored API base
 * and SDK token. Init-once: later calls return immediately.
 */
void muse_vm_init(void);

/* True when a device access token is cached (i.e. the device is paired).
 * Lazily initializes the store on first call. */
bool muse_vm_is_provisioned(void);

/*
 * Cached values for the session task. RAM only, safe from any task; the
 * outputs are always NUL-terminated. The host is empty when no override was
 * provisioned (the session then uses its default, hatch.metaaivm.com); the
 * VM id is empty when no preferred VM was chosen (the account's default).
 */
size_t muse_vm_cached_token_len(void);
void muse_vm_cached_token(char *out, size_t cap);
void muse_vm_cached_host(char *out, size_t cap);
void muse_vm_cached_vm(char *out, size_t cap);

/* The node id in use (the stored one once paired, else the setup id). */
const char *muse_vm_node_id(void);
/* The maker's SDK token (NVS, else the build's CONFIG_GADGET_SDK_TOKEN). */
const char *muse_vm_sdk_token(void);

/*
 * Stores a device-token pair (from pairing) in NVS and refreshes the cache.
 * node_id, api_url_v2 and noise_host may be NULL/empty to leave the stored
 * values alone. Marks setup complete and remembers which SDK token the pair
 * was issued for. Call from a task with an internal-RAM stack.
 */
bool muse_vm_store_tokens(const char *access_token, const char *refresh_token,
                          const char *node_id, const char *api_url_v2,
                          const char *noise_host);

/* Outcome of a device-token refresh, so the app can tell "try again later"
 * (FAILED) from "the pairing is dead, ask for pairing again" (REVOKED). */
typedef enum {
    MUSE_VM_REFRESH_OK,
    MUSE_VM_REFRESH_FAILED,   /* Network/server error: retry later */
    MUSE_VM_REFRESH_REVOKED,  /* Server says the credential is revoked: unpair */
} muse_vm_refresh_result_t;

/*
 * Refreshes the cached pair via the account API (access leg, then refresh
 * leg) and stores the new pair. Call from a task with an internal-RAM
 * stack; takes seconds (HTTPS + retries).
 */
muse_vm_refresh_result_t muse_vm_refresh_tokens_ex(void);

/* muse_vm_refresh_tokens_ex() reduced to a boolean (OK vs anything else). */
bool muse_vm_refresh_tokens(void);

/* Forgets the pairing (tokens, VM, node id) from NVS and the cache. */
bool muse_vm_unpair(void);

/* Preferred VM id (empty = the account's default VM). Stored in NVS. */
void muse_vm_set_preferred_vm(const char *vm_id);

#ifdef __cplusplus
}
#endif
