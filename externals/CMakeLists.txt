
# psd_sdk (MolecularMatters PSD reader/writer)
add_subdirectory(psd_sdk/src/Psd)
target_include_directories(Psd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/psd_sdk/src/Psd)

# WhiteoutLib

set(WHITEOUT_ENABLE_CASC ON CACHE BOOL "Enable CASC support" FORCE)
set(WHITEOUT_ENABLE_MPQ  ON CACHE BOOL "Enable MPQ support"  FORCE)
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

# nlohmann/json — header-only; an INTERFACE target carries the include dir so
# both the GUI and the (ImGui-free) pipeline model / CLI can consume it.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/json/single_include)

# imgui-node-editor (thedmd) — node graph canvas for the Pipelines tab.
# Built as a static lib linking DearImgui, mirroring the DearImgui block above.
add_library(ImGuiNodeEditor STATIC
    imgui-node-editor/imgui_node_editor.cpp
    imgui-node-editor/imgui_node_editor_api.cpp
    imgui-node-editor/imgui_canvas.cpp
    imgui-node-editor/crude_json.cpp
)
target_include_directories(ImGuiNodeEditor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui-node-editor
)
target_link_libraries(ImGuiNodeEditor PUBLIC DearImgui)

# crude_json.cpp calls std::terminate() but does not include <exception>; newer
# libc++ (Xcode 16+) stopped providing it transitively, breaking the build.
# Force-include the header for this upstream source (it is a submodule we don't
# patch in-tree).  MSVC's STL still pulls it in, so it is not needed there.
if(NOT MSVC)
    target_compile_options(ImGuiNodeEditor PRIVATE -include exception)
endif()

# Real-ESRGAN AI Upscaler (optional — requires Vulkan SDK)
add_subdirectory(upscaler)
