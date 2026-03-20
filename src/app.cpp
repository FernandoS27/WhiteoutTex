// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "app.h"
#include "common_types.h"
#include "views/dialogs.h"
#include "views/menu_bar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

namespace {

using whiteout::i32;

void SDLCALL file_dialog_callback(void* userdata, const char* const* filelist, i32 filter) {
    if (!filelist || !filelist[0]) {
        return;
    }
    auto* state = static_cast<whiteout::gui::FileDialogState*>(userdata);
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->pending_filter = filter;
    state->has_pending.store(true);
}

/// Store the parent directory of @p path into @p out (with trailing separator).
void rememberParentDir(const std::string& path, std::string& out) {
    if (path.empty())
        return;
    auto parent = std::filesystem::path(path).parent_path().make_preferred();
    if (!parent.empty())
        out = (parent / "").string();
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

namespace tex = whiteout::textures;

/// Default window clear color.
constexpr ImVec4 kClearColor{0.10f, 0.10f, 0.12f, 1.00f};

} // anonymous namespace

namespace whiteout::gui {

// ============================================================================
// Initialization & Shutdown
// ============================================================================

App::~App() = default;

bool App::initSDL() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool App::initWindow() {
    const f32 main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const i32 default_width = static_cast<i32>(WINDOW_WIDTH * main_scale);
    const i32 default_height = static_cast<i32>(WINDOW_HEIGHT * main_scale);
    i32 startup_width = default_width;
    i32 startup_height = default_height;

    const SavedHostWindowSize saved_host_size = load_saved_host_window_size(imgui_ini_path_);
    if (saved_host_size.has_size) {
        startup_width = std::max(MIN_WINDOW_WIDTH, saved_host_size.width);
        startup_height = std::max(MIN_WINDOW_HEIGHT, saved_host_size.height);
    }

    const MainWindowIniRect ini_rect = load_main_window_ini_rect(imgui_ini_path_);
    if (!saved_host_size.has_size && ini_rect.has_size) {
        const i32 extra_y = ini_rect.has_pos ? std::clamp(ini_rect.pos_y, 0, 200) : 0;
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
    const f32 main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = imgui_ini_path_.c_str();
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    // Dark Ruda style by Raikiri from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 4.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.TabBorderSize = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(0.9490196f, 0.95686275f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.35686275f, 0.41960785f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.11764706f, 0.2f, 0.2784314f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 0.65f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.39f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.1764706f, 0.21960784f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.08627451f, 0.20784314f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36862746f, 0.60784316f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 0.55f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);

    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
}

void App::shutdown() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SaveIniSettingsToDisk(imgui_ini_path_.c_str());
    io.IniFilename = nullptr;

    i32 final_w = 0, final_h = 0;
    SDL_GetWindowSize(window_, &final_w, &final_h);
    if (final_w > 0 && final_h > 0) {
        append_saved_host_window_size(imgui_ini_path_, final_w, final_h);
    }
    append_save_prefs(imgui_ini_path_, save_prefs_);
    append_batch_prefs(imgui_ini_path_, batch_prefs_);
    append_recent_files(imgui_ini_path_, recent_files_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentCascPaths]", recent_casc_paths_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchInputDirs]",
                        recent_batch_input_dirs_);
    append_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchOutputDirs]",
                        recent_batch_output_dirs_);

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
    auto result = texture_service_.loadFromFile(path);
    applyLoadResult(path, std::move(result));
}

void App::applyLoadResult(const std::string& path, TextureLoadResult result) {
    if (result.needs_d4_payload_dialog) {
        ui_.pending_d4_meta_path = std::move(result.d4_meta_path);
        copyToBuffer(ui_.d4_payload_path_buf,
                     result.d4_payload_prefill.empty() ? path : result.d4_payload_prefill);
        ui_.show_d4_payload_dialog = true;
        return;
    }
    if (result.texture) {
        tex_state_.texture = std::move(result.texture);
        tex_state_.source_fmt = result.source_fmt;
        tex_state_.file_format = result.file_format;
        tex_state_.path = path;
        tex_state_.status_message.clear();
        viewer_.setTexture(*tex_state_.texture);
        if (result.needs_bc3n_dialog) {
            ui_.show_bc3n_dialog = true;
        }
    } else {
        tex_state_.status_message = result.error_message;
        ui_.result_popup_message = std::move(result.error_message);
        ui_.result_popup_success = false;
        ui_.show_result_popup = true;
    }
}

