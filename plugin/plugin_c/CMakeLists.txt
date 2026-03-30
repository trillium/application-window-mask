cmake_minimum_required(VERSION 3.16)
project(pii-mask-filter VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)

# OBS SDK from obs-aitum-multistream deps
set(OBS_DEPS_DIR "$ENV{HOME}/code/obs-aitum-multistream/.deps")
list(APPEND CMAKE_PREFIX_PATH "${OBS_DEPS_DIR}")
list(APPEND CMAKE_FRAMEWORK_PATH "${OBS_DEPS_DIR}/Frameworks")

find_package(libobs REQUIRED)

# Plugin target
add_library(pii-mask-filter MODULE
    pii_mask_filter.c
    shm_reader.c
    mask_renderer.c
    rect_texture.c
    blur_kawase.c
)

target_include_directories(pii-mask-filter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../protocol
)

target_link_libraries(pii-mask-filter
    OBS::libobs
)

# macOS bundle setup
if(APPLE)
    set_target_properties(pii-mask-filter PROPERTIES
        BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER com.piimask.obs-filter
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    )

    # Copy data files into bundle Resources
    set(EFFECT_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/data/pii_composite.effect
        ${CMAKE_CURRENT_SOURCE_DIR}/data/dual_kawase_down.effect
        ${CMAKE_CURRENT_SOURCE_DIR}/data/dual_kawase_up.effect
        ${CMAKE_CURRENT_SOURCE_DIR}/data/pixelate.effect
    )
    set_source_files_properties(${EFFECT_FILES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(pii-mask-filter PRIVATE ${EFFECT_FILES})
endif()

# Install to OBS plugin directory
install(TARGETS pii-mask-filter
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    BUNDLE DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
)
