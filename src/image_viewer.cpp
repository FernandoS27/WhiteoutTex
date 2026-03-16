// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_viewer.h"

#include <algorithm>
#include <cstdio>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tex = whiteout::textures;

namespace whiteout::gui {

// ── Channel button colors ──────────────────────────────────────────────
static constexpr ImVec4 kChannelColorR{0.80f, 0.15f, 0.15f, 0.90f};
static constexpr ImVec4 kChannelColorG{0.15f, 0.65f, 0.15f, 0.90f};
static constexpr ImVec4 kChannelColorB{0.15f, 0.35f, 0.90f, 0.90f};
static constexpr ImVec4 kChannelColorA{0.55f, 0.55f, 0.55f, 0.90f};
static constexpr ImVec4 kChannelColorOff{0.18f, 0.18f, 0.18f, 0.75f};

/// Squared distance threshold below which a mouse-up is treated as a click
/// rather than a drag (in pixels^2).
static constexpr float kClickThresholdSq = 9.0f;

// ============================================================================
// Lifetime
// ============================================================================

ImageViewer::~ImageViewer() {
    if (image_texture_) {
        SDL_DestroyTexture(image_texture_);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ImageViewer::setTexture(const tex::Texture& texture, bool is_orm) {
    display_texture_ = makeDisplayTexture(texture);
    selected_mip_ = 0;
    if (is_orm) {
        channel_r_ = true;
        channel_g_ = false;
        channel_b_ = false;
        channel_a_ = false;
    } else {
        channel_r_ = channel_g_ = channel_b_ = channel_a_ = true;
    }
    auto_fit_ = true;
    pan_offset_ = ImVec2{0.0f, 0.0f};
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

void ImageViewer::refreshDisplay(const tex::Texture& texture, bool is_orm) {
    if (is_orm) {
        channel_r_ = true;
        channel_g_ = false;
        channel_b_ = false;
        channel_a_ = false;
    } else {
        channel_r_ = channel_g_ = channel_b_ = channel_a_ = true;
    }
    display_texture_ = makeDisplayTexture(texture);
    if (display_texture_ && display_texture_->mipCount() > 0) {
        if (selected_mip_ >= static_cast<int>(display_texture_->mipCount())) {
            selected_mip_ = static_cast<int>(display_texture_->mipCount()) - 1;
        }
    }
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

void ImageViewer::selectMip(int mip) {
    selected_mip_ = mip;
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

int ImageViewer::mipCount() const {
    return display_texture_ ? static_cast<int>(display_texture_->mipCount()) : 0;
}

// ============================================================================
// Drawing
// ============================================================================

void ImageViewer::draw(SDL_Renderer* renderer, bool is_orm) {
    renderer_ = renderer;

    // Ensure preview is built (first frame after setTexture when renderer wasn't set yet)
    if (display_texture_ && !image_texture_) {
        rebuildPreview(renderer);
    }

    if (!image_texture_) {
        return;
    }

    // ---- Toolbar: zoom controls (left) + channel buttons (right) ----
    if (ImGui::Button("Fit")) {
        auto_fit_ = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("1:1")) {
        auto_fit_ = false;
        zoom_scale_ = 1.0f;
        pan_offset_ = ImVec2{0.0f, 0.0f};
    }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::Text("%.0f%%", zoom_scale_ * 100.0f);

    // Channel filter buttons (right-aligned)
    struct ChannelDef {
        const char* label;
        bool* flag;
        ImVec4 on_col;
        bool visible;
    };
    ChannelDef ch_defs[4] = {
        {is_orm ? "O" : "R", &channel_r_, kChannelColorR, true},
        {is_orm ? "R" : "G", &channel_g_, kChannelColorG, true},
        {is_orm ? "M" : "B", &channel_b_, kChannelColorB, true},
        {"A", &channel_a_, kChannelColorA, !is_orm},
    };
    constexpr ImVec4 k_off_col = kChannelColorOff;

    const int visible_count = is_orm ? 3 : 4;
    const float btn_size = ImGui::GetFrameHeight() * 1.4f;
    const float btn_gap = 4.0f;
    const float total_btns_width = static_cast<float>(visible_count) * btn_size +
                                   static_cast<float>(visible_count - 1) * btn_gap;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - total_btns_width);

    bool first_visible = true;
    for (int ci = 0; ci < 4; ++ci) {
        if (!ch_defs[ci].visible)
            continue;
        if (!first_visible)
            ImGui::SameLine(0.0f, btn_gap);
        first_visible = false;
        const ImVec4& col = *ch_defs[ci].flag ? ch_defs[ci].on_col : k_off_col;
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(col.x + 0.15f, col.y + 0.15f, col.z + 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(col.x - 0.10f, col.y - 0.10f, col.z - 0.10f, 1.0f));
        char btn_id[8];
        std::snprintf(btn_id, sizeof(btn_id), "%s##ch", ch_defs[ci].label);
        if (ImGui::Button(btn_id, ImVec2(btn_size, btn_size))) {
            if (is_orm) {
                channel_r_ = (ci == 0);
                channel_g_ = (ci == 1);
                channel_b_ = (ci == 2);
                rebuildPreview(renderer);
            } else {
                *ch_defs[ci].flag = !*ch_defs[ci].flag;
                const bool any_on = channel_r_ || channel_g_ || channel_b_ || channel_a_;
                if (!any_on) {
                    *ch_defs[ci].flag = true;
                } else {
                    rebuildPreview(renderer);
                }
            }
        }
        ImGui::PopStyleColor(3);
    }

    // ---- Zoomable / pannable image display ----
    const float actual_w = ImGui::GetContentRegionAvail().x;
    const float actual_h = ImGui::GetContentRegionAvail().y;

    if (auto_fit_) {
        zoom_scale_ = std::min(actual_w / static_cast<float>(image_width_),
                               actual_h / static_cast<float>(image_height_));
        pan_offset_ = ImVec2{0.0f, 0.0f};
    }

    float disp_w = static_cast<float>(image_width_) * zoom_scale_;
    float disp_h = static_cast<float>(image_height_) * zoom_scale_;

    const ImVec2 area_pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##img_area", ImVec2(actual_w, actual_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);

    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io_ref = ImGui::GetIO();

        // Scroll wheel: zoom centred on mouse
        if (io_ref.MouseWheel != 0.0f) {
            const float factor = (io_ref.MouseWheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
            const float new_zoom = std::clamp(zoom_scale_ * factor, 0.01f, 64.0f);

            const ImVec2 mouse_local{io_ref.MousePos.x - area_pos.x,
                                     io_ref.MousePos.y - area_pos.y};
            const float ix = actual_w * 0.5f + pan_offset_.x - disp_w * 0.5f;
            const float iy = actual_h * 0.5f + pan_offset_.y - disp_h * 0.5f;
            const float uvx = (mouse_local.x - ix) / disp_w;
            const float uvy = (mouse_local.y - iy) / disp_h;

            const float new_dw = static_cast<float>(image_width_) * new_zoom;
            const float new_dh = static_cast<float>(image_height_) * new_zoom;
            pan_offset_.x = mouse_local.x - uvx * new_dw - actual_w * 0.5f + new_dw * 0.5f;
            pan_offset_.y = mouse_local.y - uvy * new_dh - actual_h * 0.5f + new_dh * 0.5f;
            zoom_scale_ = new_zoom;
            auto_fit_ = false;
            disp_w = new_dw;
            disp_h = new_dh;
        }

        // Drag: pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 delta = io_ref.MouseDelta;
            pan_offset_.x += delta.x;
            pan_offset_.y += delta.y;
            auto_fit_ = false;
        }

        // Click actions
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            const ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            if (dd.x * dd.x + dd.y * dd.y < kClickThresholdSq) {
                auto_fit_ = false;
                zoom_scale_ = 1.0f;
                pan_offset_ = ImVec2{0.0f, 0.0f};
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
            const ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            if (dd.x * dd.x + dd.y * dd.y < kClickThresholdSq) {
                auto_fit_ = true;
                pan_offset_ = ImVec2{0.0f, 0.0f};
            }
        }
    }

    // Draw the image clipped to the panel area
    const float img_x = area_pos.x + actual_w * 0.5f + pan_offset_.x - disp_w * 0.5f;
    const float img_y = area_pos.y + actual_h * 0.5f + pan_offset_.y - disp_h * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(area_pos, ImVec2(area_pos.x + actual_w, area_pos.y + actual_h), true);
    dl->AddImage(ImTextureRef(image_texture_), ImVec2(img_x, img_y),
                 ImVec2(img_x + disp_w, img_y + disp_h));
    dl->PopClipRect();
}

// ============================================================================
// Private helpers
// ============================================================================

std::vector<u8> ImageViewer::applyChannelFilter(const u8* data, int width, int height,
                                                     bool show_r, bool show_g, bool show_b,
                                                     bool show_a) {
    const int count = width * height;
    std::vector<u8> out(static_cast<size_t>(count) * 4);
    const int active = (show_r ? 1 : 0) + (show_g ? 1 : 0) + (show_b ? 1 : 0) + (show_a ? 1 : 0);
    for (int i = 0; i < count; ++i) {
        const u8 r = data[i * 4 + 0];
        const u8 g = data[i * 4 + 1];
        const u8 b = data[i * 4 + 2];
        const u8 a = data[i * 4 + 3];
        if (active == 1) {
            u8 val = show_r ? r : (show_g ? g : (show_b ? b : a));
            out[i * 4 + 0] = val;
            out[i * 4 + 1] = val;
            out[i * 4 + 2] = val;
            out[i * 4 + 3] = 255;
        } else {
            out[i * 4 + 0] = show_r ? r : 0;
            out[i * 4 + 1] = show_g ? g : 0;
            out[i * 4 + 2] = show_b ? b : 0;
            out[i * 4 + 3] = show_a ? a : 255;
        }
    }
    return out;
}

SDL_Texture* ImageViewer::createTextureFromRGBA8(SDL_Renderer* renderer, const u8* data,
                                                 int width, int height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return nullptr;
    }
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    return texture;
}

tex::Texture ImageViewer::makeDisplayTexture(const tex::Texture& texture) {
    if (texture.kind() == tex::TextureKind::Normal) {
        if (auto expanded = texture.copyFromNormalToRGBA()) {
            return std::move(*expanded);
        }
    }
    auto result = texture.copyAsFormat(tex::PixelFormat::RGBA8);
    const auto src_fmt = texture.format();
    if (src_fmt == tex::PixelFormat::R8 || src_fmt == tex::PixelFormat::R16 ||
        src_fmt == tex::PixelFormat::R32F || src_fmt == tex::PixelFormat::BC4) {
        auto span = result.data();
        for (size_t i = 0; i + 3 < span.size(); i += 4) {
            span[i + 1] = span[i]; // G = R
            span[i + 2] = span[i]; // B = R
        }
    }
    return result;
}

void ImageViewer::rebuildPreview(SDL_Renderer* renderer) {
    if (!display_texture_)
        return;
    if (image_texture_) {
        SDL_DestroyTexture(image_texture_);
        image_texture_ = nullptr;
    }
    const auto mip_idx = static_cast<u32>(selected_mip_);
    const auto& dml = display_texture_->mipLevel(mip_idx);
    image_width_ = static_cast<int>(dml.width);
    image_height_ = static_cast<int>(dml.height);
    auto mip_data = display_texture_->mipData(mip_idx);
    if (channel_r_ && channel_g_ && channel_b_ && channel_a_) {
        image_texture_ =
            createTextureFromRGBA8(renderer, mip_data.data(), image_width_, image_height_);
    } else {
        auto filtered = applyChannelFilter(mip_data.data(), image_width_, image_height_, channel_r_,
                                           channel_g_, channel_b_, channel_a_);
        image_texture_ =
            createTextureFromRGBA8(renderer, filtered.data(), image_width_, image_height_);
    }
}

} // namespace whiteout::gui