void App::dispatchCommands(std::vector<AppCommand>& commands) {
    for (auto& cmd : commands) {
        std::visit(
            Overloaded{
                [&](OpenFileCmd& c) { openFile(c.path); },
                [&](RefreshDisplayCmd&) {
                    if (tex_state_.texture)
                        viewer_.refreshDisplay(*tex_state_.texture);
                },
                [&](RegenerateMipmapsCmd& c) {
                    if (!tex_state_.texture)
                        return;
                    auto op = texture_service_.regenerateMipmaps(*tex_state_.texture, c.mip_count);
                    if (op.success)
                        viewer_.setTexture(*tex_state_.texture);
                    ui_.result_popup_message = std::move(op.message);
                    ui_.result_popup_success = op.success;
                    ui_.show_result_popup = true;
                },
                [&](DownscaleCmd& c) {
                    if (!tex_state_.texture)
                        return;
                    auto op = texture_service_.downscale(*tex_state_.texture, c.levels);
                    if (op.success)
                        viewer_.setTexture(*tex_state_.texture);
                    ui_.result_popup_message = std::move(op.message);
                    ui_.result_popup_success = op.success;
                    ui_.show_result_popup = true;
                },
                [&](StartUpscaleCmd& c) {
#ifdef WHITEOUT_HAS_UPSCALER
                    if (tex_state_.texture && !upscaler_service_.isRunning()) {
                        upscale_model_index_ = c.model_index;
                        image_details_.setUpscaleInProgress(true);
                        upscaler_service_.startAsync(upscale_models_[upscale_model_index_],
                                                     *tex_state_.texture, c.upscale_alpha);
                    }
#else
                    (void)c;
#endif
                },
                [&](ShowResultPopupCmd& c) {
                    ui_.result_popup_message = std::move(c.message);
                    ui_.result_popup_success = c.success;
                    ui_.show_result_popup = true;
                },
                [&](SelectMipCmd& c) { viewer_.selectMip(c.level); },
                [&](ShowOpenDialogCmd&) {
                    const char* default_dir = save_prefs_.last_open_dir.empty()
                                                  ? nullptr
                                                  : save_prefs_.last_open_dir.c_str();
                    SDL_ShowOpenFileDialog(file_dialog_callback, &open_dialog_state_, window_,
                                           OPEN_FILTERS, static_cast<i32>(std::size(OPEN_FILTERS)),
                                           default_dir, false);
                },
                [&](ShowSaveDialogCmd&) {
                    save_dialog_.buildFilterOrder(save_prefs_);
                    const char* save_default = save_prefs_.last_save_dir.empty()
                                                   ? nullptr
                                                   : save_prefs_.last_save_dir.c_str();
                    SDL_ShowSaveFileDialog(file_dialog_callback, &save_dialog_state_, window_,
                                           save_dialog_.filterData(), save_dialog_.filterCount(),
                                           save_default);
                },
                [&](OpenBatchConvertCmd&) {
#ifdef WHITEOUT_HAS_UPSCALER
                    batch_convert_.setUpscalerModels(
                        Upscaler::availableModels(Upscaler::defaultModelDir()));
#endif
                    batch_convert_.open(batch_prefs_);
                },
                [&](OpenCascBrowserCmd&) { casc_browser_.open(); },
                [&](ShowUpscaleDialogCmd&) {
#ifdef WHITEOUT_HAS_UPSCALER
                    upscale_models_ = UpscalerService::availableModels();
                    upscale_model_index_ = 0;
                    ui_.show_upscale_dialog = true;
#endif
                },
                [&](ShowAboutCmd&) { ui_.show_about = true; },
                [&](ClearRecentFilesCmd&) { recent_files_.paths.clear(); },
                [&](LoadCascTextureCmd& c) {
                    TextureLoadResult load_result;
                    if (c.is_d4_tex) {
                        load_result =
                            texture_service_.loadD4FromMemory(c.name, c.data, c.payload, c.paylow);
                    } else {
                        auto fmt = tex::TextureConverter::classifyPath(c.name);
                        load_result = texture_service_.loadFromMemory(c.name, c.data, fmt);
                    }
                    applyLoadResult(c.name, std::move(load_result));
                },
                [&](ApplyBC3NSwapCmd&) {
                    if (tex_state_.texture) {
                        TextureService::applyBC3NSwap(*tex_state_.texture);
                        viewer_.setTexture(*tex_state_.texture);
                    }
                },
                [&](LoadD4PayloadCmd& c) {
                    auto result = texture_service_.loadD4WithPayload(
                        c.meta_path, c.payload_path,
                        c.paylow_path.empty() ? std::string{} : c.paylow_path);
                    applyLoadResult(c.meta_path, std::move(result));
                },
                [&](OpenCascCmd&) { /* reserved for future use */ },
            },
            cmd);
    }
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
    rememberParentDir(path, save_prefs_.last_open_dir);
}

void App::processSaveResult() {
    if (!save_dialog_state_.has_pending.load())
        return;

    std::string path;
    i32 filter_idx;
    {
        std::lock_guard lock(save_dialog_state_.mtx);
        path = std::move(save_dialog_state_.pending_path);
        filter_idx = save_dialog_state_.pending_filter;
        save_dialog_state_.has_pending.store(false);
    }

    save_dialog_.onFileChosen(path, filter_idx, save_prefs_,
                              tex_state_.texture ? &*tex_state_.texture : nullptr);
    rememberParentDir(path, save_prefs_.last_save_dir);
}

