# Muse avatar renderer, vendored from the Muse gadget SDK (see
# examples/factory/VENDORED.md): the procedural pixel-art avatar
# (muse_pixel.c) plus the GIF-derived happy animation (happy_anim.c).
set(MUSE_AVATAR_SRCS "happy_anim.c")

# A custom avatar (components/muse_avatar/avatar/muse_pixel.c, gitignored)
# replaces the default renderer, the way the SDK's tools/muse/avatar.py
# does for its own boards. No CONFIGURE_DEPENDS: ESP-IDF includes this
# file in script mode while gathering component requirements, where that
# flag is invalid — re-run cmake after adding or removing a custom avatar.
file(GLOB CUSTOM_AVATAR "${COMPONENT_DIR}/avatar/muse_pixel.c")
if(CUSTOM_AVATAR)
    message(STATUS "Custom avatar: components/muse_avatar/avatar/muse_pixel.c")
    list(APPEND MUSE_AVATAR_SRCS "avatar/muse_pixel.c")
else()
    list(APPEND MUSE_AVATAR_SRCS "muse_pixel.c")
endif()

idf_component_register(
    SRCS ${MUSE_AVATAR_SRCS}
    INCLUDE_DIRS "."
    # muse_pixel.h includes muse_state.h (muse_mode_t lives there).
    REQUIRES muse_session
)
