// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file builtin_nodes.cpp
 * @brief The set of node types, registered into NodeRegistry.
 *
 * Nodes are PURE DATA: each subclass only declares its pins/params in its
 * constructor.  Execution lives outside the model in the PipelineExecutor
 * (services/), which interprets the graph using app types (Texture, etc.) —
 * keeping this model layer free of ImGui/SDL/Texture dependencies.
 *
 * Adding a type is one class here plus one registerType() row in
 * registerBuiltinNodes() — the format_registry idiom.
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

// Blend modes (Blizzard / WC3 style), as combobox option labels (i18n keys).
const std::vector<std::string> kBlendModes = {
    "pipeline.blend.transparent", "pipeline.blend.blend", "pipeline.blend.additive",
    "pipeline.blend.modulate", "pipeline.blend.modulate2x"};

// Downscale factor option labels (i18n keys).
const std::vector<std::string> kDownscaleFactors = {
    "pipeline.scale.half", "pipeline.scale.quarter", "pipeline.scale.eighth"};

// Mipmap-regeneration modes (i18n keys).  Custom (index 1) exposes a count.
const std::vector<std::string> kMipmapModes = {"pipeline.mipmode.current", "pipeline.mipmode.custom",
                                               "pipeline.mipmode.maximum"};

// Luma weighting methods (i18n keys).
const std::vector<std::string> kLumaMethods = {"pipeline.luma.rec709", "pipeline.luma.rec601",
                                               "pipeline.luma.average"};

// ── Inputs ──────────────────────────────────────────────────────────────────

// The pipeline's primary/standard input image (whatever it's applied to).
class StandardInputNode final : public Node {
public:
    StandardInputNode() : Node("input.standard", NodeCategory::Input) {
        addOutput("image", PinType::RGBA);
    }
};

// A preset loaded from the bundled resources/presets folder.
class ResourceInputNode final : public Node {
public:
    ResourceInputNode() : Node("input.resource", NodeCategory::Input) {
        addOutput("image", PinType::RGBA);
        addResourceParam("path"); // path relative to resources/presets
    }
};

// A constant integer value source (no inputs).
class ConstantIntegerNode final : public Node {
public:
    ConstantIntegerNode() : Node("input.const_int", NodeCategory::Input) {
        addOutput("value", PinType::Int);
        addParam("value", i64{0});
    }
};

// A constant real (double) value source (no inputs).
class ConstantRealNode final : public Node {
public:
    ConstantRealNode() : Node("input.const_real", NodeCategory::Input) {
        addOutput("value", PinType::Real);
        addParam("value", f64{0.0});
    }
};

// ── Operations ──────────────────────────────────────────────────────────────

// RGBA -> single-channel (R) carrying the selected channel.
class ExtractChannelNode final : public Node {
public:
    ExtractChannelNode() : Node("op.extract_channel", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("channel", PinType::R);
        addEnumParam("channel", 0, kChannelLabels); // 0=R, 1=G, 2=B, 3=A
    }
};

// RGBA -> RGBA with the selected channel replaced by (max - value).
class InvertChannelNode final : public Node {
public:
    InvertChannelNode() : Node("op.invert_channel", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("channel", 0, kChannelLabels); // 0=R, 1=G, 2=B, 3=A
    }
};

// R -> R, value replaced by (max - value).  No channel to pick.
class InvertNode final : public Node {
public:
    InvertNode() : Node("op.invert", NodeCategory::Operation) {
        addInput("channel", PinType::R);
        addOutput("channel", PinType::R);
    }
};

// Four single channels (R) -> one RGBA image.
class MergeChannelsNode final : public Node {
public:
    MergeChannelsNode() : Node("op.merge_channels", NodeCategory::Operation) {
        addInput("red", PinType::R);
        addInput("green", PinType::R);
        addInput("blue", PinType::R);
        addInput("alpha", PinType::R);
        addOutput("image", PinType::RGBA);
    }
};

