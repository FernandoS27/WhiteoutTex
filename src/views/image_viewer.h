// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <vector>

#include "common_types.h"

#include <whiteout/interfaces.h>
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
    void setTexture(const whiteout::textures::Texture& texture);

    /// Rebuild the GPU preview after a kind change (e.g. normal-map expansion).
    void refreshDisplay(const whiteout::textures::Texture& texture);

    /// Returns true if a texture has been loaded (may still need GPU upload).
    bool hasImage() const {
        return display_texture_.has_value();
    }

    /// Draw the toolbar (zoom controls + channel buttons) and the zoomable
    /// image area.  Must be called inside an ImGui child/window.
    void draw(SDL_Renderer* renderer);

    // ── Accessors for mip selection (used by details panel) ────────────
    i32 selectedMip() const {
        return selected_mip_;
    }
    void selectMip(i32 mip);
    i32 mipCount() const;

private:
    /// Upload RGBA8 pixel data into an SDL_Texture.
    static SDL_Texture* createTextureFromRGBA8(SDL_Renderer* renderer, const u8* data, i32 width,
                                               i32 height);

    /// Extract per-channel info (labels, visibility) from a texture.
    void updateChannelInfo(const whiteout::textures::Texture& texture);

    /// Reset channel visibility flags based on the current texture kind.
    void resetChannelVisibility();

    /// Draw the toolbar row (zoom controls + channel filter buttons).
    void drawToolbar(SDL_Renderer* renderer);

    /// Draw the zoomable / pannable image area.
    void drawImageArea(SDL_Renderer* renderer);

    /// Rebuild the SDL preview texture from the current display_texture_
    /// at the selected mip with the active channel filter.
    void rebuildPreview(SDL_Renderer* renderer);

    // Display state
    std::optional<whiteout::textures::Texture> display_texture_;
    SDL_Texture* image_texture_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    i32 image_width_ = 0;
    i32 image_height_ = 0;
    i32 selected_mip_ = 0;

    // Channel filter
    bool channel_r_ = true;
    bool channel_g_ = true;
    bool channel_b_ = true;
    bool channel_a_ = true;

    // Per-channel display info (derived from texture kind)
    struct ChannelInfo {
        char label[4] = {};
        bool visible = true;
    };
    ChannelInfo channel_info_[4] = {{"R", true}, {"G", true}, {"B", true}, {"A", true}};
    bool is_multikind_ = false;

    // Zoom / pan
    bool auto_fit_ = true;
    f32 zoom_scale_ = 1.0f;
    ImVec2 pan_offset_{0.0f, 0.0f};
};

} // namespace whiteout::gui
