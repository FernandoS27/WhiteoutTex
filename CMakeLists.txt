cmake_minimum_required(VERSION 3.15)
project(WhiteoutTex VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/FS)
endif()

option(WHITEOUT_ENABLE_UPSCALER "Enable AI upscaling via Real-ESRGAN / ncnn / Vulkan" OFF)
option(WHITEOUT_BUILD_CLI "Build WhiteoutTexCLI command-line tool" OFF)

add_subdirectory(externals)

add_executable(WhiteoutTex
    src/main.cpp
    src/app.cpp
    src/preferences.cpp
    src/texture_converter.cpp
    src/thread_pool_manager.cpp
    src/services/texture_service.cpp
    src/services/casc_service.cpp
    src/services/mpq_service.cpp
    src/services/batch_service.cpp
    src/views/batch_convert.cpp
    src/views/casc_browser.cpp
    src/views/dialogs.cpp
    src/views/image_details.cpp
    src/views/image_viewer.cpp
    src/views/menu_bar.cpp
    src/views/save_dialog.cpp
    src/views/save_helpers.cpp
    src/psd/parser.cpp
    src/psd/writer.cpp
)

if(WHITEOUT_ENABLE_UPSCALER)
    target_sources(WhiteoutTex PRIVATE src/upscaler.cpp src/services/upscaler_service.cpp)
    target_compile_definitions(WhiteoutTex PRIVATE WHITEOUT_HAS_UPSCALER)
endif()

if(WIN32)
    target_sources(WhiteoutTex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
    set_target_properties(WhiteoutTex PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(WhiteoutTex PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WhiteoutTex"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.fernandosahmkow.whiteouttex"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AppIcon")
    # The CI workflow generates AppIcon.icns from resources/Icon.png before
    # configure runs.  Local macOS builds without the .icns get a bundle
    # with the system default icon — non-fatal.
    set(_appicon "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.icns")
    if(EXISTS "${_appicon}")
        target_sources(WhiteoutTex PRIVATE "${_appicon}")
        set_source_files_properties("${_appicon}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()
endif()

target_include_directories(WhiteoutTex PRIVATE src)
target_link_libraries(WhiteoutTex PRIVATE DearImgui SDL3::SDL3 whiteout_casc whiteout_mpq Psd)

if(APPLE)
    target_sources(WhiteoutTex PRIVATE src/views/macos_folder_dialog.mm)
    set_source_files_properties(src/views/macos_folder_dialog.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    # Cocoa is needed for the native NSOpenPanel-based folder picker
    # (macos_folder_dialog.mm).  HTTP for online CASC is handled by
    # WhiteoutLib's SimpleHttpHandler via libcurl from the macOS SDK.
    target_link_libraries(WhiteoutTex PRIVATE "-framework Cocoa")
endif()

if(WHITEOUT_ENABLE_UPSCALER)
    target_link_libraries(WhiteoutTex PRIVATE realesrgan_lib)
endif()

add_custom_command(TARGET WhiteoutTex POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE.md"
        "${CMAKE_SOURCE_DIR}/LICENSE-AI.md"
        "${CMAKE_SOURCE_DIR}/README.md"
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md"
        $<TARGET_FILE_DIR:WhiteoutTex>
)

# ============================================================================
# WhiteoutTexCLI — optional command-line tool
# ============================================================================

if(WHITEOUT_BUILD_CLI)
    add_executable(WhiteoutTexCLI
        src/cli_main.cpp
        src/texture_converter.cpp
        src/thread_pool_manager.cpp
        src/psd/parser.cpp
        src/psd/writer.cpp
    )

    if(WHITEOUT_ENABLE_UPSCALER)
        target_sources(WhiteoutTexCLI PRIVATE src/upscaler.cpp)
        target_compile_definitions(WhiteoutTexCLI PRIVATE WHITEOUT_HAS_UPSCALER)
        # upscaler.cpp transitively includes views/save_helpers.h → save_dialog.h → imgui.h
        target_include_directories(WhiteoutTexCLI PRIVATE
            ${CMAKE_SOURCE_DIR}/externals/DearImgui
            ${CMAKE_SOURCE_DIR}/externals/DearImgui/backends)
    endif()

    target_include_directories(WhiteoutTexCLI PRIVATE src)
    target_link_libraries(WhiteoutTexCLI PRIVATE whiteout_casc Psd)

    if(WIN32)
        target_sources(WhiteoutTexCLI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
    endif()

    if(WHITEOUT_ENABLE_UPSCALER)
        target_link_libraries(WhiteoutTexCLI PRIVATE realesrgan_lib SDL3::SDL3)
    endif()

endif()

