# Muse BLE pairing (Bluedroid GATT server) for the P4 knob factory firmware.
#
# UNTESTED ON HARDWARE. See INTEGRATION.md in the parent deliverable.
#
# NOTE on cJSON: the factory project already uses the managed espressif/cjson
# component (see XiaozhiApp/idf_component.yml). This component requires
# "cjson" (the managed one), NOT the IDF built-in "json" component — the two
# build the same cJSON sources and would collide at link time.

idf_component_register(
    SRCS "muse_ble_pairing.c"
         "vendored/link_pairing.c"
         "vendored/pairing_transcript.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendored"
    REQUIRES esp_common
    PRIV_REQUIRES bt cjson esp_timer freertos mbedtls
)
