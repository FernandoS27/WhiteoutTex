// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <vector>

#include "common_types.h"

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace whiteout::gui {

/// Manages texture preview display including channel filtering, mip
/// selection, zoom/pan, and GPU texture upload.
class ImageViewer {
public:
    ImageViewer() = default;
    ~ImageViewer();

    ImageViewer(const ImageViewer&) = delete;
    ImageViewer& operator=(const ImageViewer&) = delete;

    /// Replace the loaded texture and reset the viewer state.
    /// @param texture     The source texture (in its original pixel format).
    /// @param is_orm      Whether this is an ORM texture (changes default channel filter).
    void setTexture(const whiteout::textures::Texture& texture, bool is_orm);

    /// Rebuild the GPU preview after a kind change (e.g. normal-map expansion).
    void refreshDisplay(const whiteout::textures::Texture& texture, bool is_orm);

    /// Returns true if a texture has been loaded (may still need GPU upload).
    bool hasImage() const {
        return display_texture_.has_value();
    }

    /// Draw the toolbar (zoom controls + channel buttons) and the zoomable
    /// image area.  Must be called inside an ImGui child/window.
    void draw(SDL_Renderer* renderer, bool is_orm);

    // ── Accessors for mip selection (used by details panel) ────────────
    int selectedMip() const {
        return selected_mip_;
    }
    void selectMip(int mip);
    int mipCount() const;

private:
    /// Apply per-channel filter to RGBA8 pixel data.
    static std::vector<u8> applyChannelFilter(const u8* data, int width, int height,
                                              bool show_r, bool show_g, bool show_b,
                                              bool show_a);

    /// Upload RGBA8 pixel data into an SDL_Texture.
    static SDL_Texture* createTextureFromRGBA8(SDL_Renderer* renderer, const u8* data,
                                               int width, int height);

    /// Build an RGBA8 display copy (handles normal-map expansion).
    static whiteout::textures::Texture makeDisplayTexture(
        const whiteout::textures::Texture& texture);

    /// Rebuild the SDL preview texture from the current display_texture_
    /// at the selected mip with the active channel filter.
    void rebuildPreview(SDL_Renderer* renderer);

    // Display state
    std::optional<whiteout::textures::Texture> display_texture_;
    SDL_Texture* image_texture_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int image_width_ = 0;
    int image_height_ = 0;
    int selected_mip_ = 0;

    // Channel filter
    bool channel_r_ = true;
    bool channel_g_ = true;
    bool channel_b_ = true;
    bool channel_a_ = true;

    // Zoom / pan
    bool auto_fit_ = true;
    float zoom_scale_ = 1.0f;
    ImVec2 pan_offset_{0.0f, 0.0f};
};

} // namespace whiteout::gui
