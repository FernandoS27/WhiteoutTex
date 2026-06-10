// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file builtin_nodes.cpp
 * @brief The initial set of node types, registered into NodeRegistry.
 *
 * Each node subclass declares its pins/params in its constructor; evaluate() is
 * stubbed until the executor stage lands.  Adding a type is one class here plus
 * one registerType() row in registerBuiltinNodes() — the format_registry idiom.
 *
 * These three cover the three categories (Input / Operation / Output) and the
 * three NodeInput sources the design calls for (Storage / File / Resource come
 * as distinct Input node types; Storage is shown here, the rest follow the same
 * shape).
 */

#include <memory>

#include "pipeline/node.h"
#include "pipeline/node_registry.h"

namespace whiteout::textool::pipeline {

namespace {

// The four RGBA channels, as combobox option labels (i18n keys).  Shared by the
// channel-selecting operations; the param's i64 value indexes into this list.
const std::vector<std::string> kChannelLabels = {
    "pipeline.channel.red", "pipeline.channel.green", "pipeline.channel.blue",
    "pipeline.channel.alpha"};

// ── Input: the pipeline's primary/standard input image ─────────────────────
// No source param — it represents whatever image the pipeline is applied to
// (e.g. the currently-loaded texture).
class StandardInputNode final : public Node {
public:
    StandardInputNode() : Node("input.standard", NodeCategory::Input) {
        addOutput("image", PinType::RGBA);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: emit the current working image into `image`.
    }
};

// ── Input: load a preset from the bundled resources folder ─────────────────
class ResourceInputNode final : public Node {
public:
    ResourceInputNode() : Node("input.resource", NodeCategory::Input) {
        addOutput("image", PinType::RGBA);
        addResourceParam("path"); // path relative to resources/presets
    }
    void evaluate(EvalContext&) override {
        // Executor stage: load resources/presets/<path> into `image`.
    }
};

// ── Operation: extract one channel from an RGBA image ──────────────────────
// Input RGBA -> single-channel (R) output carrying the selected channel.
class ExtractChannelNode final : public Node {
public:
    ExtractChannelNode() : Node("op.extract_channel", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("channel", PinType::R);
        addEnumParam("channel", 0, kChannelLabels); // 0=R, 1=G, 2=B, 3=A
    }
    void evaluate(EvalContext&) override {
        // Executor stage: copy the selected channel plane into `channel`.
    }
};

// ── Operation: invert one specified channel of an RGBA image ────────────────
// Input RGBA -> RGBA with the selected channel replaced by (max - value).
class InvertChannelNode final : public Node {
public:
    InvertChannelNode() : Node("op.invert_channel", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("channel", 0, kChannelLabels); // 0=R, 1=G, 2=B, 3=A
    }
    void evaluate(EvalContext&) override {
        // Executor stage: out = in, with out[channel] = 255 - in[channel].
    }
};

// Blend modes (Blizzard / WC3 style), as combobox option labels (i18n keys).
const std::vector<std::string> kBlendModes = {
    "pipeline.blend.transparent", "pipeline.blend.blend", "pipeline.blend.additive",
    "pipeline.blend.modulate", "pipeline.blend.modulate2x"};

// ── Operation: blend two images into one ───────────────────────────────────
// Inputs `a` (bottom) and `b` (top) RGBA -> RGBA combined per the chosen mode.
class BlendNode final : public Node {
public:
    BlendNode() : Node("op.blend", NodeCategory::Operation) {
        addInput("a", PinType::RGBA);
        addInput("b", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("mode", 0, kBlendModes);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: combine `a` and `b` per the selected blend mode.
    }
};

// ── Operation: invert a single-channel plane ───────────────────────────────
// Input R -> R, value replaced by (max - value).  No channel selection: the
// plane *is* the channel, so there's nothing to pick (hence no param/combobox).
class InvertNode final : public Node {
public:
    InvertNode() : Node("op.invert", NodeCategory::Operation) {
        addInput("channel", PinType::R);
        addOutput("channel", PinType::R);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: out = 255 - in.
    }
};

// ── Operation: which base mipmap matches a related image's top mip ─────────
// Inputs `base` and `related` RGBA -> Int mip index.  Returns the index of the
// base mip whose dimensions match the related image's top mip; if the related
// top mip is larger than the base's top mip, returns 0 (the top).
class MatchingMipmapNode final : public Node {
public:
    MatchingMipmapNode() : Node("op.matching_mipmap", NodeCategory::Operation) {
        addInput("base", PinType::RGBA);
        addInput("related", PinType::RGBA);
        addOutput("mipmap", PinType::Int);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: match `related` top mip size against `base`'s mips.
    }
};

// ── Operation: extract a specific mipmap level as an image ─────────────────
// Inputs `image` RGBA and `mipmap` Int -> RGBA of that mip level.
class ExtractMipmapNode final : public Node {
public:
    ExtractMipmapNode() : Node("op.extract_mipmap", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("mipmap", PinType::Int);
        addOutput("image", PinType::RGBA);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: output mip level `mipmap` of `image`.
    }
};

// Downscale factor option labels (i18n keys).
const std::vector<std::string> kDownscaleFactors = {
    "pipeline.scale.half", "pipeline.scale.quarter", "pipeline.scale.eighth"};

// Mipmap-regeneration modes (i18n keys).  Custom (index 1) exposes a count.
const std::vector<std::string> kMipmapModes = {"pipeline.mipmode.current", "pipeline.mipmode.custom",
                                               "pipeline.mipmode.maximum"};

// ── Operation: downscale an image ──────────────────────────────────────────
class DownscaleNode final : public Node {
public:
    DownscaleNode() : Node("op.downscale", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("factor", 0, kDownscaleFactors); // 1/2, 1/4, 1/8
    }
    void evaluate(EvalContext&) override {
        // Executor stage: box/triangle downsample by the selected factor.
    }
};

// ── Operation: upscale an image with an AI model ───────────────────────────
class UpscaleNode final : public Node {
public:
    UpscaleNode() : Node("op.upscale", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addModelParam("model"); // chosen from available Real-ESRGAN models
    }
    void evaluate(EvalContext&) override {
        // Executor stage: run the selected AI model over `image`.
    }
};

// ── Operation: regenerate the mipmap chain ─────────────────────────────────
// Input RGBA -> RGBA.  Mode selects how many levels: Current keeps the source's
// count, Maximum builds down to 1x1, Custom uses the `count` param.
class RegenerateMipmapsNode final : public Node {
public:
    RegenerateMipmapsNode() : Node("op.regenerate_mipmaps", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("mode", 0, kMipmapModes);
        addParam("count", i64{1});  // mip-level count
        gateLastParam("mode", 1);   // only shown in Custom mode
    }
    void evaluate(EvalContext&) override {
        // Executor stage: rebuild mips per mode (count levels when Custom).
    }
};

// ── Output: the pipeline's primary/standard output image ───────────────────
// No destination param — it represents the image the pipeline yields back to
// whatever invoked it (mirrors Standard Input).
class StandardOutputNode final : public Node {
public:
    StandardOutputNode() : Node("output.standard", NodeCategory::Output) {
        addInput("image", PinType::RGBA);
    }
    void evaluate(EvalContext&) override {
        // Executor stage: capture `image` as the pipeline's result.
    }
};

} // namespace

void registerBuiltinNodes() {
    auto& reg = NodeRegistry::instance();
    reg.registerType({"input.standard", NodeCategory::Input, "pipeline.node.standard_input",
                      [] { return std::make_unique<StandardInputNode>(); }});
    reg.registerType({"input.resource", NodeCategory::Input, "pipeline.node.resource",
                      [] { return std::make_unique<ResourceInputNode>(); }});
    reg.registerType({"op.extract_channel", NodeCategory::Operation,
                      "pipeline.node.extract_channel",
                      [] { return std::make_unique<ExtractChannelNode>(); }});
    reg.registerType({"op.invert_channel", NodeCategory::Operation, "pipeline.node.invert_channel",
                      [] { return std::make_unique<InvertChannelNode>(); }});
    reg.registerType({"op.invert", NodeCategory::Operation, "pipeline.node.invert",
                      [] { return std::make_unique<InvertNode>(); }});
    reg.registerType({"op.blend", NodeCategory::Operation, "pipeline.node.blend",
                      [] { return std::make_unique<BlendNode>(); }});
    reg.registerType({"op.matching_mipmap", NodeCategory::Operation,
                      "pipeline.node.matching_mipmap",
                      [] { return std::make_unique<MatchingMipmapNode>(); }});
    reg.registerType({"op.extract_mipmap", NodeCategory::Operation, "pipeline.node.extract_mipmap",
                      [] { return std::make_unique<ExtractMipmapNode>(); }});
    reg.registerType({"op.downscale", NodeCategory::Operation, "pipeline.node.downscale",
                      [] { return std::make_unique<DownscaleNode>(); }});
    reg.registerType({"op.upscale", NodeCategory::Operation, "pipeline.node.upscale",
                      [] { return std::make_unique<UpscaleNode>(); }});
    reg.registerType({"op.regenerate_mipmaps", NodeCategory::Operation,
                      "pipeline.node.regenerate_mipmaps",
                      [] { return std::make_unique<RegenerateMipmapsNode>(); }});
    reg.registerType({"output.standard", NodeCategory::Output, "pipeline.node.standard_output",
                      [] { return std::make_unique<StandardOutputNode>(); }});
}

} // namespace whiteout::textool::pipeline
