// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file pin.h
 * @brief Pin data types for the pipeline node graph.
 *
 * A pin carries one of seven value kinds.  We model the value as a single
 * std::variant (tagged union) rather than a Pin class hierarchy: PinType is
 * just the variant's alternative index, and type-dispatched logic (conversion,
 * preview, serialization) is expressed with std::visit.
 *
 * Image planes are ref-counted handles so a pin carrying a large buffer costs
 * a refcount bump, not a copy.  Scalar parameters (Int/Real/Bool/String) reuse
 * a narrower variant — see ParamValue.
 *
 * This header is intentionally free of ImGui / SDL dependencies; the pipeline
 * model is pure data so it can be serialized and (later) run headless.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "common_types.h"

namespace whiteout::textool::pipeline {

/// An 8-bit image plane with a fixed channel count.  Distinct types per channel
/// count let the variant (and thus pin type-checking) distinguish RGBA/RGB/R at
/// compile time.  Evaluation/upscaling fill these in later stages.
template <int Channels>
struct ImagePlane {
    static constexpr int kChannels = Channels;
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data; ///< width * height * Channels, row-major, 8-bit/channel.
};

using ImageRGBA = std::shared_ptr<const ImagePlane<4>>;
using ImageRGB = std::shared_ptr<const ImagePlane<3>>;
using ImageR = std::shared_ptr<const ImagePlane<1>>;

/// The pin value kinds.  Indices 0..6 are LOAD-BEARING: they match the
/// alternative order of PinValue below (PinType == PinValue::index()).
/// `Number` (7) is a connection-only polymorphic type used by arithmetic nodes
/// (Integer / Real / single Channel); it has no PinValue alternative.
enum class PinType : u8 {
    RGBA = 0,
    RGB = 1,
    R = 2,
    Int = 3,
    Real = 4,
    Bool = 5,
    String = 6,
    Number = 7,
};

/// Full pin payload — image handles plus scalars.
using PinValue = std::variant<ImageRGBA, ImageRGB, ImageR, // image planes
                              i64, f64, bool, std::string>; // scalars

/// Scalar subset used by editable node parameters.  Images flow only through
/// pins/links, never as params (no sensible literal default / property widget),
/// so ParamValue deliberately omits the image alternatives.
using ParamValue = std::variant<i64, f64, bool, std::string>;

/// True if a value of @p from may feed an input pin of @p to.  Exact-match,
/// except `Number` interconnects with Int, Real, and R (single channel) — the
/// polymorphic numeric/channel type arithmetic nodes use.
constexpr bool pinTypesCompatible(PinType from, PinType to) noexcept {
    if (from == to)
        return true;
    const auto numberLike = [](PinType t) {
        return t == PinType::Int || t == PinType::Real || t == PinType::R || t == PinType::Number;
    };
    if (to == PinType::Number)
        return numberLike(from);
    if (from == PinType::Number)
        return numberLike(to);
    return false;
}

/// Human/JSON token for a pin type (stable; used in serialization & tooltips).
constexpr const char* pinTypeName(PinType t) noexcept {
    switch (t) {
    case PinType::RGBA: return "rgba";
    case PinType::RGB: return "rgb";
    case PinType::R: return "r";
    case PinType::Int: return "int";
    case PinType::Real: return "real";
    case PinType::Bool: return "bool";
    case PinType::String: return "string";
    case PinType::Number: return "number";
    }
    return "unknown";
}

/// One pin on a node.  Identified within its node by a stable @c name (used by
/// links and JSON) — never by index, so adding pins stays forward-compatible.
struct Pin {
    std::string name;
    PinType type;
    bool is_input;
};

} // namespace whiteout::textool::pipeline
