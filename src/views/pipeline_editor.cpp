// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "views/pipeline_editor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // ImRect, BeginDragDropTargetCustom
#include <imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include "common_types.h" // to_lower, FolderState
#include "format_registry.h"
#include "localization.h"
#include "pipeline/node_registry.h"
#include "pipeline/serialization.h"

namespace ed = ax::NodeEditor;

namespace whiteout::textool::views {

namespace {

using pipeline::Node;
using pipeline::NodeCategory;
using pipeline::NodeGraph;
using pipeline::NodeId;
using pipeline::Param;
using pipeline::ParamWidget;
using pipeline::Pin;
using pipeline::PinRef;
using pipeline::PinType;

// A param may be gated to only appear for a given selection of an enum param
// (e.g. the mip `count` only shows in Custom mode).
bool paramVisible(Node* n, const Param& p) {
    if (p.visible_when.empty())
        return true;
    Param* gate = n->findParam(p.visible_when);
    if (gate) {
        if (std::holds_alternative<i64>(gate->value))
            return std::get<i64>(gate->value) == p.visible_when_value;
        if (std::holds_alternative<bool>(gate->value))
            return std::get<bool>(gate->value) == (p.visible_when_value != 0);
    }
    return true;
}

// File-dialog filter for pipeline documents.
const SDL_DialogFileFilter kPipelineFilters[] = {{"Pipeline (*.json)", "json"}, {"All files", "*"}};

// SDL file-dialog callback: stash the chosen path (may run off-thread).
void SDLCALL pipelineDialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* state = static_cast<FolderState*>(userdata);
    if (!filelist || !filelist[0])
        return; // cancelled / error
    std::lock_guard lock(state->mtx);
    state->pending_path = filelist[0];
    state->has_pending.store(true);
}

// Consume a pending dialog result path, if any.
std::optional<std::string> takeResult(FolderState& state) {
    if (!state.has_pending.load())
        return std::nullopt;
    std::lock_guard lock(state.mtx);
    state.has_pending.store(false);
    return std::move(state.pending_path);
}

// ── Deterministic model <-> editor id mapping (Risk 3) ──────────────────────
// Editor pin id packs (node id, side, pin index) so we never keep a side map
// and integer ids never leak into the serialized format (JSON uses pin names).
// Layout: [ node id << 8 | side bit (0x80) | pin index (0x7F) ].
constexpr std::uintptr_t encodePin(NodeId node, bool is_output, std::size_t index) {
    return (static_cast<std::uintptr_t>(node) << 8) | (is_output ? 0x80u : 0u) |
           (static_cast<std::uintptr_t>(index) & 0x7Fu);
}

struct DecodedPin {
    NodeId node;
    bool is_output;
    std::size_t index;
};

constexpr DecodedPin decodePin(std::uintptr_t pid) {
    return {static_cast<NodeId>(pid >> 8), (pid & 0x80u) != 0,
            static_cast<std::size_t>(pid & 0x7Fu)};
}

std::optional<std::uintptr_t> pinIdForRef(const NodeGraph& graph, const PinRef& ref,
                                          bool is_output) {
    const Node* n = graph.node(ref.node);
    if (!n)
        return std::nullopt;
    const auto pins = is_output ? n->outputs() : n->inputs();
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (pins[i].name == ref.pin)
            return encodePin(ref.node, is_output, i);
    }
    return std::nullopt;
}

const char* nodeTitle(const Node* n) {
    if (const auto* d = pipeline::NodeRegistry::instance().find(n->typeId()))
        return i18n::tr(d->display_name.c_str());
    return n->typeId().c_str();
}

// ── Colors ──────────────────────────────────────────────────────────────────
// Nodes are tinted by category; pins and links by their data type.
ImVec4 categoryColor(NodeCategory c) {
    switch (c) {
    case NodeCategory::Input: return ImVec4(0.30f, 0.66f, 0.38f, 1.0f);     // green
    case NodeCategory::Constant: return ImVec4(0.62f, 0.42f, 0.78f, 1.0f);  // violet
    case NodeCategory::Operation: return ImVec4(0.29f, 0.51f, 0.82f, 1.0f); // blue
    case NodeCategory::Output: return ImVec4(0.85f, 0.55f, 0.26f, 1.0f);    // amber
    case NodeCategory::Control: return ImVec4(0.82f, 0.40f, 0.52f, 1.0f);   // rose
    case NodeCategory::Frame: return ImVec4(0.45f, 0.48f, 0.55f, 1.0f);     // slate
    }
    return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
}

