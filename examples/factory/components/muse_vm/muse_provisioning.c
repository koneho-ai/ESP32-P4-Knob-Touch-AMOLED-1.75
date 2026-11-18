/*
 * Provisioning interface shared by the Muse app and the BLE pairing
 * component — thin wrappers over the credential store (muse_credentials.c).
 *
 * UNTESTED ON HARDWARE — no board exists yet; compile verification only.
 */

#include "muse_provisioning.h"

#include "identity.h"
#include "muse_credentials.h"

bool muse_provisioning_set_device_tokens(const char *access_token,
                                         const char *refresh_token,
                                         const char *node_id,
                                         const char *api_url_v2,
                                         const char *noise_host)
{
    return muse_vm_store_tokens(access_token, refresh_token, node_id,
                                api_url_v2, noise_host);
}

bool muse_provisioning_is_provisioned(void)
{
    return muse_vm_is_provisioned();
}

bool muse_provisioning_unpair(void)
{
    return muse_vm_unpair();
}

const char *muse_provisioning_node_id(void)
{
    // The identity comes from NVS/eFuse; init lazily so the BLE pairing
    // component never has to init the store first.
    muse_vm_init();
    return identity_setup_node_id();
}

const char *muse_provisioning_ble_name(void)
{
    muse_vm_init();
    return identity_ble_name();
}

const char *muse_provisioning_sdk_token(void)
{
    muse_vm_init();
    return muse_vm_sdk_token();
}
