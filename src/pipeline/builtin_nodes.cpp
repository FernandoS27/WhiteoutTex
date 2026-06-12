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

// Derivative kernels (i18n keys).  Quad = central difference, Sobel = 3x3.
const std::vector<std::string> kDerivativeModes = {"pipeline.deriv.quad", "pipeline.deriv.sobel"};

// Mirror axes and scale resampling filters (i18n keys).
const std::vector<std::string> kMirrorAxes = {"pipeline.axis.x", "pipeline.axis.y"};
// Edge handling for the Displace node (i18n keys).
const std::vector<std::string> kDisplaceModes = {"pipeline.displace.wrap", "pipeline.displace.clamp",
                                                 "pipeline.displace.transparent"};
const std::vector<std::string> kScaleFilters = {"pipeline.filter.bicubic", "pipeline.filter.linear",
                                                "pipeline.filter.lanczos"};

// Comparison operators for the Conditional control node (i18n keys).
const std::vector<std::string> kComparators = {
    "pipeline.cmp.equal",   "pipeline.cmp.not_equal",    "pipeline.cmp.less",
    "pipeline.cmp.less_eq", "pipeline.cmp.greater",      "pipeline.cmp.greater_eq"};

// Texture kinds (i18n keys), in TextureKind enum order so the combo index
// equals the enum value emitted as an int.
const std::vector<std::string> kKindLabels = {
    "pipeline.kind.other",         "pipeline.kind.diffuse",
    "pipeline.kind.normal",        "pipeline.kind.specular",
    "pipeline.kind.orm",           "pipeline.kind.albedo",
    "pipeline.kind.roughness",     "pipeline.kind.metalness",
    "pipeline.kind.ambient_occlusion", "pipeline.kind.gloss",
    "pipeline.kind.emissive",      "pipeline.kind.alpha_mask",
    "pipeline.kind.binary_mask",   "pipeline.kind.transparency_mask",
    "pipeline.kind.blend_mask",    "pipeline.kind.lightmap",
    "pipeline.kind.environment_pbr", "pipeline.kind.environment_legacy",
    "pipeline.kind.multikind",     "pipeline.kind.unused"};

// ── Inputs ──────────────────────────────────────────────────────────────────

// The pipeline's primary/standard input image (whatever it's applied to).
class StandardInputNode final : public Node {
public:
    StandardInputNode() : Node("input.standard", NodeCategory::Input) {
        addOutput("image", PinType::RGBA);
        addParam("name", std::string{"Input"}); // names this external image input
    }
};

// A real-valued pipeline parameter, supplied when the pipeline is run (a field,
// or a slider when clamped to [min, max]).  The in-node "value" is the default.
class RealInputNode final : public Node {
public:
    RealInputNode() : Node("input.real", NodeCategory::Input) {
        addOutput("value", PinType::Real);
        addParam("name", std::string{"value"});
        addParam("value", f64{0.0});
        addParam("clamp", false);
        addParam("min", f64{0.0});
        gateLastParam("clamp", 1); // min shown only when clamp is on
        addParam("max", f64{1.0});
        gateLastParam("clamp", 1); // max shown only when clamp is on
    }
};

// An integer-valued pipeline parameter, supplied when the pipeline is run.
// The in-node "value" is the default; clamp bounds it to [min, max].
class IntegerInputNode final : public Node {
public:
    IntegerInputNode() : Node("input.integer", NodeCategory::Input) {
        addOutput("value", PinType::Int);
        addParam("name", std::string{"value"});
        addParam("value", i64{0});
        addParam("clamp", false);
        addParam("min", i64{0});
        gateLastParam("clamp", 1); // min shown only when clamp is on
        addParam("max", i64{255});
        gateLastParam("clamp", 1); // max shown only when clamp is on
    }
};

// A single-channel pipeline input, supplied when the pipeline is run as a
// function.  Carries only a name (the channel data arrives at run time).
class ChannelInputNode final : public Node {
public:
    ChannelInputNode() : Node("input.channel", NodeCategory::Input) {
        addOutput("channel", PinType::R);
        addParam("name", std::string{"channel"});
    }
};

