# Muse VM account: VM discovery + device-token mint/refresh + the NVS
# credential store, vendored/slimmed from the Muse gadget SDK (see
# examples/factory/VENDORED.md).
idf_component_register(
    SRCS
        "vm_api.c"
        "config_store.c"
        "identity.c"
        "muse_credentials.c"
        "muse_provisioning.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        cjson
        esp_http_client
        esp-tls
        esp_hw_support
        esp_system
        freertos
        log
        mbedtls
        nvs_flash
)
