set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

set(PROJ_SRCS_C
    "assets/img_app_muse.c"
)

set(PROJ_SRCS_CPP
    "MuseApp.cpp"
    "MuseClient.cpp"
    "MuseUi.cpp"
)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core app_services Settings MuseBlePairing muse_session muse_vm esp_audio_effects
    PRIV_REQUIRES
        app_fonts
        app_update
        board_app
        cjson
        esp_event
        esp_netif
        esp_timer
        nvs_flash
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
