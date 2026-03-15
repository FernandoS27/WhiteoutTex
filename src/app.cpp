// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "app.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <whiteout/textures/blp/types.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

namespace {

void SDLCALL file_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    if (!filelist || !filelist[0]) {
        return;
    }
    auto* state = static_cast<whiteout_tex::FileDialogState*>(userdata);
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->pending_filter = filter;
    state->has_pending.store(true);
}

constexpr SDL_DialogFileFilter OPEN_FILTERS[] = {
    {"All Supported Images", "blp;bmp;dds;jpg;jpeg;png;tex;tga"},
    {"BLP (Blizzard Picture)", "blp"},
    {"BMP (Bitmap)", "bmp"},
    {"DDS (DirectDraw Surface)", "dds"},
    {"JPEG", "jpg;jpeg"},
    {"PNG", "png"},
    {"TEX (Blizzard Proprietary)", "tex"},
    {"TGA (Targa)", "tga"},
    {"All Files", "*"},
};
constexpr int OPEN_FILTER_COUNT = static_cast<int>(std::size(OPEN_FILTERS));

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

} // anonymous namespace

namespace whiteout_tex {

// ============================================================================
// Initialization & Shutdown
// ============================================================================

bool App::initSDL() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool App::initWindow() {
    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const int default_width = static_cast<int>(WINDOW_WIDTH * main_scale);
    const int default_height = static_cast<int>(WINDOW_HEIGHT * main_scale);
    int startup_width = default_width;
    int startup_height = default_height;

    const SavedHostWindowSize saved_host_size = load_saved_host_window_size(imgui_ini_path_);
    if (saved_host_size.has_size) {
        startup_width = std::max(320, saved_host_size.width);
        startup_height = std::max(240, saved_host_size.height);
    }

    const MainWindowIniRect ini_rect = load_main_window_ini_rect(imgui_ini_path_);
    if (!saved_host_size.has_size && ini_rect.has_size) {
        const int extra_y = ini_rect.has_pos ? std::clamp(ini_rect.pos_y, 0, 200) : 0;
        startup_width = std::max(320, ini_rect.width);
        startup_height = std::max(240, ini_rect.height + extra_y);
    }

    const SDL_WindowFlags window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(WINDOW_TITLE, startup_width, startup_height, window_flags);
    if (!window_) {
        std::printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    SDL_SetRenderVSync(renderer_, 1);
    if (!renderer_) {
        SDL_Log("Error: SDL_CreateRenderer(): %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    return true;
}

void App::initImGui() {
    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = imgui_ini_path_.c_str();
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
}

void App::shutdown() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SaveIniSettingsToDisk(imgui_ini_path_.c_str());
    io.IniFilename = nullptr;

    int final_w = 0, final_h = 0;
    SDL_GetWindowSize(window_, &final_w, &final_h);
    if (final_w > 0 && final_h > 0) {
        append_saved_host_window_size(imgui_ini_path_, final_w, final_h);
    }
    append_save_prefs(imgui_ini_path_, save_prefs_);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

// ============================================================================
// File dialog result processing
// ============================================================================

void App::processOpenResult() {
    if (!open_dialog_state_.has_pending.load())
        return;

    std::string path;
    {
        std::lock_guard lock(open_dialog_state_.mtx);
        path = std::move(open_dialog_state_.pending_path);
        open_dialog_state_.has_pending.store(false);
    }

    auto result = converter_.load(path);
    if (result) {
        loaded_texture_ = std::move(*result);
        auto guessed_kind = TC::guessTextureKind(path, loaded_texture_->format());
        loaded_texture_->setKind(guessed_kind);
        bool is_orm = loaded_texture_->kind() == tex::TextureKind::ORM;
        viewer_.setTexture(*loaded_texture_, is_orm);
        loaded_path_ = path;
        loaded_file_format_ = TC::classifyPath(path);
        status_message_.clear();
    } else {
        status_message_ = "Failed to load: " + path;
        if (converter_.hasIssues()) {
            for (const auto& issue : converter_.getIssues()) {
                status_message_ += "\n  " + issue;
            }
        }
    }
}

void App::processSaveResult() {
    if (!save_dialog_state_.has_pending.load())
        return;

    std::string path;
    int filter_idx;
    {
        std::lock_guard lock(save_dialog_state_.mtx);
        path = std::move(save_dialog_state_.pending_path);
        filter_idx = save_dialog_state_.pending_filter;
        save_dialog_state_.has_pending.store(false);
    }

    save_dialog_.onFileChosen(path, filter_idx, save_prefs_,
                              loaded_texture_ ? &*loaded_texture_ : nullptr);
}

// ============================================================================
// UI drawing
// ============================================================================

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open")) {
            SDL_ShowOpenFileDialog(file_dialog_callback, &open_dialog_state_, window_, OPEN_FILTERS,
                                   OPEN_FILTER_COUNT, nullptr, false);
        }
        if (ImGui::MenuItem("Save As...", nullptr, false, loaded_texture_.has_value())) {
            save_dialog_.buildFilterOrder(save_prefs_);
            SDL_ShowSaveFileDialog(file_dialog_callback, &save_dialog_state_, window_,
                                   save_dialog_.filterData(), save_dialog_.filterCount(), nullptr);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About WhiteoutTex")) {
            show_about_ = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::drawDetailsPanel(float width, float height) {
    ImGui::BeginChild("##TextPanel", ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Image Details");

    if (loaded_texture_) {
        const auto& t = *loaded_texture_;

        ImGui::SeparatorText("File");
        ImGui::Text("Path: %s", loaded_path_.c_str());
        ImGui::Text("File Format: %s", TC::fileFormatName(loaded_file_format_));

        ImGui::SeparatorText("Texture");
        ImGui::Text("Dimensions: %u x %u", t.width(), t.height());
        if (t.depth() > 1) {
            ImGui::Text("Depth: %u", t.depth());
        }
        ImGui::Text("Type: %s", TC::textureTypeName(t.type()));
        ImGui::Text("Pixel Format: %s", TC::pixelFormatName(t.format()));

        int current_kind = static_cast<int>(t.kind());
        if (ImGui::Combo("Kind", &current_kind, TEXTURE_KIND_NAMES, TEXTURE_KIND_COUNT)) {
            loaded_texture_->setKind(static_cast<tex::TextureKind>(current_kind));
            bool is_orm = loaded_texture_->kind() == tex::TextureKind::ORM;
            viewer_.refreshDisplay(*loaded_texture_, is_orm);
        }

        ImGui::Text("sRGB: %s", t.isSrgb() ? "Yes" : "No");

        ImGui::SeparatorText("Mip Chain");
        ImGui::Text("Mip Levels: %u", t.mipCount());
        ImGui::Text("Layers: %u", t.layerCount());
        ImGui::Text("Total Data Size: %llu bytes", static_cast<unsigned long long>(t.dataSize()));

        if (ImGui::Button("Regenerate Mipmaps")) {
            auto work = *loaded_texture_;
            auto preserved_kind = work.kind();
            bool is_bcn =
                work.format() >= tex::PixelFormat::BC1 && work.format() <= tex::PixelFormat::BC7;
            tex::PixelFormat original_fmt = work.format();
            if (is_bcn) {
                switch (original_fmt) {
                case tex::PixelFormat::BC4:
                    work = work.copyAsFormat(tex::PixelFormat::R32F);
                    break;
                case tex::PixelFormat::BC5:
                    work = work.copyAsFormat(tex::PixelFormat::RG32F);
                    break;
                default:
                    work = work.copyAsFormat(tex::PixelFormat::RGBA32F);
                    break;
                }
            }
            work.setKind(preserved_kind);
            work.generateMipmaps();
            if (is_bcn) {
                work = work.copyAsFormat(original_fmt);
            }
            work.setKind(preserved_kind);
            *loaded_texture_ = std::move(work);

            bool is_orm = loaded_texture_->kind() == tex::TextureKind::ORM;
            viewer_.setTexture(*loaded_texture_, is_orm);
            result_popup_message_ = "Mipmaps regenerated successfully.";
            show_result_popup_ = true;
        }

        if (t.mipCount() > 0 && ImGui::TreeNode("Mip Level Details")) {
            for (whiteout::u32 mip = 0; mip < t.mipCount(); ++mip) {
                const auto& ml = t.mipLevel(mip);
                ImGui::Text("Mip %u: %u x %u  (%llu bytes)", mip, ml.width, ml.height,
                            static_cast<unsigned long long>(ml.size));
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TextWrapped("No image loaded. Use File > Open to load a texture.");
    }
    ImGui::EndChild();
}

void App::drawMipList(float width, float height) {
    ImGui::BeginChild("##MipList", ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Mip Levels");
    for (whiteout::u32 mip = 0; mip < loaded_texture_->mipCount(); ++mip) {
        const auto& ml = loaded_texture_->mipLevel(mip);
        char label[64];
        std::snprintf(label, sizeof(label), "Mip %u  (%u x %u)", mip, ml.width, ml.height);
        if (ImGui::Selectable(label, viewer_.selectedMip() == static_cast<int>(mip))) {
            viewer_.selectMip(static_cast<int>(mip));
        }
    }
    ImGui::EndChild();
}

void App::drawResultDialog() {
    if (show_result_popup_) {
        ImGui::OpenPopup("##ResultDialog");
        show_result_popup_ = false;
    }
    if (ImGui::BeginPopupModal("##ResultDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool success = result_popup_message_.rfind("Saved:", 0) == 0 ||
                             result_popup_message_.rfind("Mipmaps", 0) == 0;
        if (success) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Success");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error");
        }
        ImGui::Separator();
        ImGui::TextUnformatted(result_popup_message_.c_str());
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::drawAboutDialog() {
    if (show_about_) {
        ImGui::OpenPopup("About WhiteoutTex");
        show_about_ = false;
    }
    if (ImGui::BeginPopupModal("About WhiteoutTex", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SeparatorText("WhiteoutTex");
        ImGui::TextUnformatted("A texture viewer and converter for game assets.");
        ImGui::Spacing();
        ImGui::SeparatorText("License");
        ImGui::TextUnformatted(
            "BSD 3-Clause License\n"
            "\n"
            "Copyright (c) 2026, Fernando Sahmkow\n"
            "\n"
            "Redistribution and use in source and binary forms, with or without\n"
            "modification, are permitted provided that the following conditions are met:\n"
            "\n"
            "1. Redistributions of source code must retain the above copyright notice,\n"
            "   this list of conditions and the following disclaimer.\n"
            "\n"
            "2. Redistributions in binary form must reproduce the above copyright notice,\n"
            "   this list of conditions and the following disclaimer in the documentation\n"
            "   and/or other materials provided with the distribution.\n"
            "\n"
            "3. Neither the name of the copyright holder nor the names of its contributors\n"
            "   may be used to endorse or promote products derived from this software\n"
            "   without specific prior written permission.\n"
            "\n"
            "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
            "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
            "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n"
            "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
            "FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
            "DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
            "SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
            "CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,\n"
            "OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
            "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.");
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                             ImGui::GetCursorPosX());
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// Main loop
// ============================================================================

int App::run() {
    if (!initSDL())
        return 1;

    // Prepare INI path
    if (const char* base_path = SDL_GetBasePath(); base_path) {
        imgui_ini_path_ = (std::filesystem::path(base_path) / "config.ini").string();
    } else {
        imgui_ini_path_ = "config.ini";
    }

    save_prefs_ = load_save_prefs(imgui_ini_path_);
    save_dialog_.buildFilterOrder(save_prefs_);

    if (!initWindow())
        return 1;
    initImGui();

    const ImVec4 clear_color{0.10f, 0.10f, 0.12f, 1.00f};
    bool done = false;

    while (!done) {
        // Event polling
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window_)) {
                done = true;
            }
        }

        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // Process file dialog results
        processOpenResult();
        processSaveResult();

        // Start frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        drawMenuBar();

        // Main fullscreen window
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##MainWindow", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        const float available_width = ImGui::GetContentRegionAvail().x;
        const float available_height = ImGui::GetContentRegionAvail().y;
        const float left_panel_width = available_width * 0.4f;

        const bool show_mip_list = loaded_texture_ && loaded_texture_->mipCount() > 1;
        const float mip_list_height = show_mip_list ? available_height * 0.3f : 0.0f;
        const float details_height = available_height - mip_list_height;

        // Left column
        ImGui::BeginGroup();
        drawDetailsPanel(left_panel_width, details_height);
        if (show_mip_list) {
            drawMipList(left_panel_width, mip_list_height);
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // Right panel: image preview
        ImGui::BeginChild("##ImagePanel", ImVec2(0.0f, available_height), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("Image Preview");

        bool is_orm = loaded_texture_ && loaded_texture_->kind() == tex::TextureKind::ORM;
        if (viewer_.hasImage()) {
            viewer_.draw(renderer_, is_orm);
        } else if (!status_message_.empty()) {
            ImGui::TextWrapped("%s", status_message_.c_str());
        } else {
            ImGui::Text("No image loaded. Use File > Open to load a texture.");
        }
        ImGui::EndChild();

        ImGui::End();

        // Popups
        auto save_status = save_dialog_.draw(
            converter_, loaded_texture_ ? &*loaded_texture_ : nullptr, save_prefs_);
        if (!save_status.empty()) {
            result_popup_message_ = std::move(save_status);
            show_result_popup_ = true;
        }

        drawAboutDialog();
        drawResultDialog();

        // Render
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Render();
        SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer_, clear_color.x, clear_color.y, clear_color.z,
                                    clear_color.w);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

    shutdown();
    return 0;
}

} // namespace whiteout_tex