// A single channel (R) -> a grayscale RGBA image (R=G=B=value, A=opaque).
class PrimsNode final : public Node {
public:
    PrimsNode() : Node("op.prims", NodeCategory::Operation) {
        addInput("channel", PinType::R);
        addOutput("image", PinType::RGBA);
    }
};

// ── Arithmetic (Number = Integer / Real / single Channel) ──────────────────
// Two channels must match in size; Integer+Real promotes to Real; a Channel and
// a number applies the number to each element.

class AddNode final : public Node {
public:
    AddNode() : Node("op.add", NodeCategory::Operation) {
        addInput("a", PinType::Number);
        addInput("b", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

class MultiplyNode final : public Node {
public:
    MultiplyNode() : Node("op.multiply", NodeCategory::Operation) {
        addInput("a", PinType::Number);
        addInput("b", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

class NegateNode final : public Node {
public:
    NegateNode() : Node("op.negate", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

class SquareRootNode final : public Node {
public:
    SquareRootNode() : Node("op.sqrt", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// RGBA -> single-channel (R) luma, computed by the chosen weighting method.
class LumaNode final : public Node {
public:
    LumaNode() : Node("op.luma", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("channel", PinType::R);
        addEnumParam("method", 0, kLumaMethods);
    }
};

// Inputs `top layer` (over) and `bottom layer` (under), RGBA -> RGBA combined
// per the chosen mode.
class BlendNode final : public Node {
public:
    BlendNode() : Node("op.blend", NodeCategory::Operation) {
        addInput("top layer", PinType::RGBA);
        addInput("bottom layer", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("mode", 0, kBlendModes);
    }
};

// Inputs `base` and `related` RGBA -> Int mip index of the base mip whose size
// matches related's top mip (0 if related's top mip is larger than base's).
class MatchingMipmapNode final : public Node {
public:
    MatchingMipmapNode() : Node("op.matching_mipmap", NodeCategory::Operation) {
        addInput("base", PinType::RGBA);
        addInput("related", PinType::RGBA);
        addOutput("mipmap", PinType::Int);
    }
};

// Inputs `image` RGBA and `mipmap` Int -> RGBA of that mip level.
class ExtractMipmapNode final : public Node {
public:
    ExtractMipmapNode() : Node("op.extract_mipmap", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("mipmap", PinType::Int);
        addOutput("image", PinType::RGBA);
    }
};

// RGBA -> RGBA downsampled by the selected factor (1/2, 1/4, 1/8).
class DownscaleNode final : public Node {
public:
    DownscaleNode() : Node("op.downscale", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("factor", 0, kDownscaleFactors);
    }
};

// RGBA -> RGBA produced by running a selected standard pipeline on the input.
class SubpipelineNode final : public Node {
public:
    SubpipelineNode() : Node("op.subpipeline", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addPipelineParam("pipeline");
    }
};

// RGBA -> RGBA run through the selected Real-ESRGAN model.
class UpscaleNode final : public Node {
public:
    UpscaleNode() : Node("op.upscale", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addModelParam("model");
    }
};

// RGBA -> RGBA with a rebuilt mip chain.  Mode: Current keeps the source count,
// Maximum builds to 1x1, Custom uses `count`.
class RegenerateMipmapsNode final : public Node {
public:
    RegenerateMipmapsNode() : Node("op.regenerate_mipmaps", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addEnumParam("mode", 0, kMipmapModes);
        addParam("count", i64{1}); // mip-level count
        gateLastParam("mode", 1);  // only shown in Custom mode
    }
};

// ── Output ──────────────────────────────────────────────────────────────────

// The pipeline's primary/standard output image (mirrors Standard Input).
class StandardOutputNode final : public Node {
public:
    StandardOutputNode() : Node("output.standard", NodeCategory::Output) {
        addInput("image", PinType::RGBA);
    }
};

} // namespace

void registerBuiltinNodes() {
    auto& reg = NodeRegistry::instance();
    reg.registerType({"input.standard", NodeCategory::Input, "pipeline.node.standard_input",
                      [] { return std::make_unique<StandardInputNode>(); }});
    reg.registerType({"input.resource", NodeCategory::Input, "pipeline.node.resource",
                      [] { return std::make_unique<ResourceInputNode>(); }});
    reg.registerType({"input.const_int", NodeCategory::Input, "pipeline.node.const_int",
                      [] { return std::make_unique<ConstantIntegerNode>(); }});
    reg.registerType({"input.const_real", NodeCategory::Input, "pipeline.node.const_real",
                      [] { return std::make_unique<ConstantRealNode>(); }});
    reg.registerType({"op.extract_channel", NodeCategory::Operation,
                      "pipeline.node.extract_channel",
                      [] { return std::make_unique<ExtractChannelNode>(); }});
    reg.registerType({"op.invert_channel", NodeCategory::Operation, "pipeline.node.invert_channel",
                      [] { return std::make_unique<InvertChannelNode>(); }});
    reg.registerType({"op.invert", NodeCategory::Operation, "pipeline.node.invert",
                      [] { return std::make_unique<InvertNode>(); }});
    reg.registerType({"op.merge_channels", NodeCategory::Operation,
                      "pipeline.node.merge_channels",
                      [] { return std::make_unique<MergeChannelsNode>(); }});
    reg.registerType({"op.prims", NodeCategory::Operation, "pipeline.node.prims",
                      [] { return std::make_unique<PrimsNode>(); }});
    reg.registerType({"op.luma", NodeCategory::Operation, "pipeline.node.luma",
                      [] { return std::make_unique<LumaNode>(); }});
    reg.registerType({"op.add", NodeCategory::Operation, "pipeline.node.add",
                      [] { return std::make_unique<AddNode>(); }});
    reg.registerType({"op.multiply", NodeCategory::Operation, "pipeline.node.multiply",
                      [] { return std::make_unique<MultiplyNode>(); }});
    reg.registerType({"op.negate", NodeCategory::Operation, "pipeline.node.negate",
                      [] { return std::make_unique<NegateNode>(); }});
    reg.registerType({"op.sqrt", NodeCategory::Operation, "pipeline.node.sqrt",
                      [] { return std::make_unique<SquareRootNode>(); }});
    reg.registerType({"op.blend", NodeCategory::Operation, "pipeline.node.blend",
                      [] { return std::make_unique<BlendNode>(); }});
    reg.registerType({"op.matching_mipmap", NodeCategory::Operation,
                      "pipeline.node.matching_mipmap",
                      [] { return std::make_unique<MatchingMipmapNode>(); }});
    reg.registerType({"op.extract_mipmap", NodeCategory::Operation, "pipeline.node.extract_mipmap",
                      [] { return std::make_unique<ExtractMipmapNode>(); }});
    reg.registerType({"op.downscale", NodeCategory::Operation, "pipeline.node.downscale",
                      [] { return std::make_unique<DownscaleNode>(); }});
    reg.registerType({"op.subpipeline", NodeCategory::Operation, "pipeline.node.subpipeline",
                      [] { return std::make_unique<SubpipelineNode>(); }});
    reg.registerType({"op.upscale", NodeCategory::Operation, "pipeline.node.upscale",
                      [] { return std::make_unique<UpscaleNode>(); }});
    reg.registerType({"op.regenerate_mipmaps", NodeCategory::Operation,
                      "pipeline.node.regenerate_mipmaps",
                      [] { return std::make_unique<RegenerateMipmapsNode>(); }});
    reg.registerType({"output.standard", NodeCategory::Output, "pipeline.node.standard_output",
                      [] { return std::make_unique<StandardOutputNode>(); }});
}

} // namespace whiteout::textool::pipeline
