set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

set(PROJ_SRCS_C
    "assets/macropad_icons.c"
)

set(PROJ_SRCS_CPP
    "MacroPadApp.cpp"
    "MacroPadConfig.cpp"
    "MacroPadHid.cpp"
    "MacroPadIcons.cpp"
    "MacroPadUi.cpp"
)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core app_services
    PRIV_REQUIRES cjson
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
