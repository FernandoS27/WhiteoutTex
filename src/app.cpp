// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "app.h"
#include "save_helpers.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

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
    auto* state = static_cast<whiteout::gui::FileDialogState*>(userdata);
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

/// UI color for success messages in result popups.
constexpr ImVec4 kSuccessColor{0.4f, 1.0f, 0.4f, 1.0f};
/// UI color for error messages in result popups.
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
/// Default window clear color.
constexpr ImVec4 kClearColor{0.10f, 0.10f, 0.12f, 1.00f};

} // anonymous namespace

namespace whiteout::gui {

// ============================================================================
// Initialization & Shutdown
// ============================================================================

App::~App() {
#ifdef WHITEOUT_HAS_UPSCALER
    if (upscale_thread_.joinable())
        upscale_thread_.join();
#endif
}

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
        startup_width = std::max(MIN_WINDOW_WIDTH, saved_host_size.width);
        startup_height = std::max(MIN_WINDOW_HEIGHT, saved_host_size.height);
    }

    const MainWindowIniRect ini_rect = load_main_window_ini_rect(imgui_ini_path_);
    if (!saved_host_size.has_size && ini_rect.has_size) {
        const int extra_y = ini_rect.has_pos ? std::clamp(ini_rect.pos_y, 0, 200) : 0;
        startup_width = std::max(MIN_WINDOW_WIDTH, ini_rect.width);
        startup_height = std::max(MIN_WINDOW_HEIGHT, ini_rect.height + extra_y);
    }

    const SDL_WindowFlags window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(WINDOW_TITLE, startup_width, startup_height, window_flags);
    if (!window_) {
        std::printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_Log("Error: SDL_CreateRenderer(): %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);
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
    append_batch_prefs(imgui_ini_path_, batch_prefs_);
    append_recent_files(imgui_ini_path_, recent_files_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentCascPaths]", recent_casc_paths_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchInputDirs]", recent_batch_input_dirs_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchOutputDirs]", recent_batch_output_dirs_);

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

void App::openFile(const std::string& path) {
    recent_files_.push(path);
    if (TC::classifyPath(path) == TFF::TEX && TC::isD4Tex(path)) {
        auto result = loadD4TexWithFallback(converter_, path);
        if (result) {
            applyLoadedTexture(path, std::move(*result));
        } else if (converter_.hasIssues()) {
            status_message_ = "Failed to load D4 TEX: " + path;
            appendIssues(status_message_, converter_.getIssues());
        } else {
            pending_d4_meta_path_ = path;
            const std::string prefill = replaceMetaSegment(path, "payload");
            copyToBuffer(d4_payload_path_buf_, prefill.empty() ? path : prefill);
            show_d4_payload_dialog_ = true;
        }
        return;
    }

    auto result = converter_.load(path);
    if (result) {
        applyLoadedTexture(path, std::move(*result));
    } else {
        status_message_ = "Failed to load: " + path;
        if (converter_.hasIssues())
            appendIssues(status_message_, converter_.getIssues());
    }
}

void App::applyLoadedTexture(const std::string& path, tex::Texture texture) {
    loaded_texture_ = std::move(texture);
    applyGuessedKind(*loaded_texture_, path);

    const tex::PixelFormat loaded_fmt = loaded_texture_->format();
    loaded_source_fmt_ = loaded_fmt;

    if (tex::isBcn(loaded_fmt)) {
        auto* pool = threadPoolManager().get();
        auto decompressed = loaded_texture_->copyAsFormat(tex::workingFormatFor(loaded_fmt), pool);
        copyKindMetadata(decompressed, *loaded_texture_);
        *loaded_texture_ = std::move(decompressed);
    }

    viewer_.setTexture(*loaded_texture_);
    if (tex::isBcn(loaded_fmt) && loaded_fmt == tex::PixelFormat::BC3 &&
        loaded_texture_->kind() == tex::TextureKind::Normal) {
        // Defer conversion: BC3N dialog will handle it.
        show_bc3n_dialog_ = true;
    }
    loaded_path_ = path;
    loaded_file_format_ = TC::classifyPath(path);
    status_message_.clear();
}

void App::processOpenResult() {
    if (!open_dialog_state_.has_pending.load())
        return;

    std::string path;
    {
        std::lock_guard lock(open_dialog_state_.mtx);
        path = std::move(open_dialog_state_.pending_path);
        open_dialog_state_.has_pending.store(false);
    }

    openFile(path);

    // Remember the parent directory for next File > Open.
    if (!path.empty()) {
        auto parent = std::filesystem::path(path).parent_path().make_preferred();
        if (!parent.empty())
            save_prefs_.last_open_dir = (parent / "").string();
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

    // Remember the parent directory for next File > Save As.
    if (!path.empty()) {
        auto parent = std::filesystem::path(path).parent_path().make_preferred();
        if (!parent.empty())
            save_prefs_.last_save_dir = (parent / "").string();
    }
}

// ============================================================================
// UI drawing
// ============================================================================

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open")) {
            const char* default_dir = save_prefs_.last_open_dir.empty()
                                          ? nullptr
                                          : save_prefs_.last_open_dir.c_str();
            SDL_ShowOpenFileDialog(file_dialog_callback, &open_dialog_state_, window_, OPEN_FILTERS,
                                   OPEN_FILTER_COUNT, default_dir, false);
        }
        if (ImGui::BeginMenu("Open Recent", !recent_files_.paths.empty())) {
            std::string pending_recent_open;
            for (const auto& recent_path : recent_files_.paths) {
                namespace fs = std::filesystem;
                std::string label = fs::path(recent_path).filename().string();
                if (ImGui::MenuItem(label.c_str())) {
                    pending_recent_open = recent_path;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", recent_path.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent")) {
                recent_files_.paths.clear();
            }
            ImGui::EndMenu();
            if (!pending_recent_open.empty()) {
                openFile(pending_recent_open);
            }
        }
        if (ImGui::MenuItem("Save As...", nullptr, false, loaded_texture_.has_value())) {
            save_dialog_.buildFilterOrder(save_prefs_);
            const char* save_default = save_prefs_.last_save_dir.empty()
                                           ? nullptr
                                           : save_prefs_.last_save_dir.c_str();
            SDL_ShowSaveFileDialog(file_dialog_callback, &save_dialog_state_, window_,
                                   save_dialog_.filterData(), save_dialog_.filterCount(),
                                   save_default);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Batch convert...")) {
#ifdef WHITEOUT_HAS_UPSCALER
            batch_convert_.setUpscalerModels(
                Upscaler::availableModels(Upscaler::defaultModelDir()));
#endif
            batch_convert_.open(batch_prefs_);
        }
        if (ImGui::MenuItem("CASC Browser...")) {
            casc_browser_.open();
        }
#ifdef WHITEOUT_HAS_UPSCALER
        ImGui::Separator();
        if (ImGui::MenuItem("Upscale (AI)...", nullptr, false, loaded_texture_.has_value())) {
            auto model_dir = Upscaler::defaultModelDir();
            upscale_models_ = Upscaler::availableModels(model_dir);
            upscale_model_index_ = 0;
            upscale_status_.clear();
            show_upscale_dialog_ = true;
        }
#endif
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

void App::drawResultDialog() {
    if (show_result_popup_) {
        ImGui::OpenPopup("##ResultDialog");
        show_result_popup_ = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("##ResultDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool success = result_popup_success_;
        if (success) {
            ImGui::TextColored(kSuccessColor, "Success");
        } else {
            ImGui::TextColored(kErrorColor, "Error");
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

void App::drawBC3NDialog() {
    if (show_bc3n_dialog_) {
        ImGui::OpenPopup("BC3N Normal Map");
        show_bc3n_dialog_ = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("BC3N Normal Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This DDS uses BC3 compression and is identified as a Normal map.");
        ImGui::TextUnformatted("Treat it as BC3N (X stored in alpha, Y stored in green)?");
        ImGui::Spacing();
        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            if (loaded_texture_) {
                loaded_texture_->swapChannels(tex::Channel::R, tex::Channel::A);
                loaded_texture_->invertChannel(tex::Channel::G);
                viewer_.setTexture(*loaded_texture_);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::drawD4PayloadDialog() {
    if (show_d4_payload_dialog_) {
        ImGui::OpenPopup("Diablo IV Payload Required");
        show_d4_payload_dialog_ = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("Diablo IV Payload Required", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This is a Diablo IV TEX file. Pixel data lives in a separate");
        ImGui::TextUnformatted("payload file that could not be found automatically.");
        ImGui::Spacing();
        ImGui::TextDisabled("Meta: %s", pending_d4_meta_path_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Payload file path:");
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_payload", d4_payload_path_buf_, sizeof(d4_payload_path_buf_));
        ImGui::Spacing();
        ImGui::TextUnformatted("Low-res payload file path (Optional):");
        ImGui::SetNextItemWidth(600.0f);
        ImGui::InputText("##d4_paylow", d4_paylow_path_buf_, sizeof(d4_paylow_path_buf_));
        ImGui::Spacing();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            const std::string payload_path(d4_payload_path_buf_);
            const std::string paylow_path(d4_paylow_path_buf_);
            if (!payload_path.empty() && std::filesystem::exists(payload_path)) {
                const bool use_paylow =
                    !paylow_path.empty() && std::filesystem::exists(paylow_path);
                auto result = use_paylow
                                  ? converter_.loadTexD4(pending_d4_meta_path_, payload_path,
                                                        paylow_path)
                                  : converter_.loadTexD4(pending_d4_meta_path_, payload_path);
                if (result) {
                    applyLoadedTexture(pending_d4_meta_path_, std::move(*result));
                } else {
                    status_message_ = "Failed to load D4 TEX: " + pending_d4_meta_path_;
                    appendIssues(status_message_, converter_.getIssues());
                }
            } else {
                status_message_ = "Payload file not found: " + payload_path;
            }
            pending_d4_meta_path_.clear();
            d4_payload_path_buf_[0] = '\0';
            d4_paylow_path_buf_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            pending_d4_meta_path_.clear();
            d4_payload_path_buf_[0] = '\0';
            d4_paylow_path_buf_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

#ifdef WHITEOUT_HAS_UPSCALER
void App::startUpscaleThread(const UpscalerModel& model,
                             const tex::Texture& source, bool upscale_alpha) {
    upscale_in_progress_ = true;
    upscale_status_ = "Initializing model...";

    auto model_dir = Upscaler::defaultModelDir();
    auto model_copy = model;
    auto texture_copy = std::make_shared<tex::Texture>(source);
    auto* app = this;

    if (upscale_thread_.joinable())
        upscale_thread_.join();

    upscale_thread_ = std::thread([app, model_dir, model_copy, texture_copy, upscale_alpha]() {
        if (!app->upscaler_.init(model_dir, model_copy)) {
            app->upscale_status_ = "Failed to load model.";
            app->upscale_success_ = false;
            app->upscale_done_.store(true);
            return;
        }
        app->upscale_status_ = "Upscaling...";
        auto result = app->upscaler_.process(*texture_copy, upscale_alpha);
        if (result) {
            app->upscale_result_ = std::move(*result);
            app->upscale_status_ = "Upscale complete.";
            app->upscale_success_ = true;
        } else {
            app->upscale_status_ = "Upscale failed.";
            app->upscale_success_ = false;
        }
        app->upscale_done_.store(true);
    });
}

void App::drawUpscaleDialog() {
    if (show_upscale_dialog_) {
        ImGui::OpenPopup("AI Upscale");
        show_upscale_dialog_ = false;
    }
    centerNextWindow();
    if (ImGui::BeginPopupModal("AI Upscale", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!Upscaler::isGpuAvailable()) {
            ImGui::TextColored(kErrorColor, "No Vulkan-capable GPU detected.");
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        if (upscale_models_.empty()) {
            ImGui::TextColored(kErrorColor, "No models found.");
            ImGui::TextUnformatted("Download models with:");
            ImGui::TextDisabled("  .\\scripts\\download_models.ps1");
            ImGui::TextUnformatted("Models directory:");
            ImGui::TextDisabled("  %s", Upscaler::defaultModelDir().string().c_str());
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        ImGui::TextUnformatted("Upscale the current texture using Real-ESRGAN.");
        ImGui::Spacing();

        // Model selector
        if (ImGui::BeginCombo("Model",
                              upscale_models_[upscale_model_index_].display_name.c_str())) {
            for (int i = 0; i < static_cast<int>(upscale_models_.size()); ++i) {
                bool selected = (i == upscale_model_index_);
                std::string label = upscale_models_[i].label();
                if (ImGui::Selectable(label.c_str(), selected)) {
                    upscale_model_index_ = i;
                }
            }
            ImGui::EndCombo();
        }

        const auto& model = upscale_models_[upscale_model_index_];
        if (loaded_texture_) {
            int outw = static_cast<int>(loaded_texture_->width()) * model.scale;
            int outh = static_cast<int>(loaded_texture_->height()) * model.scale;
            ImGui::Text("Output: %d x %d (%dx)", outw, outh, model.scale);
        }

        ImGui::Spacing();

        if (!upscale_status_.empty()) {
            ImGui::TextWrapped("%s", upscale_status_.c_str());
            ImGui::Spacing();
        }

        bool busy = upscale_in_progress_;
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Upscale", ImVec2(120, 0))) {
            if (loaded_texture_) {
                startUpscaleThread(model, *loaded_texture_, false);
            }
        }
        if (busy) ImGui::EndDisabled();

        ImGui::SameLine();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        if (busy) ImGui::EndDisabled();

        ImGui::EndPopup();
    }
}
#endif // WHITEOUT_HAS_UPSCALER

void App::drawAboutDialog() {
    if (show_about_) {
        ImGui::OpenPopup("About WhiteoutTex");
        show_about_ = false;
    }
    centerNextWindow();
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

int App::run(int argc, char** argv) {
    if (!initSDL())
        return 1;

    // Prepare INI path
    if (const char* base_path = SDL_GetBasePath(); base_path) {
        imgui_ini_path_ = (std::filesystem::path(base_path) / "config.ini").string();
    } else {
        imgui_ini_path_ = "config.ini";
    }

    save_prefs_ = load_save_prefs(imgui_ini_path_);
    batch_prefs_ = load_batch_prefs(imgui_ini_path_);
    recent_files_ = load_recent_files(imgui_ini_path_);
    recent_casc_paths_ = load_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentCascPaths]");
    recent_batch_input_dirs_ = load_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchInputDirs]");
    recent_batch_output_dirs_ = load_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchOutputDirs]");
    save_dialog_.buildFilterOrder(save_prefs_);

#ifdef WHITEOUT_HAS_UPSCALER
    // Discover available upscaler models and pass to image details panel.
    upscale_models_ = Upscaler::availableModels(Upscaler::defaultModelDir());
    image_details_.setUpscalerModels(upscale_models_);
#endif

    if (!initWindow())
        return 1;
    initImGui();

    // "Open With" / command-line file support
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        open_dialog_state_.pending_path = argv[1];
        open_dialog_state_.has_pending.store(true);
    }

    const ImVec4 clear_color = kClearColor;
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
        {
            auto details_result = image_details_.drawDetailsPanel(
                loaded_texture_ ? &*loaded_texture_ : nullptr,
                loaded_path_, loaded_file_format_, loaded_source_fmt_,
                left_panel_width, details_height);

            if (details_result.refresh_display && loaded_texture_) {
                viewer_.refreshDisplay(*loaded_texture_);
            }
            if (details_result.updated_texture) {
                *loaded_texture_ = std::move(*details_result.updated_texture);
                viewer_.setTexture(*loaded_texture_);
            }
            if (!details_result.result_message.empty()) {
                result_popup_message_ = std::move(details_result.result_message);
                result_popup_success_ = details_result.result_success;
                show_result_popup_ = true;
            }
#ifdef WHITEOUT_HAS_UPSCALER
            if (details_result.upscale_model_index >= 0 && loaded_texture_ &&
                !upscale_in_progress_) {
                upscale_model_index_ = details_result.upscale_model_index;
                image_details_.setUpscaleInProgress(true);
                startUpscaleThread(upscale_models_[upscale_model_index_],
                                   *loaded_texture_, details_result.upscale_alpha);
            }

            // Process upscale completion on the main thread.
            if (upscale_done_.load()) {
                if (upscale_thread_.joinable())
                    upscale_thread_.join();
                upscale_done_.store(false);
                upscale_in_progress_ = false;
                image_details_.setUpscaleInProgress(false);
                if (upscale_success_ && upscale_result_) {
                    loaded_texture_ = std::move(*upscale_result_);
                    upscale_result_.reset();
                    loaded_source_fmt_ = tex::PixelFormat::RGBA8;
                    viewer_.setTexture(*loaded_texture_);
                    result_popup_message_ = "Upscale complete.";
                    result_popup_success_ = true;
                } else {
                    upscale_result_.reset();
                    result_popup_message_ = upscale_status_;
                    result_popup_success_ = false;
                }
                show_result_popup_ = true;
            }
#endif
        }
        if (show_mip_list) {
            int new_mip = image_details_.drawMipList(
                *loaded_texture_, viewer_.selectedMip(),
                left_panel_width, mip_list_height);
            if (new_mip >= 0)
                viewer_.selectMip(new_mip);
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // Right panel: image preview
        ImGui::BeginChild("##ImagePanel", ImVec2(0.0f, available_height), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("Image Preview");

        if (viewer_.hasImage()) {
            viewer_.draw(renderer_);
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
            result_popup_success_ = result_popup_message_.starts_with("Saved:");
            show_result_popup_ = true;
        }

        {
            auto batch_status = batch_convert_.draw(window_, batch_prefs_,
                                                      recent_batch_input_dirs_,
                                                      recent_batch_output_dirs_);
            if (!batch_status.empty()) {
                result_popup_message_ = std::move(batch_status);
                result_popup_success_ = result_popup_message_.starts_with("Batch complete:");
                show_result_popup_ = true;
            }
        }

        {
            auto casc_result = casc_browser_.draw(window_, recent_casc_paths_);

            if (casc_result) {
                std::optional<tex::Texture> loaded;
                if (casc_result.is_d4_tex) {
                    if (!casc_result.paylow.empty()) {
                        loaded = converter_.loadTexD4(casc_result.data, casc_result.payload,
                                                      casc_result.paylow);
                    } else {
                        loaded = converter_.loadTexD4(casc_result.data, casc_result.payload);
                    }
                } else {
                    auto fmt = tex::TextureConverter::classifyPath(casc_result.name);
                    loaded = converter_.load(casc_result.data, fmt);
                }

                if (loaded) {
                    applyLoadedTexture(casc_result.name, std::move(*loaded));
                } else {
                    status_message_ = "Failed to load CASC file: " + casc_result.name;
                    if (converter_.hasIssues()) {
                        appendIssues(status_message_, converter_.getIssues());
                    }
                }
            }
        }

        drawAboutDialog();
        drawResultDialog();
        drawBC3NDialog();
        drawD4PayloadDialog();
#ifdef WHITEOUT_HAS_UPSCALER
        drawUpscaleDialog();
#endif

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

} // namespace whiteout::gui
