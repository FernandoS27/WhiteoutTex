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
    src/services/batch_service.cpp
    src/views/batch_convert.cpp
    src/views/casc_browser.cpp
    src/views/dialogs.cpp
    src/views/image_details.cpp
    src/views/image_viewer.cpp
    src/views/menu_bar.cpp
    src/views/save_dialog.cpp
    src/views/save_helpers.cpp
)

if(WHITEOUT_ENABLE_UPSCALER)
    target_sources(WhiteoutTex PRIVATE src/upscaler.cpp src/services/upscaler_service.cpp)
    target_compile_definitions(WhiteoutTex PRIVATE WHITEOUT_HAS_UPSCALER)
endif()

if(WIN32)
    target_sources(WhiteoutTex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
    set_target_properties(WhiteoutTex PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(WhiteoutTex PRIVATE src)
target_link_libraries(WhiteoutTex PRIVATE DearImgui SDL3::SDL3 whiteout_casc)

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
    target_link_libraries(WhiteoutTexCLI PRIVATE whiteout_casc)

    if(WIN32)
        target_sources(WhiteoutTexCLI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
    endif()

    if(WHITEOUT_ENABLE_UPSCALER)
        target_link_libraries(WhiteoutTexCLI PRIVATE realesrgan_lib SDL3::SDL3)
    endif()

endif()