ImVec4 pinTypeColor(PinType t) {
    switch (t) {
    case PinType::RGBA: return ImVec4(0.86f, 0.47f, 0.47f, 1.0f);   // red
    case PinType::RGB: return ImVec4(0.80f, 0.58f, 0.44f, 1.0f);    // salmon
    case PinType::R: return ImVec4(0.74f, 0.74f, 0.74f, 1.0f);      // grey
    case PinType::Int: return ImVec4(0.47f, 0.80f, 0.55f, 1.0f);    // green
    case PinType::Real: return ImVec4(0.47f, 0.71f, 0.86f, 1.0f);   // blue
    case PinType::Bool: return ImVec4(0.82f, 0.78f, 0.47f, 1.0f);   // yellow
    case PinType::String: return ImVec4(0.75f, 0.55f, 0.80f, 1.0f); // purple
    case PinType::Number: return ImVec4(0.55f, 0.82f, 0.80f, 1.0f); // teal
    case PinType::Any: return ImVec4(0.90f, 0.90f, 0.92f, 1.0f);    // near-white
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

// Draw a pin connector icon and reserve its layout box.  Inputs get a filled
// circle, outputs a right-pointing arrow — both outlined and tinted by type.
void drawPinIcon(bool is_input, ImU32 color, bool connected, float size = 14.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r = size * 0.34f;
    const ImU32 outline = IM_COL32(12, 12, 16, 235);
    if (is_input) {
        if (connected)
            dl->AddCircleFilled(c, r, color, 20);
        else {
            dl->AddCircleFilled(c, r, IM_COL32(35, 38, 44, 255), 20);
            dl->AddCircle(c, r, color, 20, 2.0f);
        }
        dl->AddCircle(c, r, outline, 20, 1.0f);
    } else {
        const ImVec2 a(c.x - r, c.y - r), b(c.x - r, c.y + r), tip(c.x + r * 1.15f, c.y);
        if (connected)
            dl->AddTriangleFilled(a, b, tip, color);
        else {
            dl->AddTriangleFilled(a, b, tip, IM_COL32(35, 38, 44, 255));
            dl->AddTriangle(a, b, tip, color, 2.0f);
        }
        dl->AddTriangle(a, b, tip, outline, 1.0f);
    }
}

// Absolute path of the bundled presets folder (copied next to the exe on
// build).  Resource input nodes store paths relative to this.
std::filesystem::path presetsRoot() {
    if (const char* base = SDL_GetBasePath())
        return std::filesystem::path(base) / "presets";
    return std::filesystem::path("presets");
}

// Popup body: list recognized image files under resources/presets; selecting
// one writes its path (relative to the presets root) into @p path_value.
void drawPresetPicker(std::string& path_value) {
    namespace fs = std::filesystem;
    const fs::path root = presetsRoot();
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.no_folder"));
        return;
    }

    ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.pick"));
    ImGui::BeginChild("##presetlist", ImVec2(240.0f, 200.0f), ImGuiChildFlags_Borders);
    bool any = false;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        const std::string ext = to_lower(it->path().extension().string());
        if (whiteout::textures::classifyExtension(ext) ==
            whiteout::textures::TextureFileFormat::Unknown)
            continue;
        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (rel.empty())
            continue;
        any = true;
        if (ImGui::Selectable(rel.c_str(), rel == path_value)) {
            path_value = rel;
            ImGui::CloseCurrentPopup();
        }
    }
    if (!any)
        ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.empty"));
    ImGui::EndChild();
}

} // namespace

PipelineEditor::PipelineEditor() {
    ed::Config config;
    // We own graph persistence (model -> JSON); disable the editor's own
    // settings file so node positions aren't written/read twice.
    config.SettingsFile = nullptr;
    context_ = ed::CreateEditor(&config);
}

PipelineEditor::~PipelineEditor() {
    if (context_) {
        ed::DestroyEditor(context_);
        context_ = nullptr;
    }
}

void PipelineEditor::seedDemo() {
    Node* in = graph_.createNode("input.standard");
    Node* invert = graph_.createNode("op.invert_channel");
    Node* out = graph_.createNode("output.standard");
    if (!in || !invert || !out)
        return;
    in->setPosition({40.0f, 60.0f});
    invert->setPosition({300.0f, 60.0f});
    out->setPosition({560.0f, 60.0f});
    graph_.addLink({in->id(), "image"}, {invert->id(), "image"});
    graph_.addLink({invert->id(), "image"}, {out->id(), "image"});
}

pipeline::NodeId PipelineEditor::spawnNodeAt(const char* type_id, pipeline::Vec2 pos) {
    Node* n = graph_.createNode(type_id);
    if (!n)
        return 0;
    n->setPosition(pos);
    return n->id();
}

pipeline::NodeId PipelineEditor::spawnNode(const char* type_id) {
    const NodeId id = spawnNodeAt(type_id, next_spawn_pos_);
    if (id) {
        // Cascade so repeated spawns don't stack exactly on top of each other.
        next_spawn_pos_.x += 28.0f;
        next_spawn_pos_.y += 28.0f;
    }
    return id;
}

void PipelineEditor::requestSave(SDL_Window* window) {
    SDL_ShowSaveFileDialog(pipelineDialogCallback, &save_dialog_result_, window, kPipelineFilters,
                           static_cast<int>(sizeof(kPipelineFilters) / sizeof(kPipelineFilters[0])), "pipeline.json");
}

void PipelineEditor::requestLoad(SDL_Window* window) {
    SDL_ShowOpenFileDialog(pipelineDialogCallback, &load_dialog_result_, window, kPipelineFilters,
                           static_cast<int>(sizeof(kPipelineFilters) / sizeof(kPipelineFilters[0])), nullptr,
                           /*allow_many=*/false);
}

void PipelineEditor::processDialogs() {
    if (auto path = takeResult(save_dialog_result_))
        savePipeline(*path);
    if (auto path = takeResult(load_dialog_result_))
        loadPipeline(*path);
}

void PipelineEditor::savePipeline(const std::string& path) {
    // Ensure a .json extension (the dialog filter doesn't always append it).
    std::filesystem::path out(path);
    if (to_lower(out.extension().string()) != ".json")
        out += ".json";

    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f)
        return;
    f << pipeline::toJson(graph_).dump(2);
}

bool PipelineEditor::loadPipeline(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    std::vector<std::string> warnings; // (surfaced via a status line in a later pass)
    if (!pipeline::fromJson(doc, graph_, &warnings))
        return false;

    // The loaded graph has fresh node ids; clear placement tracking so each
    // node's saved position is pushed back into the editor, and keep the demo
    // seed from re-running over the loaded graph.
    placed_.clear();
    seeded_ = true;
    return true;
}

