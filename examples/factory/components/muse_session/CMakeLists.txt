# Muse chat session core, vendored from the Muse gadget SDK (see
# examples/factory/VENDORED.md). This is the SDK's own headless Voice-PE
# build file list (the CONFIG_HOMEHUB_VOICE branch of the SDK muse
# component): the chat session plus its text/options helpers, with the
# factory glue (muse_factory_glue.c) standing in for the Muse settings,
# Wi-Fi and Link hooks the way the SDK's voice_muse_chat.c does.
set(srcs
    "muse_chat.c"
    "muse_account_api.c"
    "muse_chat_session.cpp"
    "muse_chat_text.c"
    "muse_text.c"
    "muse_options.c"
    "muse_options_json.c"
    "muse_factory_glue.c"
)
if(CONFIG_MUSE_REALTIME_VOICE)
    # Phase 4.1: realtime voice calls (no AEC — see VENDORED.md).
    list(APPEND srcs "muse_voice_call.cpp" "muse_voice_call_core.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES "test_reply.mp3"
    PRIV_REQUIRES
        app_services
        cjson
        esp_http_client
        esp_hw_support
        esp_netif
        esp_psram
        esp_system
        esp-tls
        esp_timer
        freertos
        heap
        log
        lwip
        mbedtls
        minimp3
        muse_noise_core
        muse_vm
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