#ifdef WHITEOUT_HAS_UPSCALER
void App::pollUpscaleResult() {
    auto upscale_result = upscaler_service_.pollResult();
    if (!upscale_result)
        return;

    image_details_.setUpscaleInProgress(false);
    if (upscale_result->success && upscale_result->texture) {
        tex_state_.texture = std::move(*upscale_result->texture);
        tex_state_.source_fmt = tex::PixelFormat::RGBA8;
        viewer_.setTexture(*tex_state_.texture);
        ui_.result_popup_message = "Upscale complete.";
        ui_.result_popup_success = true;
    } else {
        ui_.result_popup_message = std::move(upscale_result->status_message);
        ui_.result_popup_success = false;
    }
    ui_.show_result_popup = true;
}
#endif

// ============================================================================
// Main loop
// ============================================================================

i32 App::run(i32 argc, char** argv) {
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
    recent_batch_input_dirs_ =
        load_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchInputDirs]");
    recent_batch_output_dirs_ =
        load_recent_paths(imgui_ini_path_, "[WhiteoutTex][RecentBatchOutputDirs]");
    save_dialog_.buildFilterOrder(save_prefs_);

#ifdef WHITEOUT_HAS_UPSCALER
    // Discover available upscaler models and pass to image details panel.
    upscale_models_ = UpscalerService::availableModels();
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

        // --- Menu bar ---
        {
            constexpr bool kHasUpscaler =
#ifdef WHITEOUT_HAS_UPSCALER
                true;
#else
                false;
#endif
            auto cmds =
                drawMenuBar(tex_state_.texture.has_value(), recent_files_.paths, kHasUpscaler);
            dispatchCommands(cmds);
        }

        // Main fullscreen window
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##MainWindow", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        const f32 available_width = ImGui::GetContentRegionAvail().x;
        const f32 available_height = ImGui::GetContentRegionAvail().y;
        const f32 left_panel_width = available_width * 0.4f;

        const bool show_mip_list = tex_state_.texture && tex_state_.texture->mipCount() > 1;
        const f32 mip_list_height = show_mip_list ? available_height * 0.3f : 0.0f;
        const f32 details_height = available_height - mip_list_height;

        // Left column
        ImGui::BeginGroup();
        {
            auto cmds = image_details_.drawDetailsPanel(
                tex_state_.texture ? &*tex_state_.texture : nullptr, tex_state_.path,
                tex_state_.file_format, tex_state_.source_fmt, left_panel_width, details_height);
            dispatchCommands(cmds);

#ifdef WHITEOUT_HAS_UPSCALER
            pollUpscaleResult();
#endif
        }
        if (show_mip_list) {
            auto cmds = image_details_.drawMipList(*tex_state_.texture, viewer_.selectedMip(),
                                                   left_panel_width, mip_list_height);
            dispatchCommands(cmds);
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // Right panel: image preview
        ImGui::BeginChild("##ImagePanel", ImVec2(0.0f, available_height), ImGuiChildFlags_Borders);
        ImGui::SeparatorText("Image Preview");

        if (viewer_.hasImage()) {
            viewer_.draw(renderer_);
        } else if (!tex_state_.status_message.empty()) {
            ImGui::TextWrapped("%s", tex_state_.status_message.c_str());
        } else {
            ImGui::Text("No image loaded. Use File > Open to load a texture.");
        }
        ImGui::EndChild();

        ImGui::End();

        // --- Popups & dialogs ---
        {
            auto cmds = save_dialog_.draw(
                converter_, tex_state_.texture ? &*tex_state_.texture : nullptr, save_prefs_);
            dispatchCommands(cmds);
        }
        {
            auto cmds = batch_convert_.draw(window_, batch_prefs_, recent_batch_input_dirs_,
                                            recent_batch_output_dirs_);
            dispatchCommands(cmds);
        }
        {
            auto cmds = casc_browser_.draw(window_, recent_casc_paths_);
            dispatchCommands(cmds);
        }

        drawAboutDialog(ui_.show_about);
        drawResultDialog(ui_);
        {
            auto cmds = drawBC3NDialog(ui_.show_bc3n_dialog);
            dispatchCommands(cmds);
        }
        {
            auto cmds = drawD4PayloadDialog(ui_);
            dispatchCommands(cmds);
        }
#ifdef WHITEOUT_HAS_UPSCALER
        {
            const i32 tw = tex_state_.texture ? static_cast<i32>(tex_state_.texture->width()) : 0;
            const i32 th = tex_state_.texture ? static_cast<i32>(tex_state_.texture->height()) : 0;
            auto cmds = drawUpscaleDialog(ui_.show_upscale_dialog, upscale_models_,
                                          upscale_model_index_, UpscalerService::isGpuAvailable(),
                                          upscaler_service_.isRunning(), upscaler_service_.status(),
                                          UpscalerService::defaultModelDir(), tw, th);
            dispatchCommands(cmds);
        }
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
