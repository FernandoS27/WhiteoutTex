// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_viewer.h"
#include "save_helpers.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cstdio>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tex = whiteout::textures;
namespace interfaces = whiteout::interfaces;

namespace whiteout::gui {

// ── Channel button colors ──────────────────────────────────────────────
static constexpr ImVec4 kChannelColorR{0.58f, 0.28f, 0.28f, 0.90f};
static constexpr ImVec4 kChannelColorG{0.28f, 0.50f, 0.28f, 0.90f};
static constexpr ImVec4 kChannelColorB{0.28f, 0.38f, 0.62f, 0.90f};
static constexpr ImVec4 kChannelColorA{0.48f, 0.48f, 0.48f, 0.90f};
static constexpr ImVec4 kChannelColorOff{0.18f, 0.18f, 0.18f, 0.75f};

/// Short label for a TextureKind (used for Multikind channel buttons).
static const char* channelKindShortLabel(tex::TextureKind k) {
    switch (k) {
    case tex::TextureKind::AmbientOcclusion: return "O";
    case tex::TextureKind::Roughness:        return "R";
    case tex::TextureKind::Metalness:        return "M";
    case tex::TextureKind::Gloss:            return "G";
    case tex::TextureKind::Albedo:           return "Al";
    case tex::TextureKind::Diffuse:          return "D";
    case tex::TextureKind::Normal:           return "N";
    case tex::TextureKind::Specular:         return "S";
    case tex::TextureKind::Emissive:         return "E";
    case tex::TextureKind::AlphaMask:        return "A";
    case tex::TextureKind::BinaryMask:       return "Bm";
    case tex::TextureKind::TransparencyMask: return "T";
    case tex::TextureKind::BlendMask:        return "Bl";
    case tex::TextureKind::Lightmap:         return "L";
    default:                                 return "?";
    }
}

/// Squared distance threshold below which a mouse-up is treated as a click
/// rather than a drag (in pixels^2).
static constexpr f32 kClickThresholdSq = 9.0f;

/// Zoom multiplier per mouse wheel tick.
static constexpr f32 kZoomFactor = 1.15f;

/// Minimum and maximum zoom levels.
static constexpr f32 kZoomMin = 0.01f;
static constexpr f32 kZoomMax = 64.0f;

/// Channel button size multiplier relative to frame height.
static constexpr f32 kChannelBtnSizeMultiplier = 1.4f;

/// Gap between channel buttons in pixels.
static constexpr f32 kChannelBtnGap = 4.0f;

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

void ImageViewer::setTexture(const tex::Texture& texture) {
    updateChannelInfo(texture);
    auto* pool = threadPoolManager().get();
    display_texture_ = makeDisplayTexture(texture, pool);
    selected_mip_ = 0;
    resetChannelVisibility();
    auto_fit_ = true;
    pan_offset_ = ImVec2{0.0f, 0.0f};
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

void ImageViewer::refreshDisplay(const tex::Texture& texture) {
    updateChannelInfo(texture);
    resetChannelVisibility();
    auto* pool = threadPoolManager().get();
    display_texture_ = makeDisplayTexture(texture, pool);
    if (display_texture_ && display_texture_->mipCount() > 0) {
        if (selected_mip_ >= static_cast<i32>(display_texture_->mipCount())) {
            selected_mip_ = static_cast<i32>(display_texture_->mipCount()) - 1;
        }
    }
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

void ImageViewer::selectMip(i32 mip) {
    selected_mip_ = mip;
    if (renderer_) {
        rebuildPreview(renderer_);
    }
}

i32 ImageViewer::mipCount() const {
    return display_texture_ ? static_cast<i32>(display_texture_->mipCount()) : 0;
}

// ============================================================================
// Drawing
// ============================================================================

void ImageViewer::draw(SDL_Renderer* renderer) {
    renderer_ = renderer;

    // Ensure preview is built (first frame after setTexture when renderer wasn't set yet)
    if (display_texture_ && !image_texture_) {
        rebuildPreview(renderer);
    }

    if (!image_texture_) {
        return;
    }

    drawToolbar(renderer);
    drawImageArea(renderer);
}

// ============================================================================
// Toolbar
// ============================================================================

void ImageViewer::drawToolbar(SDL_Renderer* renderer) {
    // ---- Zoom controls (left) ----
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

    // ---- Channel filter buttons (right-aligned) ----
    struct ChannelDef {
        const char* label;
        bool* flag;
        ImVec4 on_col;
        bool visible;
    };
    ChannelDef ch_defs[4] = {
        {channel_info_[0].label, &channel_r_, kChannelColorR, channel_info_[0].visible},
        {channel_info_[1].label, &channel_g_, kChannelColorG, channel_info_[1].visible},
        {channel_info_[2].label, &channel_b_, kChannelColorB, channel_info_[2].visible},
        {channel_info_[3].label, &channel_a_, kChannelColorA, channel_info_[3].visible},
    };
    constexpr ImVec4 k_off_col = kChannelColorOff;

    i32 visible_count = 0;
    for (i32 i = 0; i < 4; ++i)
        if (ch_defs[i].visible)
            ++visible_count;
    const f32 btn_size = ImGui::GetFrameHeight() * kChannelBtnSizeMultiplier;
    const f32 btn_gap = kChannelBtnGap;
    const f32 total_btns_width = static_cast<f32>(visible_count) * btn_size +
                                   static_cast<f32>(visible_count - 1) * btn_gap;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - total_btns_width);

    bool first_visible = true;
    for (i32 ci = 0; ci < 4; ++ci) {
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
        char btn_id[16];
        std::snprintf(btn_id, sizeof(btn_id), "%s##ch%d", ch_defs[ci].label, ci);
        if (ImGui::Button(btn_id, ImVec2(btn_size, btn_size))) {
            if (is_multikind_) {
                channel_r_ = (ci == 0);
                channel_g_ = (ci == 1);
                channel_b_ = (ci == 2);
                channel_a_ = (ci == 3);
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
}

// ============================================================================
// Zoomable / pannable image area
// ============================================================================

void ImageViewer::drawImageArea(SDL_Renderer* /*renderer*/) {
    const f32 actual_w = ImGui::GetContentRegionAvail().x;
    const f32 actual_h = ImGui::GetContentRegionAvail().y;

    if (auto_fit_) {
        zoom_scale_ = std::min(actual_w / static_cast<f32>(image_width_),
                               actual_h / static_cast<f32>(image_height_));
        pan_offset_ = ImVec2{0.0f, 0.0f};
    }

    f32 disp_w = static_cast<f32>(image_width_) * zoom_scale_;
    f32 disp_h = static_cast<f32>(image_height_) * zoom_scale_;

    const ImVec2 area_pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##img_area", ImVec2(actual_w, actual_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);

    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io_ref = ImGui::GetIO();

        // Scroll wheel: zoom centred on mouse
        if (io_ref.MouseWheel != 0.0f) {
            const f32 factor = (io_ref.MouseWheel > 0.0f) ? kZoomFactor : (1.0f / kZoomFactor);
            const f32 new_zoom = std::clamp(zoom_scale_ * factor, kZoomMin, kZoomMax);

            const ImVec2 mouse_local{io_ref.MousePos.x - area_pos.x,
                                     io_ref.MousePos.y - area_pos.y};
            const f32 ix = actual_w * 0.5f + pan_offset_.x - disp_w * 0.5f;
            const f32 iy = actual_h * 0.5f + pan_offset_.y - disp_h * 0.5f;
            const f32 uvx = (mouse_local.x - ix) / disp_w;
            const f32 uvy = (mouse_local.y - iy) / disp_h;

            const f32 new_dw = static_cast<f32>(image_width_) * new_zoom;
            const f32 new_dh = static_cast<f32>(image_height_) * new_zoom;
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
    const f32 img_x = area_pos.x + actual_w * 0.5f + pan_offset_.x - disp_w * 0.5f;
    const f32 img_y = area_pos.y + actual_h * 0.5f + pan_offset_.y - disp_h * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(area_pos, ImVec2(area_pos.x + actual_w, area_pos.y + actual_h), true);
    dl->AddImage(ImTextureRef(image_texture_), ImVec2(img_x, img_y),
                 ImVec2(img_x + disp_w, img_y + disp_h));
    dl->PopClipRect();
}

// ============================================================================
// Private helpers
// ============================================================================

void ImageViewer::resetChannelVisibility() {
    if (is_multikind_) {
        // Default to showing only the first visible channel.
        channel_r_ = channel_info_[0].visible;
        channel_g_ = false;
        channel_b_ = false;
        channel_a_ = false;
    } else {
        channel_r_ = channel_g_ = channel_b_ = channel_a_ = true;
    }
}

void ImageViewer::updateChannelInfo(const tex::Texture& texture) {
    is_multikind_ = (texture.kind() == tex::TextureKind::Multikind);
    if (is_multikind_) {
        for (i32 i = 0; i < 4; ++i) {
            auto ch_kind = texture.channelKind(kRGBAChannels[i]);
            channel_info_[i].visible = (ch_kind != tex::TextureKind::Unused);
            const char* lbl = channelKindShortLabel(ch_kind);
            std::snprintf(channel_info_[i].label, sizeof(channel_info_[i].label), "%s", lbl);
        }
    } else {
        static const char kDefaults[][2] = {"R", "G", "B", "A"};
        for (i32 i = 0; i < 4; ++i) {
            channel_info_[i].visible = true;
            channel_info_[i].label[0] = kDefaults[i][0];
            channel_info_[i].label[1] = '\0';
        }
    }
}

std::vector<u8> ImageViewer::applyChannelFilter(const u8* data, i32 width, i32 height,
                                                     bool show_r, bool show_g, bool show_b,
                                                     bool show_a) {
    const i32 count = width * height;
    std::vector<u8> out(static_cast<size_t>(count) * 4);
    const i32 active = (show_r ? 1 : 0) + (show_g ? 1 : 0) + (show_b ? 1 : 0) + (show_a ? 1 : 0);
    for (i32 i = 0; i < count; ++i) {
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
                                                 i32 width, i32 height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return nullptr;
    }
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    return texture;
}

tex::Texture ImageViewer::makeDisplayTexture(const tex::Texture& texture,
                                              interfaces::WorkerPool* pool) {
    if (texture.kind() == tex::TextureKind::Normal) {
        if (auto expanded = texture.copyFromNormalToRGBA(pool)) {
            return std::move(*expanded);
        }
    }
    auto result = texture.copyAsFormat(tex::PixelFormat::RGBA8, pool);
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
    image_width_ = static_cast<i32>(dml.width);
    image_height_ = static_cast<i32>(dml.height);
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
