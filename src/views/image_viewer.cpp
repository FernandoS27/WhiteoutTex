// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "image_viewer.h"
#include "localization.h"
#include "save_helpers.h"
#include "services/texture_service.h"
#include "thread_pool_manager.h"

#include <algorithm>
#include <cstdio>

#include <whiteout/textures/texture.h>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace tex = whiteout::textures;
namespace interfaces = whiteout::interfaces;

namespace whiteout::textool::views {

using namespace services;
using i18n::tr;
namespace {

// ── Channel button colors ──────────────────────────────────────────────
constexpr ImVec4 kChannelColorR{0.58f, 0.28f, 0.28f, 0.90f};
constexpr ImVec4 kChannelColorG{0.28f, 0.50f, 0.28f, 0.90f};
constexpr ImVec4 kChannelColorB{0.28f, 0.38f, 0.62f, 0.90f};
constexpr ImVec4 kChannelColorA{0.48f, 0.48f, 0.48f, 0.90f};
constexpr ImVec4 kChannelColorOff{0.18f, 0.18f, 0.18f, 0.75f};

/// Short label for a TextureKind (used for Multikind channel buttons).
const char* channelKindShortLabel(tex::TextureKind k) {
    switch (k) {
    case tex::TextureKind::AmbientOcclusion:
        return "O";
    case tex::TextureKind::Roughness:
        return "R";
    case tex::TextureKind::Metalness:
        return "M";
    case tex::TextureKind::Gloss:
        return "G";
    case tex::TextureKind::Albedo:
        return "Al";
    case tex::TextureKind::Diffuse:
        return "D";
    case tex::TextureKind::Normal:
        return "N";
    case tex::TextureKind::Specular:
        return "S";
    case tex::TextureKind::Emissive:
        return "E";
    case tex::TextureKind::AlphaMask:
        return "A";
    case tex::TextureKind::BinaryMask:
        return "Bm";
    case tex::TextureKind::TransparencyMask:
        return "T";
    case tex::TextureKind::BlendMask:
        return "Bl";
    case tex::TextureKind::Lightmap:
        return "L";
    default:
        return "?";
    }
}

/// Squared distance threshold below which a mouse-up is treated as a click
/// rather than a drag (in pixels^2).
constexpr f32 kClickThresholdSq = 9.0f;

/// Zoom multiplier per mouse wheel tick.
constexpr f32 kZoomFactor = 1.15f;

/// Minimum and maximum zoom levels.
constexpr f32 kZoomMin = 0.01f;
constexpr f32 kZoomMax = 64.0f;

/// Channel button size multiplier relative to frame height.
constexpr f32 kChannelBtnSizeMultiplier = 1.4f;

/// Gap between channel buttons in pixels.
constexpr f32 kChannelBtnGap = 4.0f;

/// Width of the cube face combobox.
constexpr f32 kFaceComboW = 65.0f;

/// Width of the array index combobox.
constexpr f32 kArrayComboW = 90.0f;

/// Width of the volume Z-slice combobox.
constexpr f32 kSliceComboW = 110.0f;

/// Gap between comboboxes and the channel buttons.
constexpr f32 kComboGap = 8.0f;

/// Largest edge, in pixels, an all-layers layout may reach.  A deep volume
/// tiles into something no GPU will hold and no screen will show — beyond this
/// the layout is skipped and the per-slice view stands.
constexpr i32 kMaxLayoutEdge = 8192;

} // anonymous namespace

// ============================================================================
// Lifetime
// ============================================================================

ImageViewer::~ImageViewer() {
    if (image_texture_) {
        SDL_DestroyTexture(image_texture_);
    }
    if (unwrap_texture_) {
        SDL_DestroyTexture(unwrap_texture_);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ImageViewer::setTexture(const tex::Texture& texture) {
    updateChannelInfo(texture);

    // Detect cube / cube-array / 2D-array / volume topology.
    const auto tex_type = texture.type();
    is_cube_ = (tex_type == tex::TextureType::TextureCube ||
                tex_type == tex::TextureType::TextureCubeArray);
    is_cube_array_ = (tex_type == tex::TextureType::TextureCubeArray);
    is_2d_array_   = (tex_type == tex::TextureType::Texture2DArray);
    is_3d_         = (tex_type == tex::TextureType::Texture3D);
    array_size_ = (is_cube_array_ || is_2d_array_) ? static_cast<i32>(texture.arraySize()) : 1;
    selected_face_ = 0;
    selected_array_index_ = 0;
    selected_slice_ = 0;
    show_unwrap_ = false;
    if (unwrap_texture_) {
        SDL_DestroyTexture(unwrap_texture_);
        unwrap_texture_ = nullptr;
    }

    auto* pool = threadPoolManager().get();
    display_texture_ = TextureService::makeDisplayTexture(texture, pool);
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
    display_texture_ = TextureService::makeDisplayTexture(texture, pool);
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
    if (ImGui::Button(tr("viewer.fit"))) {
        auto_fit_ = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(tr("viewer.one_to_one"))) {
        auto_fit_ = false;
        zoom_scale_ = 1.0f;
        pan_offset_ = ImVec2{0.0f, 0.0f};
    }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::Text("%.0f%%", zoom_scale_ * 100.0f);

    // ---- All-layers-at-once toggle (cube cross / volume slice grid) ----
    if (is_cube_ || is_3d_) {
        ImGui::SameLine(0.0f, 8.0f);
        const ImVec4 active_col{0.30f, 0.55f, 0.30f, 1.0f};
        if (show_unwrap_)
            ImGui::PushStyleColor(ImGuiCol_Button, active_col);
        if (ImGui::Button(is_3d_ ? tr("viewer.all_slices") : tr("viewer.unwrap"))) {
            show_unwrap_ = !show_unwrap_;
            if (show_unwrap_) {
                buildUnwrapTexture(renderer);
                // Too large to lay out, or the upload failed — stay on the
                // per-face / per-slice view rather than latching a dead button.
                show_unwrap_ = unwrap_texture_ != nullptr;
            }
            auto_fit_ = true;
            pan_offset_ = ImVec2{0.0f, 0.0f};
        }
        if (show_unwrap_)
            ImGui::PopStyleColor();
    }

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
    const f32 total_btns_width =
        static_cast<f32>(visible_count) * btn_size + static_cast<f32>(visible_count - 1) * btn_gap;

    f32 total_right_width = total_btns_width;
    if (!show_unwrap_) {
        if (is_cube_)                       total_right_width += kFaceComboW  + kComboGap;
        if (is_cube_array_ || is_2d_array_) total_right_width += kArrayComboW + kComboGap;
        if (is_3d_)                         total_right_width += kSliceComboW + kComboGap;
    }

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - total_right_width);

    // ---- Z-slice combobox (volume textures, hidden in slice-grid mode) ----
    if (!show_unwrap_ && is_3d_) {
        const i32 slices = sliceCountAtSelectedMip();
        ImGui::SetNextItemWidth(kSliceComboW);
        char preview[32];
        std::snprintf(preview, sizeof(preview), tr("viewer.slice"), selected_slice_, slices);
        if (ImGui::BeginCombo("##slice", preview)) {
            for (i32 i = 0; i < slices; ++i) {
                char item[32];
                std::snprintf(item, sizeof(item), tr("viewer.slice"), i, slices);
                const bool is_selected = (i == selected_slice_);
                if (ImGui::Selectable(item, is_selected)) {
                    selected_slice_ = i;
                    rebuildPreview(renderer);
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, kComboGap);
    }

    // ---- Array index combobox (CubeArray or 2DArray, hidden in unwrap mode) ----
    if (!show_unwrap_ && (is_cube_array_ || is_2d_array_)) {
        ImGui::SetNextItemWidth(kArrayComboW);
        char arr_preview[16];
        std::snprintf(arr_preview, sizeof(arr_preview), tr("viewer.array"), selected_array_index_);
        if (ImGui::BeginCombo("##arr", arr_preview)) {
            for (i32 i = 0; i < array_size_; ++i) {
                char item[16];
                std::snprintf(item, sizeof(item), tr("viewer.array"), i);
                const bool is_selected = (i == selected_array_index_);
                if (ImGui::Selectable(item, is_selected)) {
                    selected_array_index_ = i;
                    rebuildPreview(renderer);
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, kComboGap);
    }

    // ---- Cube face combobox (Cube and CubeArray, hidden in unwrap mode) ----
    if (!show_unwrap_ && is_cube_) {
        static constexpr const char* kFaceLabels[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        ImGui::SetNextItemWidth(kFaceComboW);
        if (ImGui::BeginCombo("##face", kFaceLabels[selected_face_])) {
            for (i32 i = 0; i < 6; ++i) {
                const bool is_selected = (i == selected_face_);
                if (ImGui::Selectable(kFaceLabels[i], is_selected)) {
                    selected_face_ = i;
                    rebuildPreview(renderer);
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, kComboGap);
    }

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
    // In unwrap mode, show the cross layout; otherwise the per-face preview.
    SDL_Texture* tex = (show_unwrap_ && unwrap_texture_) ? unwrap_texture_ : image_texture_;
    const i32 w = (show_unwrap_ && unwrap_texture_) ? unwrap_w_ : image_width_;
    const i32 h = (show_unwrap_ && unwrap_texture_) ? unwrap_h_ : image_height_;

    const f32 actual_w = ImGui::GetContentRegionAvail().x;
    const f32 actual_h = ImGui::GetContentRegionAvail().y;

    if (auto_fit_) {
        zoom_scale_ = std::min(actual_w / static_cast<f32>(w),
                               actual_h / static_cast<f32>(h));
        pan_offset_ = ImVec2{0.0f, 0.0f};
    }

    f32 disp_w = static_cast<f32>(w) * zoom_scale_;
    f32 disp_h = static_cast<f32>(h) * zoom_scale_;

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

            const f32 new_dw = static_cast<f32>(w) * new_zoom;
            const f32 new_dh = static_cast<f32>(h) * new_zoom;
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
    dl->AddImage(ImTextureRef(tex), ImVec2(img_x, img_y),
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
        static const char kDefaults[] = {'R', 'G', 'B', 'A'};
        for (i32 i = 0; i < 4; ++i) {
            auto ch_kind = texture.channelKind(kRGBAChannels[i]);
            const bool used = (ch_kind != tex::TextureKind::Unused);
            // Always keep the alpha channel selectable so its contents can be
            // inspected even when it carries no assigned kind — standard ORM
            // marks alpha Unused, but the data may still be meaningful.
            channel_info_[i].visible = used || (i == 3);
            if (used) {
                const char* lbl = channelKindShortLabel(ch_kind);
                std::snprintf(channel_info_[i].label, sizeof(channel_info_[i].label), "%s", lbl);
            } else {
                // Unused channel kept visible (alpha): fall back to its RGBA letter.
                channel_info_[i].label[0] = kDefaults[i];
                channel_info_[i].label[1] = '\0';
            }
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

SDL_Texture* ImageViewer::createTextureFromRGBA8(SDL_Renderer* renderer, const u8* data, i32 width,
                                                 i32 height) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return nullptr;
    }
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    return texture;
}

i32 ImageViewer::sliceCountAtSelectedMip() const {
    if (!is_3d_ || !display_texture_ || display_texture_->mipCount() == 0)
        return 1;
    const auto mip = std::min(static_cast<u32>(std::max(selected_mip_, 0)),
                              display_texture_->mipCount() - 1);
    return static_cast<i32>(std::max(display_texture_->mipLevel(mip).depth, 1u));
}

std::span<const u8> ImageViewer::sliceData(u32 mip, u32 layer, i32 slice) const {
    auto data = display_texture_->mipData(mip, layer);
    if (!is_3d_)
        return data;

    // A volume's mip is its Z slices back to back; the display copy is RGBA8,
    // so a slice is exactly width * height * 4 bytes.
    const auto& ml = display_texture_->mipLevel(mip, layer);
    const std::size_t slice_bytes = static_cast<std::size_t>(ml.width) * ml.height * 4u;
    const std::size_t offset = static_cast<std::size_t>(std::max(slice, 0)) * slice_bytes;
    if (slice_bytes == 0 || offset + slice_bytes > data.size())
        return data.first(std::min(slice_bytes, data.size()));
    return data.subspan(offset, slice_bytes);
}

void ImageViewer::rebuildPreview(SDL_Renderer* renderer) {
    if (!display_texture_)
        return;
    if (image_texture_) {
        SDL_DestroyTexture(image_texture_);
        image_texture_ = nullptr;
    }
    const auto mip_idx = static_cast<u32>(selected_mip_);
    const u32 layer_idx =
        is_cube_     ? static_cast<u32>(selected_array_index_) * 6u + static_cast<u32>(selected_face_)
        : is_2d_array_ ? static_cast<u32>(selected_array_index_)
                       : 0u;
    // Depth halves with every mip level, so the slice picked at a coarser mip
    // may no longer exist at a finer one (and vice versa).
    selected_slice_ = std::clamp(selected_slice_, 0, sliceCountAtSelectedMip() - 1);

    const auto& dml = display_texture_->mipLevel(mip_idx, layer_idx);
    image_width_ = static_cast<i32>(dml.width);
    image_height_ = static_cast<i32>(dml.height);
    auto mip_data = sliceData(mip_idx, layer_idx, selected_slice_);
    if (channel_r_ && channel_g_ && channel_b_ && channel_a_) {
        image_texture_ =
            createTextureFromRGBA8(renderer, mip_data.data(), image_width_, image_height_);
    } else {
        auto filtered =
            TextureService::applyChannelFilter(mip_data.data(), image_width_, image_height_,
                                               channel_r_, channel_g_, channel_b_, channel_a_);
        image_texture_ =
            createTextureFromRGBA8(renderer, filtered.data(), image_width_, image_height_);
    }
    // Rebuild unwrap if active.
    if (show_unwrap_)
        buildUnwrapTexture(renderer);
}

// ============================================================================
// All-layers layouts (cube cross / volume slice grid)
// ============================================================================

void ImageViewer::buildUnwrapTexture(SDL_Renderer* renderer) {
    if (!display_texture_)
        return;
    if (unwrap_texture_) {
        SDL_DestroyTexture(unwrap_texture_);
        unwrap_texture_ = nullptr;
    }

    const u32 mip = static_cast<u32>(selected_mip_);

    // ---- Volume: every Z slice laid out as its own tile ----
    if (is_3d_) {
        const auto& vml = display_texture_->mipLevel(mip);
        const i32 sw = static_cast<i32>(vml.width);
        const i32 sh = static_cast<i32>(vml.height);
        const i32 slices = sliceCountAtSelectedMip();

        // As square a grid as the slice count allows, so a 16-slice LUT reads
        // as 4x4 rather than a 16-tile strip nothing can fit on screen.
        i32 cols = 1;
        while (cols * cols < slices)
            ++cols;
        const i32 rows = (slices + cols - 1) / cols;

        const i32 grid_w = sw * cols;
        const i32 grid_h = sh * rows;
        if (grid_w > kMaxLayoutEdge || grid_h > kMaxLayoutEdge)
            return; // leaves unwrap_texture_ null; drawToolbar drops the toggle
        std::vector<u8> grid(static_cast<size_t>(grid_w) * grid_h * 4, u8{0});

        for (i32 z = 0; z < slices; ++z) {
            auto slice_span = sliceData(mip, 0, z);

            std::vector<u8> filtered;
            const u8* src = slice_span.data();
            if (!(channel_r_ && channel_g_ && channel_b_ && channel_a_)) {
                filtered = TextureService::applyChannelFilter(
                    src, sw, sh, channel_r_, channel_g_, channel_b_, channel_a_);
                src = filtered.data();
            }

            const i32 dst_x = (z % cols) * sw;
            const i32 dst_y = (z / cols) * sh;
            for (i32 y = 0; y < sh; ++y) {
                const u8* src_row = src + static_cast<ptrdiff_t>(y) * sw * 4;
                u8* dst_row = grid.data() +
                              (static_cast<ptrdiff_t>(dst_y + y) * grid_w + dst_x) * 4;
                std::copy_n(src_row, static_cast<size_t>(sw) * 4, dst_row);
            }
        }

        unwrap_w_ = grid_w;
        unwrap_h_ = grid_h;
        unwrap_texture_ = createTextureFromRGBA8(renderer, grid.data(), grid_w, grid_h);
        return;
    }

    const u32 base_layer = static_cast<u32>(selected_array_index_) * 6u;

    const auto& dml = display_texture_->mipLevel(mip, base_layer);
    const i32 fw = static_cast<i32>(dml.width);
    const i32 fh = static_cast<i32>(dml.height);

    // Horizontal cross layout (4fw x 3fh):
    //   col:  0     1     2     3
    // row 0:  .     .    +Y     .
    // row 1: -X    +Z    +X    -Z
    // row 2:  .     .    -Y     .
    static constexpr std::pair<i32, i32> kLayout[6] = {
        {2, 1}, // face 0: +X
        {0, 1}, // face 1: -X
        {1, 0}, // face 2: +Y  (cap over +Z)
        {1, 2}, // face 3: -Y  (cap over +Z)
        {1, 1}, // face 4: +Z
        {3, 1}, // face 5: -Z
    };

    const i32 cross_w = fw * 4;
    const i32 cross_h = fh * 3;
    if (cross_w > kMaxLayoutEdge || cross_h > kMaxLayoutEdge)
        return; // leaves unwrap_texture_ null; drawToolbar drops the toggle
    std::vector<u8> cross(static_cast<size_t>(cross_w) * cross_h * 4, u8{0});

    for (i32 face = 0; face < 6; ++face) {
        const u32 layer = base_layer + static_cast<u32>(face);
        auto face_span = display_texture_->mipData(mip, layer);

        std::vector<u8> filtered;
        const u8* src = face_span.data();
        if (!(channel_r_ && channel_g_ && channel_b_ && channel_a_)) {
            filtered = TextureService::applyChannelFilter(
                src, fw, fh, channel_r_, channel_g_, channel_b_, channel_a_);
            src = filtered.data();
        }

        const auto [col, row] = kLayout[face];
        const i32 dst_x = col * fw;
        const i32 dst_y = row * fh;
        for (i32 y = 0; y < fh; ++y) {
            const u8* src_row = src + static_cast<ptrdiff_t>(y * fw * 4);
            u8* dst_row = cross.data() +
                          static_cast<ptrdiff_t>((dst_y + y) * cross_w * 4 + dst_x * 4);
            std::copy_n(src_row, fw * 4, dst_row);
        }
    }

    unwrap_w_ = cross_w;
    unwrap_h_ = cross_h;
    unwrap_texture_ = createTextureFromRGBA8(renderer, cross.data(), cross_w, cross_h);
}

} // namespace whiteout::textool::views
