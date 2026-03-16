
# WhiteoutLib

set(WHITEOUT_ENABLE_CASC ON CACHE BOOL "Enable CASC support" FORCE)
add_subdirectory(WhiteoutLib)

# SDL3
add_subdirectory(SDL)

# Dear ImGui

add_library(DearImgui STATIC
    DearImgui/imgui.cpp
    DearImgui/imgui_draw.cpp
    DearImgui/imgui_tables.cpp
    DearImgui/imgui_widgets.cpp
    DearImgui/backends/imgui_impl_sdl3.cpp
    DearImgui/backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(DearImgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/DearImgui
    ${CMAKE_CURRENT_SOURCE_DIR}/DearImgui/backends
)

target_link_libraries(DearImgui PUBLIC SDL3::SDL3)