void PipelineEditor::draw(SDL_Window* window) {
    if (!seeded_) {
        seedDemo();
        seeded_ = true;
    }

    // Palette (with Save/Load toolbar) on the left, node canvas filling the rest.
    constexpr f32 kPaletteWidth = 190.0f;
    drawPalette(kPaletteWidth, window);
    ImGui::SameLine();

    // Remember the canvas rect so we can make it a drag-and-drop target.
    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_avail = ImGui::GetContentRegionAvail();

    ed::SetCurrentEditor(context_);
    ed::Begin("##PipelineCanvas", ImVec2(0.0f, 0.0f));

    drawNodes();
    drawLinks();
    handleCreate();
    handleDelete();

    // Tooltip describing the node currently under the cursor.
    if (const ed::NodeId hov = ed::GetHoveredNode()) {
        if (const Node* n = graph_.node(static_cast<pipeline::NodeId>(hov.Get()))) {
            const char* desc = nullptr;
            if (const auto* d = pipeline::NodeRegistry::instance().find(n->typeId()))
                desc = i18n::tr((d->display_name + ".desc").c_str());
            if (desc) {
                ed::Suspend();
                ImGui::SetTooltip("%s", desc);
                ed::Resume();
            }
        }
    }

    ed::End();

    // Drag a palette template onto the canvas.  We don't use ImGui's drag-drop
    // payloads (their target hit-testing is unreliable over the node-editor's
    // own child windows); instead drag_type_ is captured on press in the palette
    // and we spawn here on release if the cursor is over the canvas.  Done while
    // the editor is still current so ScreenToCanvas maps the drop into graph space.
    if (!drag_type_.empty()) {
        const ImRect canvas_rect(
            canvas_min, ImVec2(canvas_min.x + canvas_avail.x, canvas_min.y + canvas_avail.y));
        const ImVec2 mouse = ImGui::GetMousePos();
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Follow the cursor with a small preview of the node being placed.
            if (canvas_rect.Contains(mouse))
                ImGui::SetTooltip("%s", drag_label_.c_str());
        } else { // released — drop it if over the canvas, then end the drag
            if (canvas_rect.Contains(mouse)) {
                const ImVec2 c = ed::ScreenToCanvas(mouse);
                spawnNodeAt(drag_type_.c_str(), {c.x, c.y});
            }
            drag_type_.clear();
            drag_label_.clear();
        }
    }

    syncPositions();
    ed::SetCurrentEditor(nullptr);

    // Apply any save/load chosen via the file dialogs since last frame.
    processDialogs();
}

