// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file node.h
 * @brief Abstract pipeline node.
 *
 * One abstract base + Template Method (evaluate()).  Input / Operation / Output
 * differ only in which pins they declare and what evaluate() does, so the
 * category is metadata (NodeCategory), NOT a class hierarchy.  Per-node editable
 * settings are a flat list of Param descriptors so JSON (de)serialization and
 * the future properties panel are generic — written once, not per node type.
 *
 * ImGui-free: node position is a plain Vec2, not ImVec2.  The editor view
 * converts at the boundary.
 */

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common_types.h"
#include "pipeline/pin.h"

namespace whiteout::textool::pipeline {

using NodeId = u32; ///< Stable per-graph node id (0 = unassigned).

/// Palette grouping.  Input/Constant nodes are sources, Output nodes are sinks,
/// Operations transform, Control nodes route/branch data flow.  (Constant
/// Channel is a Constant that still takes width/height/value inputs, so the
/// "sources have no inputs" rule is loose.)
enum class NodeCategory : u8 { Input, Constant, Operation, Output, Control };

/// A plain 2D point — keeps the model free of ImGui's ImVec2.
struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

/// How a param is presented/edited in the node UI.
enum class ParamWidget : u8 {
    Scalar,       ///< Plain value; int/real/bool render inline (string: none yet).
    Enum,         ///< i64 index into enum_labels -> combobox of those labels.
    ResourcePath, ///< string path under resources/presets -> text field + picker.
    Model,        ///< string AI-model id -> combobox of available upscaler models.
    Pipeline,     ///< string pipeline file name -> combobox of available pipelines.
};

/// One editable node setting.  Reused by the generic serializer and the
/// in-node widgets.  Scalar-only by construction (ParamValue).
struct Param {
    std::string name;
    ParamValue value;
    ParamWidget widget = ParamWidget::Scalar;
    /// Option i18n keys for an Enum param (value is the i64 index into them).
    /// Not serialized — re-declared by the node ctor; only the index round-trips.
    std::vector<std::string> enum_labels;
    /// Optional visibility gate: shown only when the enum param named
    /// `visible_when` has selected index `visible_when_value`.  Empty = always.
    std::string visible_when;
    i64 visible_when_value = 0;
};

/// Node is PURE DATA: it declares pins/params and carries placement, but holds
/// no execution logic.  Running a graph is the PipelineExecutor's job (it lives
/// in services/ and interprets nodes by type id), so the model stays free of
/// app/ImGui/Texture dependencies.
class Node {
public:
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // ── Identity / placement ───────────────────────────────────────────
    NodeId id() const noexcept { return id_; }
    void setId(NodeId id) noexcept { id_ = id; }
    const std::string& typeId() const noexcept { return type_id_; }
    NodeCategory category() const noexcept { return category_; }
    Vec2 position() const noexcept { return pos_; }
    void setPosition(Vec2 p) noexcept { pos_ = p; }

    // ── Pins / params ──────────────────────────────────────────────────
    std::span<const Pin> inputs() const noexcept { return inputs_; }
    std::span<const Pin> outputs() const noexcept { return outputs_; }
    std::span<Param> params() noexcept { return params_; }
    std::span<const Param> params() const noexcept { return params_; }

    const Pin* findPin(std::string_view name, bool is_input) const noexcept;
    Param* findParam(std::string_view name) noexcept;

    // ── Dynamic pins ───────────────────────────────────────────────────
    /// True if this node's pins are not fixed by its type but derived from a
    /// param (e.g. Subpipeline mirroring the selected pipeline's interface).
    /// Such pins are persisted explicitly instead of re-declared by the ctor.
    bool hasDynamicPins() const noexcept { return dynamic_pins_; }
    /// Replace the entire pin set.  Meaningful only for dynamic-pin nodes;
    /// callers must prune now-dangling links afterwards (NodeGraph does this).
    void setPins(std::vector<Pin> inputs, std::vector<Pin> outputs) {
        inputs_ = std::move(inputs);
        outputs_ = std::move(outputs);
    }

protected:
    Node(std::string type_id, NodeCategory category)
        : type_id_(std::move(type_id)), category_(category) {}

    // Declaration helpers for concrete-node constructors.
    void addInput(std::string name, PinType type) {
        inputs_.push_back({std::move(name), type, true});
    }
    void addOutput(std::string name, PinType type) {
        outputs_.push_back({std::move(name), type, false});
    }
    void addParam(std::string name, ParamValue value) {
        params_.push_back({std::move(name), std::move(value), ParamWidget::Scalar, {}, {}, 0});
    }
    /// Declare an enumeration param: an i64 index into @p labels (i18n keys).
    void addEnumParam(std::string name, i64 default_index, std::vector<std::string> labels) {
        params_.push_back(
            {std::move(name), ParamValue{default_index}, ParamWidget::Enum, std::move(labels), {}, 0});
    }
    /// Declare a resource-path param: a string path relative to resources/presets.
    void addResourceParam(std::string name) {
        params_.push_back(
            {std::move(name), ParamValue{std::string{}}, ParamWidget::ResourcePath, {}, {}, 0});
    }
    /// Declare an AI-model param: a string model id chosen from a runtime list.
    void addModelParam(std::string name) {
        params_.push_back(
            {std::move(name), ParamValue{std::string{}}, ParamWidget::Model, {}, {}, 0});
    }
    /// Declare a pipeline param: a string pipeline file name chosen at runtime.
    void addPipelineParam(std::string name) {
        params_.push_back(
            {std::move(name), ParamValue{std::string{}}, ParamWidget::Pipeline, {}, {}, 0});
    }
    /// Gate the most-recently-added param: shown only when the enum param
    /// @p enum_param has selected index @p index.
    void gateLastParam(std::string enum_param, i64 index) {
        if (!params_.empty()) {
            params_.back().visible_when = std::move(enum_param);
            params_.back().visible_when_value = index;
        }
    }
    /// Mark this node's pins as derived-at-runtime (see hasDynamicPins()).
    void markDynamicPins() noexcept { dynamic_pins_ = true; }

private:
    NodeId id_ = 0;
    std::string type_id_;
    NodeCategory category_;
    Vec2 pos_{};
    std::vector<Pin> inputs_;
    std::vector<Pin> outputs_;
    std::vector<Param> params_;
    bool dynamic_pins_ = false;
};

} // namespace whiteout::textool::pipeline
