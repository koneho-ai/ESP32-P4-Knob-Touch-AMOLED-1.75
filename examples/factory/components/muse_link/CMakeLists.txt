# Muse Link session (Phase 4.3), vendored from the Muse gadget SDK (see
# examples/factory/VENDORED.md): the Noise-over-WebSocket session to the
# user's Muse VM (main/noise_control.cpp), the image downloader
# (main/image_fetch.c), the tunnel data plane (main/noise_tunnel.cpp —
# needed even with the tunnel off, it owns the DMA-headroom functions
# noise_control.cpp calls unconditionally), the connectivity log
# (main/link_events.c), the stall probe (main/link_probe.c, adapted to
# the factory wifi_service) and the stack monitor. The MuseApp component
# owns the lifecycle (MuseLink class) and the led_status shim, so this
# component has no MuseApp dependency.
set(srcs
    "noise_control.cpp"
    "image_fetch.c"
    "noise_tunnel.cpp"
    "link_events.c"
    "link_probe.c"
    "stack_monitor.c"
    "muse_device_guide.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        app_services
        cjson
        esp_app_format
        esp_driver_jpeg
        esp_http_client
        esp_hw_support
        esp-tls
        esp_timer
        freertos
        heap
        log
        lwip
        muse_noise_core
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
