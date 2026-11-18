/*
 * Provisioning interface shared by the Muse app and the BLE pairing
 * component (MuseBlePairing).
 *
 * The BLE pairing flow (community pairing v5, provision_v2) runs in the
 * MuseBlePairing component. When it has finished — the Muse app has sent the
 * device-token pair (and the API base / Noise host overrides, when the app
 * sends them) — it hands the credentials to the Muse app through this
 * interface, which stores them in the app's NVS namespace ("museapp") and
 * refreshes the RAM cache the voice session reads. The Muse app notices the
 * new credentials on its next poll (about a second) and connects; no restart
 * is needed.
 *
 * The pairing component needs only this header plus a REQUIRES on muse_vm.
 * It must NOT touch the NVS namespace or the credential cache directly.
 *
 * Token handling: tokens are never logged, never committed, and never leave
 * this device except in the Authorization header of the account-API calls
 * in vm_api.c.
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stores a device-token pair delivered by pairing. access_token and
 * refresh_token are required; node_id, api_url_v2 and noise_host may be
 * NULL/empty to leave the stored values alone (the app sends api_url_v2 /
 * noise_host only when the account overrides the defaults). Marks setup
 * complete and remembers which SDK token the pair was issued for. Returns
 * false when the tokens are missing or the NVS write fails. Call from a
 * task with an internal-RAM stack (NVS writes run there).
 */
bool muse_provisioning_set_device_tokens(const char *access_token,
                                         const char *refresh_token,
                                         const char *node_id,
                                         const char *api_url_v2,
                                         const char *noise_host);

/* True when a device-token pair is stored (the device is paired). */
bool muse_provisioning_is_provisioned(void);

/*
 * Forgets the pairing (tokens, VM, node id) from NVS and the RAM cache.
 * Wi-Fi credentials are NOT touched (they belong to the factory
 * wifi_service). The Muse app returns to its "not connected" screen.
 */
bool muse_provisioning_unpair(void);

/*
 * The node id to offer during pairing ("homelink-XXXXXX" before any pairing,
 * the stored id after). The app may offer a different id; whatever it
 * settles on comes back through set_device_tokens()'s node_id.
 */
const char *muse_provisioning_node_id(void);

/* The BLE advertising name ("MuseGadget-XXXXXX"). */
const char *muse_provisioning_ble_name(void);

/*
 * The maker's SDK token (NVS, else the build's CONFIG_GADGET_SDK_TOKEN),
 * or NULL when none is set. Pairing includes it in the device-token mint
 * body when present. Never log or print it.
 */
const char *muse_provisioning_sdk_token(void);

#ifdef __cplusplus
}
#endif
