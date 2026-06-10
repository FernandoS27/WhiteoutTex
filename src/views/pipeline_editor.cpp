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
    if (gate && std::holds_alternative<i64>(gate->value))
        return std::get<i64>(gate->value) == p.visible_when_value;
    return true;
}

// Drag-and-drop payload id carrying a node type-id string (palette -> canvas).
constexpr const char* kNodeDragPayload = "WT_PIPELINE_NODE";

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
    case NodeCategory::Operation: return ImVec4(0.29f, 0.51f, 0.82f, 1.0f); // blue
    case NodeCategory::Output: return ImVec4(0.85f, 0.55f, 0.26f, 1.0f);    // amber
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
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
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

    ed::End();

    // Drop target for palette templates.  Done while the editor is still the
    // current one so ScreenToCanvas maps the drop point into graph space.
    const ImRect canvas_rect(canvas_min,
                             ImVec2(canvas_min.x + canvas_avail.x, canvas_min.y + canvas_avail.y));
    if (ImGui::BeginDragDropTargetCustom(canvas_rect, ImGui::GetID("##PipelineCanvasDrop"))) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kNodeDragPayload)) {
            const char* type_id = static_cast<const char*>(pl->Data);
            const ImVec2 c = ed::ScreenToCanvas(ImGui::GetMousePos());
            spawnNodeAt(type_id, {c.x, c.y});
        }
        ImGui::EndDragDropTarget();
    }

    syncPositions();
    ed::SetCurrentEditor(nullptr);

    // Apply any save/load chosen via the file dialogs since last frame.
    processDialogs();
}

void PipelineEditor::drawPalette(f32 width, SDL_Window* window) {
    ImGui::BeginChild("##PipelinePalette", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);

    // Pipeline type selector (Standard = one std input/output; Varying = many).
    // A plain ImGui combo — it lives in a normal window, so no Suspend/Resume.
    ImGui::TextUnformatted(i18n::tr("pipeline.type"));
    const char* type_items[] = {i18n::tr("pipeline.type.standard"),
                                i18n::tr("pipeline.type.varying")};
    int type_cur = static_cast<int>(graph_.pipelineType());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::Combo("##pipelinetype", &type_cur, type_items,
                     static_cast<int>(sizeof(type_items) / sizeof(type_items[0]))))
        graph_.setPipelineType(static_cast<pipeline::PipelineType>(type_cur));
    ImGui::Spacing();

    // Save/Load toolbar over the palette.  Two equal-width buttons on one row.
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 btn_w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(i18n::tr("pipeline.save"), ImVec2(btn_w, 0.0f)))
        requestSave(window);
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("pipeline.load"), ImVec2(btn_w, 0.0f)))
        requestLoad(window);
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
        {NodeCategory::Operation, "pipeline.category.operation"},
        {NodeCategory::Output, "pipeline.category.output"},
    };

    for (const auto& section : kSections) {
        ImGui::PushStyleColor(ImGuiCol_Text, categoryColor(section.category));
        ImGui::SeparatorText(i18n::tr(section.label_key));
        ImGui::PopStyleColor();

        for (const auto& d : pipeline::NodeRegistry::instance().all()) {
            if (d.category != section.category)
                continue;
            const char* label = i18n::tr(d.display_name.c_str());
            // ##type_id keeps the id stable even if two types share a label.
            const std::string item = std::string(label) + "##" + d.type_id;

            // Click spawns at a cascading position; drag drops at the cursor.
            if (ImGui::Selectable(item.c_str()))
                spawnNode(d.type_id.c_str());
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(kNodeDragPayload, d.type_id.c_str(),
                                          d.type_id.size() + 1);
                ImGui::TextUnformatted(label);
                ImGui::EndDragDropSource();
            }
        }
    }

    ImGui::EndChild();
}

void PipelineEditor::drawNodes() {
    for (const auto& up : graph_.nodes()) {
        Node* n = up.get();
        const ed::NodeId nid(n->id());
        const ImVec4 cat = categoryColor(n->category());

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

        // 2) Pins.
        ImGui::BeginGroup(); // input pins (left)
        const auto inputs = n->inputs();
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            ed::BeginPin(ed::PinId(encodePin(n->id(), false, i)), ed::PinKind::Input);
            ImGui::PushStyleColor(ImGuiCol_Text, pinTypeColor(inputs[i].type));
            ImGui::Text("-> %s", inputs[i].name.c_str());
            ImGui::PopStyleColor();
            ed::EndPin();
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup(); // output pins (right)
        const auto outputs = n->outputs();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            ed::BeginPin(ed::PinId(encodePin(n->id(), true, i)), ed::PinKind::Output);
            ImGui::PushStyleColor(ImGuiCol_Text, pinTypeColor(outputs[i].type));
            ImGui::Text("%s ->", outputs[i].name.c_str());
            ImGui::PopStyleColor();
            ed::EndPin();
        }
        ImGui::EndGroup();

        // Editable parameters inside the node body.  Popup-based widgets (enum /
        // model combobox, resource picker) only draw a trigger here; the popup
        // itself is opened after EndNode under Suspend/Resume (the node body is
        // in canvas space; popups must be placed in screen space).
        const auto params = n->params();
        std::vector<std::size_t> open_combo;    // enum params clicked this frame
        std::vector<std::size_t> open_model;    // model params clicked this frame
        std::vector<std::size_t> open_pipeline; // pipeline params clicked this frame
        std::vector<std::size_t> open_picker;   // resource params clicked this frame

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
                const std::string shown =
                    cur.empty() ? "-" : std::filesystem::path(cur).stem().string();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s:", plabel);
                ImGui::SameLine();
                const std::string btn = shown + "##plbtn_" + sfx;
                if (ImGui::Button(btn.c_str(), ImVec2(150.0f, 0.0f)))
                    open_pipeline.push_back(pi);
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
                }
                // Plain string scalars have no in-node widget yet.
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
                        for (const auto& name : pipelines_) {
                            const std::string label = std::filesystem::path(name).stem().string();
                            if (ImGui::Selectable(label.c_str(), name == cur))
                                cur = name;
                        }
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
    for (const auto& up : graph_.nodes()) {
        const ImVec2 p = ed::GetNodePosition(ed::NodeId(up->id()));
        up->setPosition({p.x, p.y});
    }
}

} // namespace whiteout::textool::views