void PipelineEditor::drawPalette(f32 width, SDL_Window* window) {
    ImGui::BeginChild("##PipelinePalette", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);

    // Pipeline name (shown in pickers instead of the file name).
    ImGui::TextUnformatted(i18n::tr("pipeline.name"));
    {
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", graph_.name().c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##pipelinename", name_buf, sizeof(name_buf)))
            graph_.setName(name_buf);
    }
    ImGui::Spacing();

    // Pipeline type: a Function is an explicit choice; otherwise the type is
    // derived from the graph — Standard (one Standard Input + one Standard
    // Output) or Varying (anything else).
    bool is_function = graph_.pipelineType() == pipeline::PipelineType::Function;
    ImGui::Checkbox(i18n::tr("pipeline.function"), &is_function);
    graph_.setPipelineType(is_function ? pipeline::PipelineType::Function
                                       : graph_.nonFunctionType());
    {
        const char* tkey =
            graph_.pipelineType() == pipeline::PipelineType::Function  ? "pipeline.type.function"
            : graph_.pipelineType() == pipeline::PipelineType::Varying ? "pipeline.type.varying"
                                                                       : "pipeline.type.standard";
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", i18n::tr(tkey));
    }
    ImGui::Spacing();

    // Save/Load toolbar over the palette.  Two equal-width buttons on one row.
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 btn_w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(i18n::tr("pipeline.save"), ImVec2(btn_w, 0.0f)))
        requestSave(window);
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("pipeline.load"), ImVec2(btn_w, 0.0f)))
        requestLoad(window);
    // Reload the pipeline catalog from disk (new/edited files, subpipelines).
    if (ImGui::Button(i18n::tr("pipeline.refresh"), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
        refresh_requested_ = true;
    ImGui::Spacing();

    ImGui::SeparatorText(i18n::tr("pipeline.palette"));
    ImGui::TextDisabled("%s", i18n::tr("pipeline.palette_hint"));
    ImGui::Spacing();

    struct Section {
        NodeCategory category;
        const char* label_key;
    };
    static constexpr Section kSections[] = {
        {NodeCategory::Input, "pipeline.category.input"},
        {NodeCategory::Constant, "pipeline.category.constants"},
        {NodeCategory::Operation, "pipeline.category.operation"},
        {NodeCategory::Control, "pipeline.category.control"},
        {NodeCategory::Frame, "pipeline.category.frame"},
        {NodeCategory::Output, "pipeline.category.output"},
    };

    // Operations have grown large, so they are split into navigable sub-groups
    // (a palette-only concern; the registry stays flat).  Any operation not
    // listed below falls into "Other" so nothing can silently disappear.
    struct OpGroup {
        const char* label_key;
        std::vector<const char*> types;
    };
    static const std::vector<OpGroup> kOpGroups = {
        {"pipeline.group.channels",
         {"op.extract_channel", "op.invert_channel", "op.fill_channel", "op.invert",
          "op.merge_channels", "op.prims", "op.luma"}},
        {"pipeline.group.arithmetic",
         {"op.add", "op.multiply", "op.min", "op.max", "op.negate", "op.sqrt", "op.reciprocal"}},
        {"pipeline.group.bitwise",
         {"op.bit_and", "op.bit_or", "op.bit_xor", "op.bit_not", "op.bit_shl", "op.bit_shr"}},
        {"pipeline.group.filters",
         {"op.gaussian_blur", "op.sharpen", "op.sobel", "op.derivatives", "op.blend"}},
        {"pipeline.group.geometry", {"op.mirror", "op.rotate", "op.scale_to", "op.scale_by"}},
        {"pipeline.group.mipmaps",
         {"op.matching_mipmap", "op.extract_mipmap", "op.regenerate_mipmaps", "op.downscale",
          "op.upscale"}},
        {"pipeline.group.image", {"op.image_properties", "op.set_kind"}},
    };

    // Render one draggable palette entry for a node descriptor.
    const auto renderEntry = [&](const pipeline::NodeDescriptor& d) {
        const char* label = i18n::tr(d.display_name.c_str());
        // ##type_id keeps the id stable even if two types share a label.
        const std::string item = std::string(label) + "##" + d.type_id;
        // Drag a template onto the canvas to create it where released.  Capturing
        // on press makes ImGui own the active id, so the node editor won't start a
        // selection box mid-drag.  (A plain click over the palette does nothing.)
        ImGui::Selectable(item.c_str());
        if (ImGui::IsItemActivated()) {
            drag_type_ = d.type_id;
            drag_label_ = label;
        }
        // Hover tooltip: a short description of what the node does.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            const char* tip = i18n::tr((d.display_name + ".desc").c_str());
            ImGui::SetTooltip("%s", tip);
        }
    };

    for (const auto& section : kSections) {
        ImGui::PushStyleColor(ImGuiCol_Text, categoryColor(section.category));
        ImGui::SeparatorText(i18n::tr(section.label_key));
        ImGui::PopStyleColor();

        auto& registry = pipeline::NodeRegistry::instance();
        if (section.category != NodeCategory::Operation) {
            for (const auto& d : registry.all())
                if (d.category == section.category)
                    renderEntry(d);
            continue;
        }

        // Operations: render each sub-group as a collapsible header.
        std::unordered_set<std::string> shown;
        const auto drawGroup = [&](const char* label_key, const auto& types) {
            // Skip empty groups (e.g. Upscale absent in builds without it).
            bool any = false;
            for (const char* t : types)
                if (registry.find(t)) {
                    any = true;
                    break;
                }
            if (!any)
                return;
            if (ImGui::CollapsingHeader(i18n::tr(label_key), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();
                for (const char* t : types)
                    if (const auto* d = registry.find(t)) {
                        renderEntry(*d);
                        shown.insert(t);
                    }
                ImGui::Unindent();
            } else {
                for (const char* t : types)
                    shown.insert(t); // collapsed, but counted as handled
            }
        };
        for (const auto& g : kOpGroups)
            drawGroup(g.label_key, g.types);

        // Any operation not assigned to a group above lands in "Other".
        std::vector<const pipeline::NodeDescriptor*> leftover;
        for (const auto& d : registry.all())
            if (d.category == NodeCategory::Operation && !shown.contains(d.type_id))
                leftover.push_back(&d);
        if (!leftover.empty() &&
            ImGui::CollapsingHeader(i18n::tr("pipeline.group.other"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            for (const auto* d : leftover)
                renderEntry(*d);
            ImGui::Unindent();
        }
    }

    ImGui::EndChild();
}

// Rebuild @p node's pins to @p target only when they actually differ, then prune
// dangling links.  Safe to call every frame (a no-op when unchanged).
static void applyInterfaceIfChanged(pipeline::Node& node, const pipeline::PipelineInterface& target,
                                    pipeline::NodeGraph& graph) {
    const auto same = [](auto pins, const std::vector<pipeline::PipelinePort>& ports) {
        if (pins.size() != ports.size())
            return false;
        for (std::size_t i = 0; i < ports.size(); ++i)
            if (pins[i].name != ports[i].name || pins[i].type != ports[i].type)
                return false;
        return true;
    };
    if (same(node.inputs(), target.inputs) && same(node.outputs(), target.outputs))
        return;
    pipeline::applyPipelineInterface(node, target);
    graph.pruneInvalidLinks();
}

void PipelineEditor::syncSubpipelinePins(pipeline::Node& node) {
    using pipeline::Param;
    // The selected pipeline's file name lives in the node's "pipeline" param.
    std::string file;
    for (const Param& p : node.params())
        if (p.name == "pipeline" && std::holds_alternative<std::string>(p.value))
            file = std::get<std::string>(p.value);

    pipeline::PipelineInterface target{{{"image", pipeline::PinType::RGBA}},
                                       {{"image", pipeline::PinType::RGBA}}};
    if (const auto it = pipeline_interfaces_.find(file); it != pipeline_interfaces_.end())
        target = it->second;
    applyInterfaceIfChanged(node, target, graph_);
}

void PipelineEditor::syncLocalCallPins(pipeline::Node& node) {
    using pipeline::Param;
    std::string frame_name;
    for (const Param& p : node.params())
        if (p.name == "frame" && std::holds_alternative<std::string>(p.value))
            frame_name = std::get<std::string>(p.value);

    // Compute the target interface: the named frame's live local interface, or
    // the default single image pair when no such frame exists.
    pipeline::PipelineInterface target{{{"image", pipeline::PinType::RGBA}},
                                       {{"image", pipeline::PinType::RGBA}}};
    for (const auto& up : graph_.nodes())
        if (up->typeId() == "frame.local") {
            std::string nm;
            for (const Param& p : up->params())
                if (p.name == "name" && std::holds_alternative<std::string>(p.value))
                    nm = std::get<std::string>(p.value);
            if (nm == frame_name) {
                target = graph_.localInterface(*up);
                break;
            }
        }

    applyInterfaceIfChanged(node, target, graph_);
}

void PipelineEditor::drawFrameNode(pipeline::Node* n) {
    using pipeline::Param;
    const ed::NodeId nid(n->id());
    if (!placed_.contains(n->id())) {
        ed::SetNodePosition(nid, ImVec2(n->position().x, n->position().y));
        placed_.insert(n->id());
    }

    float w = 360.0f, h = 220.0f;
    Param* name_p = nullptr;
    Param* w_p = nullptr;
    Param* h_p = nullptr;
    for (Param& p : n->params()) {
        if (p.name == "w" && std::holds_alternative<f64>(p.value)) {
            w = static_cast<float>(std::get<f64>(p.value));
            w_p = &p;
        } else if (p.name == "h" && std::holds_alternative<f64>(p.value)) {
            h = static_cast<float>(std::get<f64>(p.value));
            h_p = &p;
        } else if (p.name == "name" && std::holds_alternative<std::string>(p.value)) {
            name_p = &p;
        }
    }
    const ImVec4 cat = categoryColor(NodeCategory::Frame);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(cat.x, cat.y, cat.z, 0.10f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(cat.x, cat.y, cat.z, 0.85f));
    ed::BeginNode(nid);
    // Editable frame name at the top-left, then the resizable group below it.
    if (name_p) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", std::get<std::string>(name_p->value).c_str());
        ImGui::SetNextItemWidth(std::min(180.0f, std::max(90.0f, w - 12.0f)));
        if (ImGui::InputText(("##fn_" + std::to_string(n->id())).c_str(), buf, sizeof(buf)))
            name_p->value = std::string(buf);
    }
    ed::Group(ImVec2(w, h));
    const ImVec2 group_size = ImGui::GetItemRectSize(); // actual size incl. user resize
    ed::EndNode();
    ed::PopStyleColor(2);

    // Persist the (possibly resized) group size directly — no node-padding math.
    if (w_p)
        w_p->value = static_cast<f64>(std::max(60.0f, group_size.x));
    if (h_p)
        h_p->value = static_cast<f64>(std::max(48.0f, group_size.y));
}

void PipelineEditor::drawNodes() {
    // Frames first so they render behind the data nodes they contain.
    for (const auto& up : graph_.nodes())
        if (up->typeId() == "frame.local")
            drawFrameNode(up.get());

    for (const auto& up : graph_.nodes()) {
        Node* n = up.get();
        if (n->typeId() == "frame.local")
            continue; // already drawn as a group container
        const ed::NodeId nid(n->id());
        const ImVec4 cat = categoryColor(n->category());

        // Keep Subpipeline / Local-Call pins mirroring their target's live
        // interface (so corrected interfaces propagate without re-selecting).
        if (n->typeId() == "op.subpipeline")
            syncSubpipelinePins(*n);
        else if (n->typeId() == "local.call")
            syncLocalCallPins(*n);

        // Push the model position into the editor once; afterwards the user
        // drags freely and syncPositions() reads the result back.
        if (!placed_.contains(n->id())) {
            ed::SetNodePosition(nid, ImVec2(n->position().x, n->position().y));
            placed_.insert(n->id());
        }

        ed::PushStyleColor(ed::StyleColor_NodeBorder, cat);
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.13f, 0.14f, 0.16f, 0.94f));
        ed::BeginNode(nid);
        ImGui::BeginGroup(); // whole node content (measured to size the header bar)

        // 1) Title bar — light text; the category-coloured header background is
        //    painted behind it on the node draw list after EndNode (below).
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
        ImGui::TextUnformatted(nodeTitle(n));
        ImGui::PopStyleColor();
        const ImVec2 header_min = ImGui::GetItemRectMin();
        const ImVec2 header_max = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, 3.0f));

        // 2) Pins — inputs flush-left (circle), outputs flush-right (arrow).
        //    The pin row is stretched to the node's content width (driven by the
        //    widest of title/options) so the output column reaches the right
        //    border and the input column sits on the left border.
        constexpr float kIcon = 14.0f;
        constexpr float kGap = 6.0f;
        const ImVec4 kPinLabel(0.85f, 0.86f, 0.88f, 1.0f);
        const auto inputConnected = [&](const std::string& pin) {
            return graph_.isInputConnected({n->id(), pin});
        };
        const auto outputConnected = [&](const std::string& pin) {
            for (const auto& l : graph_.links())
                if (l.from.node == n->id() && l.from.pin == pin)
                    return true;
            return false;
        };

        const auto inputs = n->inputs();
        const auto outputs = n->outputs();

        // Output column width (widest label + gap + icon).
        float out_col_w = 0.0f;
        for (const auto& o : outputs)
            out_col_w = std::max(out_col_w, ImGui::CalcTextSize(o.name.c_str()).x + kGap + kIcon);

        // Target content width: title vs. the (estimated) option-row widths, so
        // the pin row can be padded to span the whole node.
        float content_w = header_max.x - header_min.x;
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            for (const auto& p : n->params()) {
                if (!paramVisible(n, p))
                    continue;
                float w = ImGui::CalcTextSize(i18n::tr(("pipeline.param." + p.name).c_str())).x +
                          ImGui::CalcTextSize(":").x + st.ItemSpacing.x;
                switch (p.widget) {
                case ParamWidget::Enum: w += 110.0f; break;
                case ParamWidget::Model:
                case ParamWidget::Pipeline:
                case ParamWidget::LocalFrame: w += 150.0f; break;
                case ParamWidget::ResourcePath: w += 150.0f + kGap + 28.0f; break;
                case ParamWidget::Scalar:
                    w += std::holds_alternative<std::string>(p.value) ? 144.0f : 94.0f;
                    break;
                }
                content_w = std::max(content_w, w);
            }
        }

        ImGui::BeginGroup(); // input pins (left): [icon] label
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            ed::BeginPin(ed::PinId(encodePin(n->id(), false, i)), ed::PinKind::Input);
            ed::PinPivotAlignment(ImVec2(0.0f, 0.5f)); // link attaches at the icon
            ed::PinPivotSize(ImVec2(0.0f, 0.0f));
            drawPinIcon(true, ImColor(pinTypeColor(inputs[i].type)),
                        inputConnected(inputs[i].name), kIcon);
            ImGui::SameLine(0.0f, kGap);
            ImGui::TextColored(kPinLabel, "%s", inputs[i].name.c_str());
            ed::EndPin();
        }
        if (inputs.empty())
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::EndGroup();
        const float in_col_w = ImGui::GetItemRectSize().x;

        if (!outputs.empty()) {
            // Spacer pushes the output column to the right border.
            const float spacer = std::max(26.0f, content_w - in_col_w - out_col_w);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::Dummy(ImVec2(spacer, 1.0f));
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::BeginGroup(); // output pins: label [icon], right-aligned
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                ed::BeginPin(ed::PinId(encodePin(n->id(), true, i)), ed::PinKind::Output);
                ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
                ed::PinPivotSize(ImVec2(0.0f, 0.0f));
                const float row_w = ImGui::CalcTextSize(outputs[i].name.c_str()).x + kGap + kIcon;
                if (row_w < out_col_w) {
                    ImGui::Dummy(ImVec2(out_col_w - row_w, 1.0f));
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ImGui::TextColored(kPinLabel, "%s", outputs[i].name.c_str());
                ImGui::SameLine(0.0f, kGap);
                drawPinIcon(false, ImColor(pinTypeColor(outputs[i].type)),
                            outputConnected(outputs[i].name), kIcon);
                ed::EndPin();
            }
            ImGui::EndGroup();
        }

        // Editable parameters inside the node body.  Popup-based widgets (enum /
        // model combobox, resource picker) only draw a trigger here; the popup
        // itself is opened after EndNode under Suspend/Resume (the node body is
        // in canvas space; popups must be placed in screen space).
        const auto params = n->params();
        std::vector<std::size_t> open_combo;    // enum params clicked this frame
        std::vector<std::size_t> open_model;    // model params clicked this frame
        std::vector<std::size_t> open_pipeline;   // pipeline params clicked this frame
        std::vector<std::size_t> open_localframe; // local-frame params clicked this frame
        std::vector<std::size_t> open_picker;     // resource params clicked this frame

        // 3) Options section — separated from the pins by a rule + faint panel.
        bool has_options = false;
        for (const auto& p : params)
            if (paramVisible(n, p)) {
                has_options = true;
                break;
            }
        float options_top = 0.0f;
        if (has_options) {
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            options_top = ImGui::GetCursorScreenPos().y;
        }

        for (std::size_t pi = 0; pi < params.size(); ++pi) {
            Param& p = params[pi];
            if (!paramVisible(n, p))
                continue;
            const std::string sfx = std::to_string(n->id()) + "_" + p.name;
            const char* plabel = i18n::tr(("pipeline.param." + p.name).c_str());

            switch (p.widget) {
            case ParamWidget::Enum: {
                if (!std::holds_alternative<i64>(p.value))
                    break;
                i64& idx = std::get<i64>(p.value);
                if (idx < 0 || idx >= static_cast<i64>(p.enum_labels.size()))
                    idx = 0;
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                const std::string btn =
                    std::string(i18n::tr(p.enum_labels[idx].c_str())) + "##pbtn_" + sfx;
                if (ImGui::Button(btn.c_str(), ImVec2(110.0f, 0.0f)))
                    open_combo.push_back(pi);
                break;
            }
            case ParamWidget::Model: {
                if (!std::holds_alternative<std::string>(p.value))
                    break;
                const std::string& cur = std::get<std::string>(p.value);
                const char* shown =
                    upscaler_models_.empty() ? i18n::tr("pipeline.resource.no_model") : "-";
                for (const auto& m : upscaler_models_)
                    if (m.id == cur) {
                        shown = m.label.c_str();
                        break;
                    }
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                const std::string btn = std::string(shown) + "##mbtn_" + sfx;
                if (ImGui::Button(btn.c_str(), ImVec2(150.0f, 0.0f)))
                    open_model.push_back(pi);
                break;
            }
            case ParamWidget::Pipeline: {
                if (!std::holds_alternative<std::string>(p.value))
                    break;
                const std::string& cur = std::get<std::string>(p.value);
                std::string shown = cur.empty() ? std::string("-") : cur;
                for (const auto& info : pipelines_)
                    if (info.file == cur) {
                        shown = info.display_name;
                        break;
                    }
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                const std::string btn = shown + "##plbtn_" + sfx;
                if (ImGui::Button(btn.c_str(), ImVec2(150.0f, 0.0f)))
                    open_pipeline.push_back(pi);
                break;
            }
            case ParamWidget::LocalFrame: {
                if (!std::holds_alternative<std::string>(p.value))
                    break;
                const std::string& cur = std::get<std::string>(p.value);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                const std::string btn = (cur.empty() ? std::string("-") : cur) + "##lfbtn_" + sfx;
                if (ImGui::Button(btn.c_str(), ImVec2(150.0f, 0.0f)))
                    open_localframe.push_back(pi);
                break;
            }
            case ParamWidget::ResourcePath: {
                if (!std::holds_alternative<std::string>(p.value))
                    break;
                std::string& path = std::get<std::string>(p.value);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                char buf[260];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::InputText(("##rp_" + sfx).c_str(), buf, sizeof(buf)))
                    path = buf;
                ImGui::SameLine();
                if (ImGui::Button(("...##pick_" + sfx).c_str()))
                    open_picker.push_back(pi);
                break;
            }
            case ParamWidget::Scalar: {
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                if (std::holds_alternative<i64>(p.value)) {
                    int v = static_cast<int>(std::get<i64>(p.value));
                    if (ImGui::InputInt(("##si_" + sfx).c_str(), &v)) {
                        if (v < 0)
                            v = 0;
                        std::get<i64>(p.value) = v;
                    }
                } else if (std::holds_alternative<f64>(p.value)) {
                    float v = static_cast<float>(std::get<f64>(p.value));
                    if (ImGui::InputFloat(("##sf_" + sfx).c_str(), &v))
                        std::get<f64>(p.value) = v;
                } else if (std::holds_alternative<bool>(p.value)) {
                    bool v = std::get<bool>(p.value);
                    if (ImGui::Checkbox(("##sb_" + sfx).c_str(), &v))
                        std::get<bool>(p.value) = v;
                } else if (std::holds_alternative<std::string>(p.value)) {
                    std::string& s = std::get<std::string>(p.value);
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::InputText(("##ss_" + sfx).c_str(), buf, sizeof(buf)))
                        s = buf;
                }
                break;
            }
            }
        }

        ImGui::EndGroup(); // whole node content
        const ImVec2 content_min = ImGui::GetItemRectMin();
        const ImVec2 content_max = ImGui::GetItemRectMax();

        ed::EndNode();
        ed::PopStyleColor(2);

        // Paint the 3-part decoration on the node's background draw list:
        // a category-coloured title bar, a rule under it, and a faint options
        // panel.  Drawn after EndNode so the node rect is final; it sits over
        // the node background but under the (already-drawn) content/text.
        if (ImDrawList* dl = ed::GetNodeBackgroundDrawList(nid)) {
            const ImVec4 pad = ed::GetStyle().NodePadding;
            const float rounding = ed::GetStyle().NodeRounding;
            const ImVec2 bmin(content_min.x - pad.x, content_min.y - pad.y);
            const ImVec2 bmax(content_max.x + pad.z, content_max.y + pad.w);
            const float header_bottom = header_max.y + 2.0f;

            dl->AddRectFilled(bmin, ImVec2(bmax.x, header_bottom), ImColor(cat), rounding,
                              ImDrawFlags_RoundCornersTop);
            dl->AddLine(ImVec2(bmin.x, header_bottom), ImVec2(bmax.x, header_bottom),
                        ImColor(0, 0, 0, 90), 1.0f);
            if (has_options) {
                const float oy = options_top - 2.0f;
                dl->AddRectFilled(ImVec2(bmin.x, oy), bmax, ImColor(255, 255, 255, 14), rounding,
                                  ImDrawFlags_RoundCornersBottom);
                dl->AddLine(ImVec2(bmin.x, oy), ImVec2(bmax.x, oy), ImColor(255, 255, 255, 38),
                            1.0f);
            }
        }

        // Deferred popups for this node's params (combobox dropdowns + resource
        // pickers), placed in screen space via Suspend/Resume.
        if (!params.empty()) {
            const auto clicked = [](const std::vector<std::size_t>& v, std::size_t i) {
                return std::find(v.begin(), v.end(), i) != v.end();
            };
            ed::Suspend();
            for (std::size_t pi = 0; pi < params.size(); ++pi) {
                Param& p = params[pi];
                const std::string sfx = std::to_string(n->id()) + "_" + p.name;

                if (p.widget == ParamWidget::Enum && std::holds_alternative<i64>(p.value)) {
                    const std::string pop = "##pp_" + sfx;
                    if (clicked(open_combo, pi))
                        ImGui::OpenPopup(pop.c_str());
                    if (ImGui::BeginPopup(pop.c_str())) {
                        i64& idx = std::get<i64>(p.value);
                        for (std::size_t oi = 0; oi < p.enum_labels.size(); ++oi) {
                            if (ImGui::Selectable(i18n::tr(p.enum_labels[oi].c_str()),
                                                  static_cast<i64>(oi) == idx))
                                idx = static_cast<i64>(oi);
                        }
                        ImGui::EndPopup();
                    }
                } else if (p.widget == ParamWidget::Model &&
                           std::holds_alternative<std::string>(p.value)) {
                    const std::string pop = "##mp_" + sfx;
                    if (clicked(open_model, pi))
                        ImGui::OpenPopup(pop.c_str());
                    if (ImGui::BeginPopup(pop.c_str())) {
                        std::string& cur = std::get<std::string>(p.value);
                        if (upscaler_models_.empty())
                            ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.no_model"));
                        for (const auto& m : upscaler_models_) {
                            if (ImGui::Selectable(m.label.c_str(), m.id == cur))
                                cur = m.id;
                        }
                        ImGui::EndPopup();
                    }
                } else if (p.widget == ParamWidget::Pipeline &&
                           std::holds_alternative<std::string>(p.value)) {
                    const std::string pop = "##plp_" + sfx;
                    if (clicked(open_pipeline, pi))
                        ImGui::OpenPopup(pop.c_str());
                    if (ImGui::BeginPopup(pop.c_str())) {
                        std::string& cur = std::get<std::string>(p.value);
                        if (pipelines_.empty())
                            ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.no_pipeline"));
                        for (const auto& info : pipelines_) {
                            if (ImGui::Selectable(info.display_name.c_str(), info.file == cur) &&
                                info.file != cur) {
                                cur = info.file;
                                // Reshape this node's pins to the picked pipeline.
                                syncSubpipelinePins(*n);
                            }
                        }
                        ImGui::EndPopup();
                    }
                } else if (p.widget == ParamWidget::LocalFrame &&
                           std::holds_alternative<std::string>(p.value)) {
                    const std::string pop = "##lfp_" + sfx;
                    if (clicked(open_localframe, pi))
                        ImGui::OpenPopup(pop.c_str());
                    if (ImGui::BeginPopup(pop.c_str())) {
                        std::string& cur = std::get<std::string>(p.value);
                        bool any = false;
                        for (const auto& up : graph_.nodes()) {
                            if (up->typeId() != "frame.local")
                                continue;
                            any = true;
                            std::string nm;
                            for (const auto& fp : up->params())
                                if (fp.name == "name" &&
                                    std::holds_alternative<std::string>(fp.value))
                                    nm = std::get<std::string>(fp.value);
                            if (ImGui::Selectable(nm.c_str(), nm == cur) && nm != cur) {
                                cur = nm;
                                syncLocalCallPins(*n);
                            }
                        }
                        if (!any)
                            ImGui::TextDisabled("%s", i18n::tr("pipeline.resource.no_frame"));
                        ImGui::EndPopup();
                    }
                } else if (p.widget == ParamWidget::ResourcePath &&
                           std::holds_alternative<std::string>(p.value)) {
                    const std::string pop = "##rpp_" + sfx;
                    if (clicked(open_picker, pi))
                        ImGui::OpenPopup(pop.c_str());
                    if (ImGui::BeginPopup(pop.c_str())) {
                        drawPresetPicker(std::get<std::string>(p.value));
                        ImGui::EndPopup();
                    }
                }
            }
            ed::Resume();
        }
    }
}