// A polymorphic numeric pipeline input (Real, Integer, or single Channel),
// supplied when the pipeline is run.  Carries only a name (value at run time).
class NumericInputNode final : public Node {
public:
    NumericInputNode() : Node("input.number", NodeCategory::Input) {
        addOutput("value", PinType::Number);
        addParam("name", std::string{"value"});
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

// ── Constants ───────────────────────────────────────────────────────────────

// A constant integer value source (no inputs).
class ConstantIntegerNode final : public Node {
public:
    ConstantIntegerNode() : Node("input.const_int", NodeCategory::Constant) {
        addOutput("value", PinType::Int);
        addParam("value", i64{0});
    }
};

// A constant real (double) value source (no inputs).
class ConstantRealNode final : public Node {
public:
    ConstantRealNode() : Node("input.const_real", NodeCategory::Constant) {
        addOutput("value", PinType::Real);
        addParam("value", f64{0.0});
    }
};

// A constant channel: width x height (Int) filled with a value (Real) -> R.
class ConstantChannelNode final : public Node {
public:
    ConstantChannelNode() : Node("input.const_channel", NodeCategory::Constant) {
        addInput("width", PinType::Int);
        addInput("height", PinType::Int);
        addInput("value", PinType::Real);
        addOutput("channel", PinType::R);
    }
};

// A constant texture kind picked from a combo, emitted as its TextureKind int.
class ConstantKindNode final : public Node {
public:
    ConstantKindNode() : Node("input.const_kind", NodeCategory::Constant) {
        addOutput("value", PinType::Int);
        addEnumParam("kind", 0, kKindLabels);
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

// RGBA + Real -> RGBA with the selected channel filled with the value.
class FillChannelNode final : public Node {
public:
    FillChannelNode() : Node("op.fill_channel", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("value", PinType::Real);
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

// Per-element minimum / maximum (polymorphic over Int / Real / single Channel).
class MinNode final : public Node {
public:
    MinNode() : Node("op.min", NodeCategory::Operation) {
        addInput("a", PinType::Number);
        addInput("b", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

class MaxNode final : public Node {
public:
    MaxNode() : Node("op.max", NodeCategory::Operation) {
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

class ReciprocalNode final : public Node {
public:
    ReciprocalNode() : Node("op.reciprocal", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// Trigonometric sine of the input (radians).  Channels are processed per element.
class SineNode final : public Node {
public:
    SineNode() : Node("op.sine", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// Trigonometric cosine of the input (radians).  Per element for channels.
class CosineNode final : public Node {
public:
    CosineNode() : Node("op.cosine", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// Natural logarithm (base e) of the input; non-positive values yield 0.
class NaturalLogNode final : public Node {
public:
    NaturalLogNode() : Node("op.ln", NodeCategory::Operation) {
        addInput("value", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// base raised to the exponent.  A channel base is processed per element.
class PowerNode final : public Node {
public:
    PowerNode() : Node("op.power", NodeCategory::Operation) {
        addInput("base", PinType::Number);
        addInput("exponent", PinType::Number);
        addOutput("result", PinType::Number);
    }
};

// ── Bitwise (Integer) ───────────────────────────────────────────────────────
// Operands are coerced to a 64-bit integer; the result is an Integer.

class BitwiseBinaryNode final : public Node { // AND / OR / XOR
public:
    explicit BitwiseBinaryNode(const char* type_id) : Node(type_id, NodeCategory::Operation) {
        addInput("a", PinType::Int);
        addInput("b", PinType::Int);
        addOutput("result", PinType::Int);
    }
};

class BitwiseNotNode final : public Node {
public:
    BitwiseNotNode() : Node("op.bit_not", NodeCategory::Operation) {
        addInput("value", PinType::Int);
        addOutput("result", PinType::Int);
    }
};

class BitShiftNode final : public Node { // Shift Left / Shift Right
public:
    explicit BitShiftNode(const char* type_id) : Node(type_id, NodeCategory::Operation) {
        addInput("value", PinType::Int);
        addInput("amount", PinType::Int);
        addOutput("result", PinType::Int);
    }
};

// A single channel (R) -> its partial derivatives in X and Y (both R).
class DerivativesNode final : public Node {
public:
    DerivativesNode() : Node("op.derivatives", NodeCategory::Operation) {
        addInput("channel", PinType::R);
        addOutput("dx", PinType::R);
        addOutput("dy", PinType::R);
        addEnumParam("mode", 0, kDerivativeModes); // 0=Quad, 1=Sobel
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

// Reports an image's dimensions, mip count, semantic kind, and per-channel
// subkinds.  Kind and subkinds are TextureKind enum values (ints); the
// subkinds are only meaningful for Multikind textures.
class ImagePropertiesNode final : public Node {
public:
    ImagePropertiesNode() : Node("op.image_properties", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("width", PinType::Int);
        addOutput("height", PinType::Int);
        addOutput("mipmaps", PinType::Int);
        addOutput("kind", PinType::Int);
        addOutput("r kind", PinType::Int);
        addOutput("g kind", PinType::Int);
        addOutput("b kind", PinType::Int);
        addOutput("a kind", PinType::Int);
    }
};

// Tags an image with a semantic kind (TextureKind value).  The kind input is
// optional: when unconnected the image's existing kind is left unchanged.
class SetKindNode final : public Node {
public:
    SetKindNode() : Node("op.set_kind", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("kind", PinType::Int);
        addOutput("image", PinType::RGBA);
    }
};

// ── Geometry (work on an image OR a channel via the polymorphic Any pin) ────

// Flip horizontally (X) or vertically (Y).
class MirrorNode final : public Node {
public:
    MirrorNode() : Node("op.mirror", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addOutput("output", PinType::Any);
        addEnumParam("axis", 0, kMirrorAxes);
    }
};

// Rotate by a Real number of degrees (bilinear, keeps dimensions).
class RotateNode final : public Node {
public:
    RotateNode() : Node("op.rotate", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addInput("degrees", PinType::Real);
        addOutput("output", PinType::Any);
    }
};

// Resample to explicit width x height pixels with the chosen filter.
class ScaleToNode final : public Node {
public:
    ScaleToNode() : Node("op.scale_to", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addInput("width", PinType::Int);
        addInput("height", PinType::Int);
        addOutput("output", PinType::Any);
        addEnumParam("filter", 0, kScaleFilters);
    }
};

// Resample by a Real factor (1.0 = unchanged, 1.25 = 125%, 0.5 = half) with the
// chosen filter.
class ScaleByNode final : public Node {
public:
    ScaleByNode() : Node("op.scale_by", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addInput("factor", PinType::Real);
        addOutput("output", PinType::Any);
        addEnumParam("filter", 0, kScaleFilters);
    }
};

// Shift the image/channel by integer X/Y offsets.  Edge handling (mode):
// Wrap (scroll), Clamp (smear edge) or Transparent (vacated area cleared).
class DisplaceNode final : public Node {
public:
    DisplaceNode() : Node("op.displace", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addInput("x", PinType::Int);
        addInput("y", PinType::Int);
        addOutput("output", PinType::Any);
        addEnumParam("mode", 0, kDisplaceModes);
    }
};

// Resize the canvas to explicit width x height, keeping the source CENTERED.
// Larger dimensions pad (transparent / black); smaller dimensions crop.  Unlike
// Scale, the pixels are not resampled.
class ResizeNode final : public Node {
public:
    ResizeNode() : Node("op.resize_canvas", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addInput("width", PinType::Int);
        addInput("height", PinType::Int);
        addOutput("output", PinType::Any);
    }
};

// Make the image/channel seamlessly tileable by blending a half-offset copy
// over the borders.
class TilingNode final : public Node {
public:
    TilingNode() : Node("op.tiling", NodeCategory::Operation) {
        addInput("input", PinType::Any);
        addOutput("output", PinType::Any);
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

// Runs a selected pipeline as a sub-step; pins mirror its interface.  Lives in
// Control (it composes/branches flow) and may recurse (call itself).
class SubpipelineNode final : public Node {
public:
    SubpipelineNode() : Node("op.subpipeline", NodeCategory::Control) {
        // Default pins mirror a standard pipeline; replaced to match the
        // selected pipeline's interface (see PipelineEditor::syncSubpipelinePins).
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addPipelineParam("pipeline");
        markDynamicPins();
    }
};

// RGBA -> RGBA run through the selected Real-ESRGAN model.
class UpscaleNode final : public Node {
public:
    UpscaleNode() : Node("op.upscale", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addModelParam("model");
        addParam("upscale_alpha", false); // run alpha through the model too
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

// ── Filters (convolution) ──────────────────────────────────────────────────

class GaussianBlurNode final : public Node {
public:
    GaussianBlurNode() : Node("op.gaussian_blur", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("radius", PinType::Real); // default 2 when unconnected
        addOutput("image", PinType::RGBA);
    }
};

class SharpenNode final : public Node {
public:
    SharpenNode() : Node("op.sharpen", NodeCategory::Operation) {
        addInput("image", PinType::RGBA);
        addInput("strength", PinType::Real); // default 1 when unconnected
        addOutput("image", PinType::RGBA);
    }
};

class SobelNode final : public Node {
public:
    SobelNode() : Node("op.sobel", NodeCategory::Operation) {
        addInput("channel", PinType::R);
        addOutput("channel", PinType::R);
    }
};

// ── Control ───────────────────────────────────────────────────────────────

// Routes a polymorphic value to one of two outputs based on comparing two
// numeric operands (a <cmp> b).  On true the value flows out "true" (and
// "false" is unset); on false it flows out "false".
class ConditionalNode final : public Node {
public:
    ConditionalNode() : Node("ctrl.conditional", NodeCategory::Control) {
        addInput("value", PinType::Any); // routes any type (image / channel / scalar)
        addInput("a", PinType::Number);
        addInput("b", PinType::Number);
        addOutput("true", PinType::Any);
        addOutput("false", PinType::Any);
        addEnumParam("comparator", 0, kComparators);
        addParam("epsilon", f64{1e-6}); // tolerance for Equal / Not Equal on floats
    }
};

// Compares two numeric operands and outputs `if true` or `if false` (either of
// which may be any type) — a polymorphic ternary select.
class SelectNode final : public Node {
public:
    SelectNode() : Node("ctrl.select", NodeCategory::Control) {
        addInput("a", PinType::Number);
        addInput("b", PinType::Number);
        addInput("if true", PinType::Any);
        addInput("if false", PinType::Any);
        addOutput("result", PinType::Any);
        addEnumParam("comparator", 0, kComparators);
        addParam("epsilon", f64{1e-6}); // tolerance for Equal / Not Equal on floats
    }
};

// Rendezvous (phi): joins diverged branches back into one path.  Emits whichever
// input carries a value — only one branch of a Conditional is ever live.
class RendezvousNode final : public Node {
public:
    RendezvousNode() : Node("ctrl.rendezvous", NodeCategory::Control) {
        addInput("a", PinType::Any);
        addInput("b", PinType::Any);
        addOutput("result", PinType::Any);
    }
};

// Invokes a Local Pipeline frame from the same graph; pins mirror the frame's
// internal Input/Output ports.  May recurse (target its own frame).
class LocalCallNode final : public Node {
public:
    LocalCallNode() : Node("local.call", NodeCategory::Control) {
        addInput("image", PinType::RGBA);
        addOutput("image", PinType::RGBA);
        addLocalFrameParam("frame");
        markDynamicPins(); // pins are replaced to mirror the selected frame
    }
};

// ── Frame ───────────────────────────────────────────────────────────────────

// A canvas container: nodes positioned inside it form a unique "local pipeline"
// callable (only) from this graph via Call Local Pipeline.  No data pins.
class LocalPipelineFrameNode final : public Node {
public:
    LocalPipelineFrameNode() : Node("frame.local", NodeCategory::Frame) {
        addParam("name", std::string{"Local"}); // unique name; called by this graph
        addParam("w", f64{360.0});               // frame size (editor updates these)
        addParam("h", f64{220.0});
    }
};

// ── Output ──────────────────────────────────────────────────────────────────

// The pipeline's primary/standard output image (mirrors Standard Input).
class StandardOutputNode final : public Node {
public:
    StandardOutputNode() : Node("output.standard", NodeCategory::Output) {
        addInput("image", PinType::RGBA);
        addParam("name", std::string{"Output"}); // names this external image output
    }
};

// A named real-valued pipeline output (for function pipelines).
class RealOutputNode final : public Node {
public:
    RealOutputNode() : Node("output.real", NodeCategory::Output) {
        addInput("value", PinType::Real);
        addParam("name", std::string{"value"});
    }
};

// A named integer-valued pipeline output (for function pipelines).
class IntegerOutputNode final : public Node {
public:
    IntegerOutputNode() : Node("output.integer", NodeCategory::Output) {
        addInput("value", PinType::Int);
        addParam("name", std::string{"value"});
    }
};

// A named single-channel pipeline output (for function pipelines).
class ChannelOutputNode final : public Node {
public:
    ChannelOutputNode() : Node("output.channel", NodeCategory::Output) {
        addInput("channel", PinType::R);
        addParam("name", std::string{"channel"});
    }
};

// A named polymorphic numeric pipeline output (Real / Integer / single Channel).
class NumericOutputNode final : public Node {
public:
    NumericOutputNode() : Node("output.number", NodeCategory::Output) {
        addInput("value", PinType::Number);
        addParam("name", std::string{"value"});
    }
};

} // namespace

void registerBuiltinNodes() {
    auto& reg = NodeRegistry::instance();
    reg.registerType({"input.standard", NodeCategory::Input, "pipeline.node.standard_input",
                      [] { return std::make_unique<StandardInputNode>(); }});
    reg.registerType({"input.real", NodeCategory::Input, "pipeline.node.real_input",
                      [] { return std::make_unique<RealInputNode>(); }});
    reg.registerType({"input.integer", NodeCategory::Input, "pipeline.node.integer_input",
                      [] { return std::make_unique<IntegerInputNode>(); }});
    reg.registerType({"input.channel", NodeCategory::Input, "pipeline.node.channel_input",
                      [] { return std::make_unique<ChannelInputNode>(); }});
    reg.registerType({"input.number", NodeCategory::Input, "pipeline.node.numeric_input",
                      [] { return std::make_unique<NumericInputNode>(); }});
    reg.registerType({"input.resource", NodeCategory::Input, "pipeline.node.resource",
                      [] { return std::make_unique<ResourceInputNode>(); }});
    reg.registerType({"input.const_int", NodeCategory::Constant, "pipeline.node.const_int",
                      [] { return std::make_unique<ConstantIntegerNode>(); }});
    reg.registerType({"input.const_real", NodeCategory::Constant, "pipeline.node.const_real",
                      [] { return std::make_unique<ConstantRealNode>(); }});
    reg.registerType({"input.const_channel", NodeCategory::Constant, "pipeline.node.const_channel",
                      [] { return std::make_unique<ConstantChannelNode>(); }});
    reg.registerType({"input.const_kind", NodeCategory::Constant, "pipeline.node.const_kind",
                      [] { return std::make_unique<ConstantKindNode>(); }});
    reg.registerType({"op.extract_channel", NodeCategory::Operation,
                      "pipeline.node.extract_channel",
                      [] { return std::make_unique<ExtractChannelNode>(); }});
    reg.registerType({"op.invert_channel", NodeCategory::Operation, "pipeline.node.invert_channel",
                      [] { return std::make_unique<InvertChannelNode>(); }});
    reg.registerType({"op.fill_channel", NodeCategory::Operation, "pipeline.node.fill_channel",
                      [] { return std::make_unique<FillChannelNode>(); }});
    reg.registerType({"op.invert", NodeCategory::Operation, "pipeline.node.invert",
                      [] { return std::make_unique<InvertNode>(); }});
    reg.registerType({"op.merge_channels", NodeCategory::Operation,
                      "pipeline.node.merge_channels",
                      [] { return std::make_unique<MergeChannelsNode>(); }});
    reg.registerType({"op.prims", NodeCategory::Operation, "pipeline.node.prims",
                      [] { return std::make_unique<PrimsNode>(); }});
    reg.registerType({"op.luma", NodeCategory::Operation, "pipeline.node.luma",
                      [] { return std::make_unique<LumaNode>(); }});
    reg.registerType({"op.image_properties", NodeCategory::Operation,
                      "pipeline.node.image_properties",
                      [] { return std::make_unique<ImagePropertiesNode>(); }});
    reg.registerType({"op.set_kind", NodeCategory::Operation, "pipeline.node.set_kind",
                      [] { return std::make_unique<SetKindNode>(); }});
    reg.registerType({"op.mirror", NodeCategory::Operation, "pipeline.node.mirror",
                      [] { return std::make_unique<MirrorNode>(); }});
    reg.registerType({"op.rotate", NodeCategory::Operation, "pipeline.node.rotate",
                      [] { return std::make_unique<RotateNode>(); }});
    reg.registerType({"op.scale_to", NodeCategory::Operation, "pipeline.node.scale_to",
                      [] { return std::make_unique<ScaleToNode>(); }});
    reg.registerType({"op.scale_by", NodeCategory::Operation, "pipeline.node.scale_by",
                      [] { return std::make_unique<ScaleByNode>(); }});
    reg.registerType({"op.displace", NodeCategory::Operation, "pipeline.node.displace",
                      [] { return std::make_unique<DisplaceNode>(); }});
    reg.registerType({"op.resize_canvas", NodeCategory::Operation, "pipeline.node.resize_canvas",
                      [] { return std::make_unique<ResizeNode>(); }});
    reg.registerType({"op.tiling", NodeCategory::Operation, "pipeline.node.tiling",
                      [] { return std::make_unique<TilingNode>(); }});
    reg.registerType({"op.add", NodeCategory::Operation, "pipeline.node.add",
                      [] { return std::make_unique<AddNode>(); }});
    reg.registerType({"op.multiply", NodeCategory::Operation, "pipeline.node.multiply",
                      [] { return std::make_unique<MultiplyNode>(); }});
    reg.registerType({"op.min", NodeCategory::Operation, "pipeline.node.min",
                      [] { return std::make_unique<MinNode>(); }});
    reg.registerType({"op.max", NodeCategory::Operation, "pipeline.node.max",
                      [] { return std::make_unique<MaxNode>(); }});
    reg.registerType({"op.negate", NodeCategory::Operation, "pipeline.node.negate",
                      [] { return std::make_unique<NegateNode>(); }});
    reg.registerType({"op.sqrt", NodeCategory::Operation, "pipeline.node.sqrt",
                      [] { return std::make_unique<SquareRootNode>(); }});
    reg.registerType({"op.reciprocal", NodeCategory::Operation, "pipeline.node.reciprocal",
                      [] { return std::make_unique<ReciprocalNode>(); }});
    reg.registerType({"op.sine", NodeCategory::Operation, "pipeline.node.sine",
                      [] { return std::make_unique<SineNode>(); }});
    reg.registerType({"op.cosine", NodeCategory::Operation, "pipeline.node.cosine",
                      [] { return std::make_unique<CosineNode>(); }});
    reg.registerType({"op.ln", NodeCategory::Operation, "pipeline.node.ln",
                      [] { return std::make_unique<NaturalLogNode>(); }});
    reg.registerType({"op.power", NodeCategory::Operation, "pipeline.node.power",
                      [] { return std::make_unique<PowerNode>(); }});
    reg.registerType({"op.bit_and", NodeCategory::Operation, "pipeline.node.bit_and",
                      [] { return std::make_unique<BitwiseBinaryNode>("op.bit_and"); }});
    reg.registerType({"op.bit_or", NodeCategory::Operation, "pipeline.node.bit_or",
                      [] { return std::make_unique<BitwiseBinaryNode>("op.bit_or"); }});
    reg.registerType({"op.bit_xor", NodeCategory::Operation, "pipeline.node.bit_xor",
                      [] { return std::make_unique<BitwiseBinaryNode>("op.bit_xor"); }});
    reg.registerType({"op.bit_not", NodeCategory::Operation, "pipeline.node.bit_not",
                      [] { return std::make_unique<BitwiseNotNode>(); }});
    reg.registerType({"op.bit_shl", NodeCategory::Operation, "pipeline.node.bit_shl",
                      [] { return std::make_unique<BitShiftNode>("op.bit_shl"); }});
    reg.registerType({"op.bit_shr", NodeCategory::Operation, "pipeline.node.bit_shr",
                      [] { return std::make_unique<BitShiftNode>("op.bit_shr"); }});
    reg.registerType({"op.derivatives", NodeCategory::Operation, "pipeline.node.derivatives",
                      [] { return std::make_unique<DerivativesNode>(); }});
    reg.registerType({"op.blend", NodeCategory::Operation, "pipeline.node.blend",
                      [] { return std::make_unique<BlendNode>(); }});
    reg.registerType({"op.matching_mipmap", NodeCategory::Operation,
                      "pipeline.node.matching_mipmap",
                      [] { return std::make_unique<MatchingMipmapNode>(); }});
    reg.registerType({"op.extract_mipmap", NodeCategory::Operation, "pipeline.node.extract_mipmap",
                      [] { return std::make_unique<ExtractMipmapNode>(); }});
    reg.registerType({"op.downscale", NodeCategory::Operation, "pipeline.node.downscale",
                      [] { return std::make_unique<DownscaleNode>(); }});
    reg.registerType({"op.subpipeline", NodeCategory::Control, "pipeline.node.subpipeline",
                      [] { return std::make_unique<SubpipelineNode>(); }});
    reg.registerType({"op.upscale", NodeCategory::Operation, "pipeline.node.upscale",
                      [] { return std::make_unique<UpscaleNode>(); }});
    reg.registerType({"op.regenerate_mipmaps", NodeCategory::Operation,
                      "pipeline.node.regenerate_mipmaps",
                      [] { return std::make_unique<RegenerateMipmapsNode>(); }});
    reg.registerType({"op.gaussian_blur", NodeCategory::Operation, "pipeline.node.gaussian_blur",
                      [] { return std::make_unique<GaussianBlurNode>(); }});
    reg.registerType({"op.sharpen", NodeCategory::Operation, "pipeline.node.sharpen",
                      [] { return std::make_unique<SharpenNode>(); }});
    reg.registerType({"op.sobel", NodeCategory::Operation, "pipeline.node.sobel",
                      [] { return std::make_unique<SobelNode>(); }});
    reg.registerType({"ctrl.conditional", NodeCategory::Control, "pipeline.node.conditional",
                      [] { return std::make_unique<ConditionalNode>(); }});
    reg.registerType({"ctrl.select", NodeCategory::Control, "pipeline.node.select",
                      [] { return std::make_unique<SelectNode>(); }});
    reg.registerType({"ctrl.rendezvous", NodeCategory::Control, "pipeline.node.rendezvous",
                      [] { return std::make_unique<RendezvousNode>(); }});
    reg.registerType({"local.call", NodeCategory::Control, "pipeline.node.local_call",
                      [] { return std::make_unique<LocalCallNode>(); }});
    reg.registerType({"frame.local", NodeCategory::Frame, "pipeline.node.frame_local",
                      [] { return std::make_unique<LocalPipelineFrameNode>(); }});
    reg.registerType({"output.standard", NodeCategory::Output, "pipeline.node.standard_output",
                      [] { return std::make_unique<StandardOutputNode>(); }});
    reg.registerType({"output.real", NodeCategory::Output, "pipeline.node.real_output",
                      [] { return std::make_unique<RealOutputNode>(); }});
    reg.registerType({"output.integer", NodeCategory::Output, "pipeline.node.integer_output",
                      [] { return std::make_unique<IntegerOutputNode>(); }});
    reg.registerType({"output.channel", NodeCategory::Output, "pipeline.node.channel_output",
                      [] { return std::make_unique<ChannelOutputNode>(); }});
    reg.registerType({"output.number", NodeCategory::Output, "pipeline.node.numeric_output",
                      [] { return std::make_unique<NumericOutputNode>(); }});
}

} // namespace whiteout::textool::pipeline
