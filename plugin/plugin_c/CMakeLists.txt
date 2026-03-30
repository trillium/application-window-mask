cmake_minimum_required(VERSION 3.16)
project(pii-mask-filter VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)

# Find OBS
find_package(libobs REQUIRED)

# Plugin target
add_library(pii-mask-filter MODULE
    pii_mask_filter.c
    shm_reader.c
    mask_renderer.c
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
endif()

# Install to OBS plugin directory
install(TARGETS pii-mask-filter
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    BUNDLE DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
)