void PipelineEditor::drawLinks() {
    for (const auto& l : graph_.links()) {
        const auto from = pinIdForRef(graph_, l.from, /*is_output=*/true);
        const auto to = pinIdForRef(graph_, l.to, /*is_output=*/false);
        if (!from || !to)
            continue;

        // Colour the link by the type carried by its source (output) pin.
        ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
        if (const Node* fn = graph_.node(l.from.node)) {
            if (const Pin* p = fn->findPin(l.from.pin, /*is_input=*/false))
                color = pinTypeColor(p->type);
        }
        ed::Link(ed::LinkId(l.id), ed::PinId(*from), ed::PinId(*to), color, 2.0f);
    }
}

void PipelineEditor::handleCreate() {
    if (ed::BeginCreate()) {
        ed::PinId a_id, b_id;
        if (ed::QueryNewLink(&a_id, &b_id) && a_id && b_id) {
            const DecodedPin a = decodePin(a_id.Get());
            const DecodedPin b = decodePin(b_id.Get());

            // Normalise to (output -> input); reject same-kind drags.
            const DecodedPin* out = nullptr;
            const DecodedPin* in = nullptr;
            if (a.is_output && !b.is_output) {
                out = &a;
                in = &b;
            } else if (!a.is_output && b.is_output) {
                out = &b;
                in = &a;
            }

            bool acceptable = false;
            PinRef from_ref, to_ref;
            if (out && in && out->node != in->node) {
                const Node* on = graph_.node(out->node);
                const Node* inn = graph_.node(in->node);
                if (on && inn && out->index < on->outputs().size() &&
                    in->index < inn->inputs().size()) {
                    const Pin& op = on->outputs()[out->index];
                    const Pin& ip = inn->inputs()[in->index];
                    from_ref = {out->node, op.name};
                    to_ref = {in->node, ip.name};
                    // Type compatibility is required; an already-driven input is
                    // allowed — it gets replaced (reconnect), see below.
                    acceptable = pipeline::pinTypesCompatible(op.type, ip.type);
                }
            }

            if (acceptable) {
                if (ed::AcceptNewItem()) {
                    // Reconnect semantics: an input pin holds a single driver,
                    // so drop any existing link feeding it before connecting.
                    for (const auto& l : graph_.links()) {
                        if (l.to.node == to_ref.node && l.to.pin == to_ref.pin) {
                            graph_.removeLink(l.id);
                            break;
                        }
                    }
                    graph_.addLink(from_ref, to_ref);
                }
            } else {
                ed::RejectNewItem();
            }
        }
    }
    ed::EndCreate();
}

void PipelineEditor::handleDelete() {
    if (ed::BeginDelete()) {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            if (ed::AcceptDeletedItem())
                graph_.removeLink(static_cast<pipeline::LinkId>(lid.Get()));
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) {
                const auto id = static_cast<NodeId>(nid.Get());
                graph_.removeNode(id);
                placed_.erase(id);
            }
        }
    }
    ed::EndDelete();
}

void PipelineEditor::syncPositions() {
    // Read editor positions back into the model so drags are captured for save.
    // Skip nodes not yet pushed to the editor (e.g. one just dropped this frame,
    // after drawNodes ran): the editor would report (0,0) and clobber the model
    // position before drawNodes ever places the node at its real spot.
    for (const auto& up : graph_.nodes()) {
        if (!placed_.contains(up->id()))
            continue;
        const ImVec2 p = ed::GetNodePosition(ed::NodeId(up->id()));
        up->setPosition({p.x, p.y});
    }
}

} // namespace whiteout::textool::views
