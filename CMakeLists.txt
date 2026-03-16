cmake_minimum_required(VERSION 3.15)
project(WhiteoutTex VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/FS)
endif()

add_subdirectory(externals)

add_executable(WhiteoutTex
    src/main.cpp
    src/app.cpp
    src/batch_convert.cpp
    src/casc_browser.cpp
    src/image_viewer.cpp
    src/preferences.cpp
    src/save_dialog.cpp
    src/save_helpers.cpp
    src/texture_converter.cpp
    src/thread_pool_manager.cpp
)

if(WIN32)
    target_sources(WhiteoutTex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.rc")
    set_target_properties(WhiteoutTex PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(WhiteoutTex PRIVATE src)
target_link_libraries(WhiteoutTex PRIVATE DearImgui SDL3::SDL3 whiteout_casc)

if(WIN32)
    add_custom_command(TARGET WhiteoutTex POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:WhiteoutTex>
    )
endif()

add_custom_command(TARGET WhiteoutTex POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE.md"
        "${CMAKE_SOURCE_DIR}/LICENSE-AI.md"
        "${CMAKE_SOURCE_DIR}/README.md"
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md"
        $<TARGET_FILE_DIR:WhiteoutTex>
)

