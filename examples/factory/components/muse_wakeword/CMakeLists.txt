# Muse wake word, vendored from the Muse gadget SDK (see
# examples/factory/VENDORED.md). Adapted from the SDK's
# components/muse_wakeword/CMakeLists.txt: same sources and the same
# aligned .pte embed; C++17 set explicitly, as in muse_session (the
# factory project defaults to a newer standard than the SDK builds with).
#
# With MUSE_WAKEWORD_ENABLE=n this registers empty: no sources and no include
# directories. The ExecuTorch and microfrontend registry components are
# declared in this component's idf_component.yml with a rule on
# MUSE_WAKEWORD_EXECUTORCH, so builds without the feature don't download them,
# and the component manager adds them to this component's requirements only
# when they are part of the build.
set(srcs)
set(includes)
if(CONFIG_MUSE_WAKEWORD_ENABLE)
    list(APPEND srcs "muse_wakeword_core.c" "ww_detector.c" "ww_frontend.c" "ww_runner_espnn.cpp")
    # Not EMBED_FILES: its generated asm has no alignment directive,
    # and ExecuTorch Program::load requires aligned program data.
    # hey_muse_pte.S.in embeds the model 16-aligned in flash instead.
    set(HEY_MUSE_PTE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte")
    file(SIZE "${HEY_MUSE_PTE_PATH}" HEY_MUSE_PTE_LEN)
    configure_file(hey_muse_pte.S.in "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S"
                                PROPERTIES OBJECT_DEPENDS "${HEY_MUSE_PTE_PATH}")
    list(APPEND srcs "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S")
    list(APPEND includes "include")
endif()

# Requirements are collected in an early pass before Kconfig values exist,
# so they are unconditional.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    PRIV_REQUIRES esp_timer
)
if(NOT CONFIG_MUSE_WAKEWORD_ENABLE)
    return()
endif()

# The model runs entirely in ExecuTorch's ESP-NN delegate: the program has
# no CPU operators, so it needs no kernel bundle (and no executorch Python
# at build time). Declaring only the backend links the runtime and the
# delegate, nothing else.
executorch_add_backend(EspNNBackend)
# Identical features on every build: no FMA contraction in the quantizer.
set_source_files_properties(ww_frontend.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
