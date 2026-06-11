// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/pipeline_executor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "common_types.h"
#include "pipeline/serialization.h"
#include "services/texture_service.h"

namespace whiteout::textool::services {

namespace tex = whiteout::textures;
using pipeline::Link;
using pipeline::Node;
using pipeline::NodeCategory;
using pipeline::NodeGraph;
using pipeline::NodeId;
using pipeline::Param;

namespace {

// A single-channel plane in floating point — used to carry channel arithmetic
// at full precision (and signed), quantized to R8 only at image boundaries.
struct FChannel {
    u32 w = 0, h = 0;
    std::vector<float> data;
};

// ── Runtime pin value ───────────────────────────────────────────────────────
// Images (RGBA / R) flow as Textures; channel arithmetic results as FChannel;
// Int/Real pins as i64/f64.  A node reads its inputs from upstream outputs and
// writes its outputs here.
struct PinData {
    std::optional<tex::Texture> image;
    std::optional<FChannel> fchan;
    std::optional<i64> integer;
    std::optional<f64> real;
};
using PinMap = std::unordered_map<std::string, PinData>;

// Execution environment threaded through node application.
struct ExecEnv {
    std::filesystem::path presets_dir;
    std::filesystem::path pipelines_dir;
    TextureService& ts;
    int depth = 0;                         // Subpipeline / local-call nesting depth.
    const NodeGraph* graph = nullptr;      // The graph currently being run (for local.call).
};

// Bounds recursion (a pipeline may reference itself directly or indirectly).
// High enough for practical recursive functions, low enough to keep the native
// call stack safe.
constexpr int kMaxSubpipelineDepth = 64;

// Run a graph (or a node subset) binding its named external ports: @p inputs
// maps port name (an Input node's "name" param) -> value; the return maps Output
// port name -> value.  @p members (nullptr = whole graph) restricts execution to
// a subset (a Local Pipeline frame's members); links with a non-member endpoint
// are ignored.  Defined after applyNode; forward-declared for Sub/Local calls.
PinMap runGraphPortsImpl(const NodeGraph& graph, const std::unordered_set<NodeId>* members,
                         const PinMap& inputs, const ExecEnv& env,
                         std::vector<std::string>& errors);
// Top-level run: like runGraphPortsImpl over the whole graph, but EXCLUDES nodes
// that live inside any Local Pipeline frame — those execute only when their frame
// is invoked via local.call (Frame/Call nodes themselves stay, transparent).
inline PinMap runGraphPorts(const NodeGraph& graph, const PinMap& inputs, const ExecEnv& env,
                            std::vector<std::string>& errors) {
    std::unordered_set<NodeId> framed;
    for (const auto& up : graph.nodes())
        if (up->typeId() == "frame.local")
            for (NodeId id : graph.nodesInFrame(*up))
                framed.insert(id);
    if (framed.empty())
        return runGraphPortsImpl(graph, nullptr, inputs, env, errors);
    std::unordered_set<NodeId> top;
    for (const auto& up : graph.nodes())
        if (framed.find(up->id()) == framed.end())
            top.insert(up->id());
    return runGraphPortsImpl(graph, &top, inputs, env, errors);
}

// ── Param helpers (const view over a node's params) ─────────────────────────
const Param* findParam(const Node& n, std::string_view name) {
    for (const auto& p : n.params())
        if (p.name == name)
            return &p;
    return nullptr;
}
i64 paramInt(const Node& n, std::string_view name, i64 def) {
    const Param* p = findParam(n, name);
    if (p && std::holds_alternative<i64>(p->value))
        return std::get<i64>(p->value);
    return def;
}
f64 paramReal(const Node& n, std::string_view name, f64 def) {
    const Param* p = findParam(n, name);
    if (p && std::holds_alternative<f64>(p->value))
        return std::get<f64>(p->value);
    return def;
}
bool paramBool(const Node& n, std::string_view name, bool def) {
    const Param* p = findParam(n, name);
    if (p && std::holds_alternative<bool>(p->value))
        return std::get<bool>(p->value);
    return def;
}
std::string paramStr(const Node& n, std::string_view name) {
    const Param* p = findParam(n, name);
    if (p && std::holds_alternative<std::string>(p->value))
        return std::get<std::string>(p->value);
    return {};
}

// ── Pixel helpers (operate on RGBA8 / R8 textures) ──────────────────────────
tex::Texture toRGBA8(const tex::Texture& t) {
    return t.format() == tex::PixelFormat::RGBA8 ? t : t.copyAsFormat(tex::PixelFormat::RGBA8);
}

// Invert one channel (0=R..3=A) across every RGBA8 pixel (all mips).
void invertChannel(tex::Texture& rgba8, int channel) {
    if (channel < 0 || channel > 3)
        return;
    std::span<u8> d = rgba8.data();
    for (std::size_t i = static_cast<std::size_t>(channel); i < d.size(); i += 4)
        d[i] = static_cast<u8>(255 - d[i]);
}

// Invert every byte of an R8 texture (all mips).
void invertR8(tex::Texture& r8) {
    for (u8& b : r8.data())
        b = static_cast<u8>(255 - b);
}

// Extract one channel of an RGBA8 image (mip 0) into a fresh single-mip R8.
tex::Texture extractChannel(const tex::Texture& rgba8, int channel) {
    const u32 w = rgba8.width(), h = rgba8.height();
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::R8, w, h, 1);
    std::span<const u8> src = rgba8.mipData(0);
    std::span<u8> dst = out.mipData(0);
    const int c = std::clamp(channel, 0, 3);
    for (std::size_t p = 0; p < dst.size(); ++p) {
        const std::size_t si = p * 4 + static_cast<std::size_t>(c);
        dst[p] = si < src.size() ? src[si] : 0;
    }
    return out;
}

// Blend b over a (both RGBA8, mip 0, same dims) per a WC3-style mode.
tex::Texture blend(const tex::Texture& a, const tex::Texture& b, i64 mode) {
    const u32 w = a.width(), h = a.height();
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, w, h, 1);
    std::span<const u8> pa = a.mipData(0), pb = b.mipData(0);
    std::span<u8> po = out.mipData(0);
    const auto clamp8 = [](int v) { return static_cast<u8>(std::clamp(v, 0, 255)); };
    for (std::size_t i = 0; i + 3 < po.size(); i += 4) {
        const int ar = pa[i], ag = pa[i + 1], ab = pa[i + 2], aa = pa[i + 3];
        const int br = pb[i], bg = pb[i + 1], bb = pb[i + 2], ba = pb[i + 3];
        int r = ar, g = ag, bl = ab, al = aa;
        switch (mode) {
        case 0: // Transparent
        case 1: // Blend (alpha over)
        {
            const float t = ba / 255.0f;
            r = static_cast<int>(ar * (1 - t) + br * t);
            g = static_cast<int>(ag * (1 - t) + bg * t);
            bl = static_cast<int>(ab * (1 - t) + bb * t);
            al = std::max(aa, ba);
            break;
        }
        case 2: // Additive
            r = ar + br;
            g = ag + bg;
            bl = ab + bb;
            al = aa;
            break;
        case 3: // Modulate
            r = ar * br / 255;
            g = ag * bg / 255;
            bl = ab * bb / 255;
            al = aa * ba / 255;
            break;
        case 4: // Modulate 2x
            r = ar * br * 2 / 255;
            g = ag * bg * 2 / 255;
            bl = ab * bb * 2 / 255;
            al = aa * ba * 2 / 255;
            break;
        default: break;
        }
        po[i] = clamp8(r);
        po[i + 1] = clamp8(g);
        po[i + 2] = clamp8(bl);
        po[i + 3] = clamp8(al);
    }
    return out;
}

// Copy one mip level of an RGBA8 image into a fresh single-mip RGBA8 texture.
tex::Texture extractMip(const tex::Texture& rgba8, u32 mip) {
    mip = std::min(mip, rgba8.mipCount() - 1);
    const u32 mw = std::max(1u, rgba8.width() >> mip);
    const u32 mh = std::max(1u, rgba8.height() >> mip);
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, mw, mh, 1);
    std::span<const u8> src = rgba8.mipData(mip);
    std::span<u8> dst = out.mipData(0);
    const std::size_t n = std::min(src.size(), dst.size());
    std::memcpy(dst.data(), src.data(), n);
    return out;
}

// Index of the base mip whose size matches related's top mip (0 if related's
// top mip is larger than base's, or no match).
i64 matchingMip(const tex::Texture& base, const tex::Texture& related) {
    const u32 rw = related.width(), rh = related.height();
    if (rw > base.width() || rh > base.height())
        return 0;
    for (u32 m = 0; m < base.mipCount(); ++m) {
        if (std::max(1u, base.width() >> m) == rw && std::max(1u, base.height() >> m) == rh)
            return static_cast<i64>(m);
    }
    return 0;
}

// Combine four single-channel (R8) planes into one RGBA8 image (mip 0).  Dims
// come from the first connected channel; missing channels default to 0 (alpha
// to 255).
tex::Texture mergeChannels(const tex::Texture* r, const tex::Texture* g, const tex::Texture* b,
                           const tex::Texture* a) {
    const tex::Texture* ref = r ? r : (g ? g : (b ? b : a));
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, ref->width(), ref->height(),
                                              1);
    std::span<u8> dst = out.mipData(0);
    const auto sample = [](const tex::Texture* t, std::size_t p, u8 def) -> u8 {
        if (!t)
            return def;
        std::span<const u8> s = t->mipData(0);
        return p < s.size() ? s[p] : def;
    };
    for (std::size_t p = 0; p * 4 + 3 < dst.size(); ++p) {
        dst[p * 4 + 0] = sample(r, p, 0);
        dst[p * 4 + 1] = sample(g, p, 0);
        dst[p * 4 + 2] = sample(b, p, 0);
        dst[p * 4 + 3] = sample(a, p, 255);
    }
    return out;
}

// Broadcast a single channel (R8) to a grayscale RGBA8 image (mip 0).
tex::Texture prims(const tex::Texture& ch) {
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, ch.width(), ch.height(), 1);
    std::span<const u8> s = ch.mipData(0);
    std::span<u8> d = out.mipData(0);
    for (std::size_t p = 0; p * 4 + 3 < d.size(); ++p) {
        const u8 v = p < s.size() ? s[p] : 0;
        d[p * 4 + 0] = d[p * 4 + 1] = d[p * 4 + 2] = v;
        d[p * 4 + 3] = 255;
    }
    return out;
}

// Compute luma of an RGBA8 image into a single-channel R8 (mip 0).
// method: 0 = Rec.709, 1 = Rec.601, 2 = Average.
tex::Texture luma(const tex::Texture& rgba8, i64 method) {
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::R8, rgba8.width(), rgba8.height(),
                                              1);
    std::span<const u8> s = rgba8.mipData(0);
    std::span<u8> d = out.mipData(0);
    for (std::size_t p = 0; p < d.size(); ++p) {
        const std::size_t i = p * 4;
        if (i + 2 >= s.size()) {
            d[p] = 0;
            continue;
        }
        const float r = s[i], g = s[i + 1], b = s[i + 2];
        float y;
        switch (method) {
        case 1: y = 0.299f * r + 0.587f * g + 0.114f * b; break;  // Rec.601
        case 2: y = (r + g + b) / 3.0f; break;                    // Average
        default: y = 0.2126f * r + 0.7152f * g + 0.0722f * b;     // Rec.709
        }
        d[p] = static_cast<u8>(std::clamp(static_cast<int>(y + 0.5f), 0, 255));
    }
    return out;
}

// ── Arithmetic (Number = int / real / single channel) ──────────────────────
u8 clamp8d(double v) {
    return static_cast<u8>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
}
double scalarOf(const PinData& d) {
    if (d.integer)
        return static_cast<double>(*d.integer);
    if (d.real)
        return *d.real;
    return 0.0;
}
// Coerce a pin value to a 64-bit integer for bitwise ops (reals are rounded).
i64 intOf(const PinData& d) {
    if (d.integer)
        return *d.integer;
    if (d.real)
        return static_cast<i64>(std::llround(*d.real));
    return 0;
}
// Evaluate a comparator (index matches kComparators): 0=Eq,1=Ne,2=Lt,3=Le,4=Gt,
// 5=Ge.  Equality uses an epsilon tolerance so float operands compare sensibly.
bool evalCompare(i64 cmp, double av, double bv, double eps) {
    switch (cmp) {
    case 0: return std::abs(av - bv) <= eps; // Equal
    case 1: return std::abs(av - bv) > eps;  // Not equal
    case 2: return av < bv;                  // Less
    case 3: return av <= bv;                 // Less or equal
    case 4: return av > bv;                  // Greater
    default: return av >= bv;                // Greater or equal
    }
}
// A pin's scalar value, or 0 if the pin is absent.
double scalarAt(const PinMap& in, const char* pin) {
    const auto it = in.find(pin);
    return it != in.end() ? scalarOf(it->second) : 0.0;
}

bool isChannelLike(const PinData& d) {
    return d.image.has_value() || d.fchan.has_value();
}

// Read a pin's channel as a NORMALIZED float plane: R8 bytes [0,255] -> [0,1].
// Channel arithmetic works in normalized space so constants like 1.0 mean
// full-scale (matching how channel math is normally expressed).
FChannel toFChannel(const PinData& d) {
    if (d.fchan)
        return *d.fchan;
    FChannel f;
    if (d.image) {
        f.w = d.image->width();
        f.h = d.image->height();
        std::span<const u8> s = d.image->mipData(0);
        f.data.resize(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            f.data[i] = s[i] / 255.0f;
    }
    return f;
}

// Quantize a normalized float plane ([0,1]) back to an R8 texture (* 255).
tex::Texture fchanToR8(const FChannel& f) {
    const u32 w = std::max(1u, f.w), h = std::max(1u, f.h);
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::R8, w, h, 1);
    std::span<u8> d = out.mipData(0);
    for (std::size_t i = 0; i < d.size(); ++i)
        d[i] = i < f.data.size() ? clamp8d(f.data[i] * 255.0f) : 0;
    return out;
}

// Fetch an input channel as an R8 texture from either an image pin or a float
// plane (lets channel-consuming ops accept arithmetic results).
std::optional<tex::Texture> channelR8(const PinMap& in, const char* pin) {
    const auto it = in.find(pin);
    if (it == in.end())
        return std::nullopt;
    if (it->second.image) {
        const tex::Texture& t = *it->second.image;
        return t.format() == tex::PixelFormat::R8 ? t : t.copyAsFormat(tex::PixelFormat::R8);
    }
    if (it->second.fchan)
        return fchanToR8(*it->second.fchan);
    return std::nullopt;
}

enum class ArithOp { Add, Mul, Min, Max };

PinData binaryArith(const PinData& a, const PinData& b, ArithOp op,
                    std::vector<std::string>& errors) {
    PinData out;
    // Generic over float (channels) and int/double (scalars); each call site
    // passes matching operand types.
    const auto apply = [op](auto x, auto y) -> decltype(x + y) {
        switch (op) {
        case ArithOp::Add: return x + y;
        case ArithOp::Mul: return x * y;
        case ArithOp::Min: return x < y ? x : y;
        case ArithOp::Max: return x > y ? x : y;
        }
        return x;
    };
    const bool aCh = isChannelLike(a), bCh = isChannelLike(b);

    // Channel arithmetic runs in float (signed, full precision); it is only
    // quantized to bytes at an image boundary (Merge/Prims/output).
    if (aCh && bCh) { // channel ⊕ channel, element-wise (same size required)
        FChannel fa = toFChannel(a), fb = toFChannel(b);
        if (fa.w != fb.w || fa.h != fb.h) {
            errors.push_back("Arithmetic: channels differ in size; passing first through");
            out.fchan = std::move(fa);
            return out;
        }
        for (std::size_t i = 0; i < fa.data.size(); ++i)
            fa.data[i] = apply(fa.data[i], i < fb.data.size() ? fb.data[i] : 0.0f);
        out.fchan = std::move(fa);
    } else if (aCh || bCh) { // channel ⊕ number, per element
        FChannel f = toFChannel(aCh ? a : b);
        const float num = static_cast<float>(scalarOf(aCh ? b : a));
        for (float& v : f.data)
            v = apply(v, num);
        out.fchan = std::move(f);
    } else if (a.integer && b.integer) { // int ⊕ int -> int
        out.integer = apply(*a.integer, *b.integer);
    } else { // any real involved -> real
        out.real = apply(scalarOf(a), scalarOf(b));
    }
    return out;
}

enum class UnaryOp { Negate, Sqrt, Reciprocal };

PinData unaryArith(const PinData& a, UnaryOp op, std::vector<std::string>&) {
    PinData out;
    const auto fn = [op](float x) -> float {
        switch (op) {
        case UnaryOp::Negate: return -x;
        case UnaryOp::Sqrt: return std::sqrt(std::max(0.0f, x));
        case UnaryOp::Reciprocal: return x != 0.0f ? 1.0f / x : 0.0f;
        }
        return x;
    };
    if (isChannelLike(a)) { // element-wise on the channel, in float
        FChannel f = toFChannel(a);
        for (float& v : f.data)
            v = fn(v);
        out.fchan = std::move(f);
    } else if (a.integer && op == UnaryOp::Negate) {
        out.integer = -*a.integer; // negate keeps integers integral
    } else {
        out.real = fn(static_cast<float>(scalarOf(a))); // sqrt / reciprocal -> real
    }
    return out;
}

// ── Convolution filters (operate on RGBA8 mip 0, edge-clamped) ─────────────
// Separable Gaussian blur with the given radius (sigma ≈ radius/2).
tex::Texture gaussianBlur(const tex::Texture& rgba8, int radius) {
    radius = std::clamp(radius, 1, 16);
    const int w = static_cast<int>(rgba8.width()), h = static_cast<int>(rgba8.height());
    const double sigma = std::max(0.5, radius / 2.0);
    std::vector<double> k(2 * radius + 1);
    double ksum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-(i * i) / (2.0 * sigma * sigma));
        k[i + radius] = v;
        ksum += v;
    }
    for (double& v : k)
        v /= ksum;

    std::span<const u8> src = rgba8.mipData(0);
    std::vector<u8> tmp(src.begin(), src.end());
    const auto idx = [w](int x, int y, int c) { return (static_cast<std::size_t>(y) * w + x) * 4 + c; };

    // Horizontal pass: src -> tmp.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                double acc = 0.0;
                for (int i = -radius; i <= radius; ++i)
                    acc += k[i + radius] * src[idx(std::clamp(x + i, 0, w - 1), y, c)];
                tmp[idx(x, y, c)] = clamp8d(acc);
            }

    // Vertical pass: tmp -> out.
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, w, h, 1);
    std::span<u8> d = out.mipData(0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                double acc = 0.0;
                for (int i = -radius; i <= radius; ++i)
                    acc += k[i + radius] * tmp[idx(x, std::clamp(y + i, 0, h - 1), c)];
                d[idx(x, y, c)] = clamp8d(acc);
            }
    return out;
}

// Unsharp 3x3 sharpen on RGB (alpha preserved).  center = 1+4s, neighbours = -s.
tex::Texture sharpen(const tex::Texture& rgba8, double strength) {
    const int w = static_cast<int>(rgba8.width()), h = static_cast<int>(rgba8.height());
    std::span<const u8> s = rgba8.mipData(0);
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::RGBA8, w, h, 1);
    std::span<u8> d = out.mipData(0);
    const auto at = [&](int x, int y, int c) {
        return static_cast<double>(s[(static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w +
                                      std::clamp(x, 0, w - 1)) *
                                         4 +
                                     c]);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const double v = (1.0 + 4.0 * strength) * at(x, y, c) - strength * at(x - 1, y, c) -
                                 strength * at(x + 1, y, c) - strength * at(x, y - 1, c) -
                                 strength * at(x, y + 1, c);
                d[i + c] = clamp8d(v);
            }
            d[i + 3] = s[i + 3]; // preserve alpha
        }
    return out;
}

// Sobel edge magnitude on a single channel (R8 in/out).
tex::Texture sobel(const tex::Texture& r8) {
    const int w = static_cast<int>(r8.width()), h = static_cast<int>(r8.height());
    std::span<const u8> s = r8.mipData(0); // R8: 1 byte per pixel
    const auto at = [&](int x, int y) {
        return static_cast<double>(
            s[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]);
    };
    tex::Texture out = tex::Texture::create2D(tex::PixelFormat::R8, w, h, 1);
    std::span<u8> d = out.mipData(0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double gx = -at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1) +
                              at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1);
            const double gy = -at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1) +
                              at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1);
            d[static_cast<std::size_t>(y) * w + x] = clamp8d(std::sqrt(gx * gx + gy * gy));
        }
    return out;
}

enum class DerivMode { Quad = 0, Sobel = 1 };

// Partial derivatives of a single channel, in NORMALIZED float ([0,1] input).
// Output is a SIGNED gradient (0 = flat, >0 rising, <0 falling) carried as a
// float plane so the sign survives downstream arithmetic (essential for
// normal-map math).  Quad = central difference (2-tap); Sobel = 3x3 kernel,
// scaled by 1/8 so its flat-ramp response matches Quad.
std::pair<FChannel, FChannel> derivativesF(const FChannel& src, DerivMode mode) {
    const int w = std::max(1, static_cast<int>(src.w)), h = std::max(1, static_cast<int>(src.h));
    const auto at = [&](int x, int y) -> float {
        const std::size_t i = static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w +
                              std::clamp(x, 0, w - 1);
        return i < src.data.size() ? src.data[i] : 0.0f;
    };
    FChannel dx, dy;
    dx.w = dy.w = static_cast<u32>(w);
    dx.h = dy.h = static_cast<u32>(h);
    dx.data.resize(static_cast<std::size_t>(w) * h);
    dy.data.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (mode == DerivMode::Sobel) {
                const float tl = at(x - 1, y - 1), l = at(x - 1, y), bl = at(x - 1, y + 1);
                const float t = at(x, y - 1), b = at(x, y + 1);
                const float tr = at(x + 1, y - 1), r = at(x + 1, y), br = at(x + 1, y + 1);
                dx.data[i] = ((tr + 2.0f * r + br) - (tl + 2.0f * l + bl)) * (1.0f / 8.0f);
                dy.data[i] = ((bl + 2.0f * b + br) - (tl + 2.0f * t + tr)) * (1.0f / 8.0f);
            } else {
                dx.data[i] = (at(x + 1, y) - at(x - 1, y)) * 0.5f;
                dy.data[i] = (at(x, y + 1) - at(x, y - 1)) * 0.5f;
            }
        }
    return {std::move(dx), std::move(dy)};
}

// ── Geometric ops (mirror / rotate / scale) ─────────────────────────────────
// They run over an interleaved float buffer so the same code serves images
// (RGBA8 n=4 / R8 n=1) and channels (float plane, n=1).
constexpr float kPi = 3.14159265358979323846f;

struct FBuf {
    int w = 0, h = 0, n = 1;
    std::vector<float> data; // interleaved, length w*h*n
    float at(int x, int y, int c) const {
        return data[(static_cast<std::size_t>(y) * w + x) * n + c];
    }
};

// Pull a pin's image/channel value into a float buffer.  `was_fchan` records a
// float-plane source; `fmt` the image pixel format to rebuild (R8 vs RGBA8).
bool pinToFBuf(const PinData& d, FBuf& b, bool& was_fchan, tex::PixelFormat& fmt) {
    if (d.fchan) {
        was_fchan = true;
        b.w = static_cast<int>(d.fchan->w);
        b.h = static_cast<int>(d.fchan->h);
        b.n = 1;
        b.data = d.fchan->data;
        b.data.resize(static_cast<std::size_t>(std::max(0, b.w)) * std::max(0, b.h), 0.0f);
        return b.w > 0 && b.h > 0;
    }
    if (d.image) {
        was_fchan = false;
        tex::Texture src = *d.image;
        if (src.format() == tex::PixelFormat::R8) {
            fmt = tex::PixelFormat::R8;
            b.n = 1;
        } else {
            src = toRGBA8(src);
            fmt = tex::PixelFormat::RGBA8;
            b.n = 4;
        }
        b.w = static_cast<int>(src.width());
        b.h = static_cast<int>(src.height());
        std::span<const u8> s = src.mipData(0);
        b.data.resize(static_cast<std::size_t>(b.w) * b.h * b.n);
        for (std::size_t i = 0; i < b.data.size(); ++i)
            b.data[i] = i < s.size() ? static_cast<float>(s[i]) : 0.0f;
        return b.w > 0 && b.h > 0;
    }
    return false;
}

PinData fbufToPin(const FBuf& b, bool was_fchan, tex::PixelFormat fmt) {
    PinData out;
    if (was_fchan) {
        FChannel f;
        f.w = static_cast<u32>(std::max(0, b.w));
        f.h = static_cast<u32>(std::max(0, b.h));
        f.data = b.data;
        out.fchan = std::move(f);
        return out;
    }
    tex::Texture t = tex::Texture::create2D(fmt, static_cast<u32>(std::max(1, b.w)),
                                            static_cast<u32>(std::max(1, b.h)), 1);
    std::span<u8> d = t.mipData(0);
    for (std::size_t i = 0; i < d.size(); ++i)
        d[i] = i < b.data.size() ? clamp8d(b.data[i]) : 0;
    out.image = std::move(t);
    return out;
}

FBuf mirrorBuf(const FBuf& s, bool x_axis) { // x_axis: flip horizontally
    FBuf o = s;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            const int sx = x_axis ? s.w - 1 - x : x;
            const int sy = x_axis ? y : s.h - 1 - y;
            for (int c = 0; c < s.n; ++c)
                o.data[(static_cast<std::size_t>(y) * o.w + x) * o.n + c] = s.at(sx, sy, c);
        }
    return o;
}

FBuf rotateBuf(const FBuf& s, float degrees) { // same dims, bilinear, zero outside
    FBuf o;
    o.w = s.w;
    o.h = s.h;
    o.n = s.n;
    o.data.assign(s.data.size(), 0.0f);
    const float rad = degrees * kPi / 180.0f;
    const float ca = std::cos(rad), sa = std::sin(rad);
    const float cx = (s.w - 1) * 0.5f, cy = (s.h - 1) * 0.5f;
    const auto px = [&](int x, int y, int c) -> float {
        return (x < 0 || y < 0 || x >= s.w || y >= s.h) ? 0.0f : s.at(x, y, c);
    };
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            const float dx = x - cx, dy = y - cy;
            const float fx = cx + (ca * dx + sa * dy);
            const float fy = cy + (-sa * dx + ca * dy);
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - x0, ty = fy - y0;
            for (int c = 0; c < o.n; ++c) {
                const float a = px(x0, y0, c), bb = px(x0 + 1, y0, c);
                const float cc = px(x0, y0 + 1, c), dd = px(x0 + 1, y0 + 1, c);
                o.data[(static_cast<std::size_t>(y) * o.w + x) * o.n + c] =
                    a * (1 - tx) * (1 - ty) + bb * tx * (1 - ty) + cc * (1 - tx) * ty +
                    dd * tx * ty;
            }
        }
    return o;
}

enum class ScaleFilter { Bicubic = 0, Linear = 1, Lanczos = 2 };

float filterRadius(ScaleFilter f) {
    return f == ScaleFilter::Linear ? 1.0f : f == ScaleFilter::Bicubic ? 2.0f : 3.0f;
}
float filterWeight(ScaleFilter f, float x) {
    x = std::abs(x);
    switch (f) {
    case ScaleFilter::Linear:
        return x < 1.0f ? 1.0f - x : 0.0f;
    case ScaleFilter::Bicubic: { // Catmull-Rom
        constexpr float a = -0.5f;
        if (x < 1.0f)
            return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
        if (x < 2.0f)
            return (((x - 5.0f) * x + 8.0f) * x - 4.0f) * a;
        return 0.0f;
    }
    case ScaleFilter::Lanczos: { // a = 3
        if (x < 1e-6f)
            return 1.0f;
        if (x >= 3.0f)
            return 0.0f;
        const float pxr = kPi * x;
        return 3.0f * std::sin(pxr) * std::sin(pxr / 3.0f) / (pxr * pxr);
    }
    }
    return 0.0f;
}

// Resample one axis (src dim -> dst dim) with proper kernel widening on shrink.
void resampleAxis(const FBuf& src, FBuf& dst, bool horizontal, ScaleFilter filt) {
    const int srcN = horizontal ? src.w : src.h;
    const int dstN = horizontal ? dst.w : dst.h;
    const int other = horizontal ? src.h : src.w; // unchanged dimension
    const float scale = static_cast<float>(dstN) / static_cast<float>(std::max(1, srcN));
    const float inv = std::min(scale, 1.0f); // <1 widens the kernel when shrinking
    const float support = filterRadius(filt) / inv;

    std::vector<std::pair<int, float>> contrib;
    for (int i = 0; i < dstN; ++i) {
        const float center = (i + 0.5f) / scale - 0.5f;
        const int left = static_cast<int>(std::ceil(center - support));
        const int right = static_cast<int>(std::floor(center + support));
        contrib.clear();
        float wsum = 0.0f;
        for (int k = left; k <= right; ++k) {
            const float w = filterWeight(filt, (center - k) * inv);
            if (w == 0.0f)
                continue;
            contrib.push_back({std::clamp(k, 0, srcN - 1), w});
            wsum += w;
        }
        if (wsum <= 0.0f)
            continue;
        for (int o = 0; o < other; ++o)
            for (int c = 0; c < src.n; ++c) {
                float acc = 0.0f;
                for (const auto& [sk, w] : contrib)
                    acc += (horizontal ? src.at(sk, o, c) : src.at(o, sk, c)) * w;
                const float r = acc / wsum;
                if (horizontal)
                    dst.data[(static_cast<std::size_t>(o) * dst.w + i) * dst.n + c] = r;
                else
                    dst.data[(static_cast<std::size_t>(i) * dst.w + o) * dst.n + c] = r;
            }
    }
}

FBuf resizeBuf(const FBuf& s, int dstW, int dstH, ScaleFilter filt) {
    dstW = std::max(1, dstW);
    dstH = std::max(1, dstH);
    FBuf tmp;
    tmp.w = dstW;
    tmp.h = s.h;
    tmp.n = s.n;
    tmp.data.assign(static_cast<std::size_t>(dstW) * std::max(1, s.h) * s.n, 0.0f);
    resampleAxis(s, tmp, /*horizontal=*/true, filt);
    FBuf out;
    out.w = dstW;
    out.h = dstH;
    out.n = s.n;
    out.data.assign(static_cast<std::size_t>(dstW) * dstH * s.n, 0.0f);
    resampleAxis(tmp, out, /*horizontal=*/false, filt);
    return out;
}

// ── Per-node execution ──────────────────────────────────────────────────────
// Returns this node's output pin values; appends any issues to `errors`.
PinMap applyNode(const Node& n, const PinMap& in, const ExecEnv& env,
                 std::vector<std::string>& errors) {
    const std::string& type = n.typeId();
    PinMap out;

    const auto inImage = [&](const char* pin) -> const tex::Texture* {
        auto it = in.find(pin);
        return (it != in.end() && it->second.image) ? &*it->second.image : nullptr;
    };

    if (type == "input.standard") {
        // Wired by the caller via a pre-seeded output; nothing to do here.
    } else if (type == "input.resource") {
        const std::string rel = paramStr(n, "path");
        if (rel.empty()) {
            errors.push_back("Resource input: no path set");
        } else {
            auto load = env.ts.loadFromFile((env.presets_dir / rel).string());
            if (load.texture)
                out["image"].image = toRGBA8(*load.texture);
            else
                errors.push_back("Resource input: failed to load '" + rel + "'");
        }
    } else if (type == "input.real") {
        double v = paramReal(n, "value", 0.0);
        if (paramBool(n, "clamp", false)) {
            const double mn = paramReal(n, "min", 0.0), mx = paramReal(n, "max", 1.0);
            v = std::clamp(v, std::min(mn, mx), std::max(mn, mx));
        }
        out["value"].real = v;
    } else if (type == "input.integer") {
        i64 v = paramInt(n, "value", 0);
        if (paramBool(n, "clamp", false)) {
            const i64 mn = paramInt(n, "min", 0), mx = paramInt(n, "max", 255);
            v = std::clamp(v, std::min(mn, mx), std::max(mn, mx));
        }
        out["value"].integer = v;
    } else if (type == "input.channel" || type == "input.number") {
        // A function input; no source when run as a standard pipeline.
    } else if (type == "input.const_int") {
        out["value"].integer = paramInt(n, "value", 0);
    } else if (type == "input.const_real") {
        out["value"].real = paramReal(n, "value", 0.0);
    } else if (type == "input.const_channel") {
        int cw = 64, ch = 64;
        double value = 0.0;
        if (const auto it = in.find("width"); it != in.end())
            cw = static_cast<int>(std::lround(scalarOf(it->second)));
        if (const auto it = in.find("height"); it != in.end())
            ch = static_cast<int>(std::lround(scalarOf(it->second)));
        if (const auto it = in.find("value"); it != in.end())
            value = scalarOf(it->second);
        cw = std::clamp(cw, 1, 8192);
        ch = std::clamp(ch, 1, 8192);
        tex::Texture plane = tex::Texture::create2D(tex::PixelFormat::R8, cw, ch, 1);
        std::span<u8> d = plane.mipData(0);
        std::fill(d.begin(), d.end(), clamp8d(value));
        out["channel"].image = std::move(plane);
    } else if (type == "op.extract_channel") {
        if (const tex::Texture* img = inImage("image"))
            out["channel"].image = extractChannel(*img, static_cast<int>(paramInt(n, "channel", 0)));
    } else if (type == "op.invert_channel") {
        if (const tex::Texture* img = inImage("image")) {
            tex::Texture work = toRGBA8(*img);
            invertChannel(work, static_cast<int>(paramInt(n, "channel", 0)));
            out["image"].image = std::move(work);
        }
    } else if (type == "op.fill_channel") {
        if (const tex::Texture* img = inImage("image")) {
            double value = 0.0;
            if (const auto it = in.find("value"); it != in.end())
                value = scalarOf(it->second);
            tex::Texture work = toRGBA8(*img);
            const u8 v = clamp8d(value);
            const int c = std::clamp(static_cast<int>(paramInt(n, "channel", 0)), 0, 3);
            std::span<u8> d = work.data();
            for (std::size_t i = static_cast<std::size_t>(c); i < d.size(); i += 4)
                d[i] = v;
            out["image"].image = std::move(work);
        }
    } else if (type == "op.invert") {
        if (auto ch = channelR8(in, "channel")) {
            invertR8(*ch);
            out["channel"].image = std::move(*ch);
        }
    } else if (type == "op.blend") {
        // blend(bottom, top) composites the top layer over the bottom.
        const tex::Texture* bottom = inImage("bottom layer");
        const tex::Texture* top = inImage("top layer");
        if (bottom && top) {
            if (bottom->width() == top->width() && bottom->height() == top->height())
                out["image"].image =
                    blend(toRGBA8(*bottom), toRGBA8(*top), paramInt(n, "mode", 0));
            else {
                errors.push_back("Blend: layers differ in size; passing bottom through");
                out["image"].image = toRGBA8(*bottom);
            }
        } else if (bottom) {
            out["image"].image = toRGBA8(*bottom);
        } else if (top) {
            out["image"].image = toRGBA8(*top);
        }
    } else if (type == "op.merge_channels") {
        auto r = channelR8(in, "red");
        auto g = channelR8(in, "green");
        auto b = channelR8(in, "blue");
        auto a = channelR8(in, "alpha");
        if (r || g || b || a)
            out["image"].image = mergeChannels(r ? &*r : nullptr, g ? &*g : nullptr,
                                                b ? &*b : nullptr, a ? &*a : nullptr);
    } else if (type == "op.prims") {
        if (auto ch = channelR8(in, "channel"))
            out["image"].image = prims(*ch);
    } else if (type == "op.luma") {
        if (const tex::Texture* img = inImage("image"))
            out["channel"].image = luma(toRGBA8(*img), paramInt(n, "method", 0));
    } else if (type == "op.image_properties") {
        if (const tex::Texture* img = inImage("image")) {
            out["width"].integer = static_cast<i64>(img->width());
            out["height"].integer = static_cast<i64>(img->height());
            out["mipmaps"].integer = static_cast<i64>(img->mipCount());
            out["kind"].integer = static_cast<i64>(img->kind());
            // Per-channel subkinds as ints (TextureKind values); meaningful for
            // Multikind textures, otherwise TextureKind::Other for each.
            out["r kind"].integer = static_cast<i64>(img->channelKind(tex::Channel::R));
            out["g kind"].integer = static_cast<i64>(img->channelKind(tex::Channel::G));
            out["b kind"].integer = static_cast<i64>(img->channelKind(tex::Channel::B));
            out["a kind"].integer = static_cast<i64>(img->channelKind(tex::Channel::A));
        }
    } else if (type == "op.mirror" || type == "op.rotate" || type == "op.scale_to" ||
               type == "op.scale_by") {
        if (const auto it = in.find("input"); it != in.end()) {
            FBuf b;
            bool was_fchan = false;
            tex::PixelFormat fmt = tex::PixelFormat::RGBA8;
            if (pinToFBuf(it->second, b, was_fchan, fmt)) {
                FBuf r;
                if (type == "op.mirror") {
                    r = mirrorBuf(b, paramInt(n, "axis", 0) == 0); // 0=X (horizontal)
                } else if (type == "op.rotate") {
                    double deg = 0.0;
                    if (const auto d = in.find("degrees"); d != in.end())
                        deg = scalarOf(d->second);
                    r = rotateBuf(b, static_cast<float>(deg));
                } else {
                    const auto filt = static_cast<ScaleFilter>(paramInt(n, "filter", 0));
                    int dw = b.w, dh = b.h;
                    if (type == "op.scale_to") {
                        if (const auto p = in.find("width"); p != in.end())
                            dw = static_cast<int>(scalarOf(p->second));
                        if (const auto p = in.find("height"); p != in.end())
                            dh = static_cast<int>(scalarOf(p->second));
                    } else { // op.scale_by — factor: 1.0 = unchanged, 1.25 = 125%, 0.5 = half
                        double factor = 1.0;
                        if (const auto p = in.find("factor"); p != in.end())
                            factor = scalarOf(p->second);
                        dw = static_cast<int>(std::lround(b.w * factor));
                        dh = static_cast<int>(std::lround(b.h * factor));
                    }
                    r = resizeBuf(b, dw, dh, filt);
                }
                out["output"] = fbufToPin(r, was_fchan, fmt);
            }
        }
    } else if (type == "op.set_kind") {
        if (const tex::Texture* img = inImage("image")) {
            tex::Texture work = toRGBA8(*img); // copyAsFormat preserves the kind
            if (const auto it = in.find("kind"); it != in.end() && it->second.integer) {
                const i64 k = *it->second.integer;
                // Valid top-level kinds are Other..Multikind; Unused is per-channel only.
                if (k >= 0 && k <= static_cast<i64>(tex::TextureKind::Multikind))
                    work.setKind(static_cast<tex::TextureKind>(k));
                else
                    errors.push_back("Set Kind: invalid kind value (ignored)");
            }
            out["image"].image = std::move(work);
        }
    } else if (type == "input.const_kind") {
        out["value"].integer = paramInt(n, "kind", 0); // enum index == TextureKind value
    } else if (type == "op.add" || type == "op.multiply" || type == "op.min" ||
               type == "op.max") {
        const auto ia = in.find("a");
        const auto ib = in.find("b");
        const ArithOp aop = type == "op.add"        ? ArithOp::Add
                            : type == "op.multiply" ? ArithOp::Mul
                            : type == "op.min"      ? ArithOp::Min
                                                    : ArithOp::Max;
        if (ia != in.end() && ib != in.end())
            out["result"] = binaryArith(ia->second, ib->second, aop, errors);
        else if (ia != in.end())
            out["result"] = ia->second; // single operand -> passthrough
        else if (ib != in.end())
            out["result"] = ib->second;
    } else if (type == "ctrl.conditional") {
        // Compare the two numeric operands; route `value` to true/false output.
        const bool cond = evalCompare(paramInt(n, "comparator", 0), scalarAt(in, "a"),
                                      scalarAt(in, "b"), std::abs(paramReal(n, "epsilon", 1e-6)));
        if (const auto it = in.find("value"); it != in.end())
            out[cond ? "true" : "false"] = it->second;
    } else if (type == "ctrl.select") {
        // Pick `if true` or `if false` based on comparing the two operands.
        const bool cond = evalCompare(paramInt(n, "comparator", 0), scalarAt(in, "a"),
                                      scalarAt(in, "b"), std::abs(paramReal(n, "epsilon", 1e-6)));
        if (const auto it = in.find(cond ? "if true" : "if false"); it != in.end())
            out["result"] = it->second;
    } else if (type == "ctrl.rendezvous") {
        // Join diverged branches: emit whichever input carries a value (only one
        // branch of a Conditional is live; the inactive branch produces nothing).
        if (const auto it = in.find("a"); it != in.end())
            out["result"] = it->second;
        else if (const auto it = in.find("b"); it != in.end())
            out["result"] = it->second;
    } else if (type == "op.bit_and" || type == "op.bit_or" || type == "op.bit_xor" ||
               type == "op.bit_shl" || type == "op.bit_shr") {
        const auto ia = in.find("a");
        const auto ib = in.find("b");
        const auto iv = in.find("value");
        const auto is = in.find("amount");
        if (type == "op.bit_shl" || type == "op.bit_shr") {
            if (iv != in.end()) {
                const i64 v = intOf(iv->second);
                const i64 amt = std::clamp<i64>(is != in.end() ? intOf(is->second) : 0, 0, 63);
                out["result"].integer = type == "op.bit_shl" ? (v << amt) : (v >> amt);
            }
        } else if (ia != in.end() && ib != in.end()) {
            const i64 a = intOf(ia->second), b = intOf(ib->second);
            out["result"].integer = type == "op.bit_and"  ? (a & b)
                                    : type == "op.bit_or" ? (a | b)
                                                          : (a ^ b);
        }
    } else if (type == "op.bit_not") {
        if (const auto it = in.find("value"); it != in.end())
            out["result"].integer = ~intOf(it->second);
    } else if (type == "op.negate" || type == "op.sqrt" || type == "op.reciprocal") {
        if (const auto it = in.find("value"); it != in.end()) {
            const UnaryOp uo = type == "op.negate"  ? UnaryOp::Negate
                               : type == "op.sqrt"  ? UnaryOp::Sqrt
                                                    : UnaryOp::Reciprocal;
            out["result"] = unaryArith(it->second, uo, errors);
        }
    } else if (type == "op.derivatives") {
        if (const auto it = in.find("channel"); it != in.end()) {
            const auto mode = static_cast<DerivMode>(paramInt(n, "mode", 0));
            auto [dx, dy] = derivativesF(toFChannel(it->second), mode);
            out["dx"].fchan = std::move(dx);
            out["dy"].fchan = std::move(dy);
        }
    } else if (type == "op.gaussian_blur") {
        if (const tex::Texture* img = inImage("image")) {
            double radius = 2.0; // default when the radius pin is unconnected
            if (const auto it = in.find("radius"); it != in.end())
                radius = scalarOf(it->second);
            out["image"].image =
                gaussianBlur(toRGBA8(*img), static_cast<int>(std::lround(radius)));
        }
    } else if (type == "op.sharpen") {
        if (const tex::Texture* img = inImage("image")) {
            double strength = 1.0; // default when the strength pin is unconnected
            if (const auto it = in.find("strength"); it != in.end())
                strength = scalarOf(it->second);
            out["image"].image = sharpen(toRGBA8(*img), strength);
        }
    } else if (type == "op.sobel") {
        if (auto c8 = channelR8(in, "channel"))
            out["channel"].image = sobel(*c8);
    } else if (type == "op.matching_mipmap") {
        const tex::Texture* base = inImage("base");
        const tex::Texture* rel = inImage("related");
        if (base && rel)
            out["mipmap"].integer = matchingMip(*base, *rel);
    } else if (type == "op.extract_mipmap") {
        if (const tex::Texture* img = inImage("image")) {
            i64 mip = 0;
            if (auto it = in.find("mipmap"); it != in.end() && it->second.integer)
                mip = *it->second.integer;
            out["image"].image = extractMip(toRGBA8(*img), static_cast<u32>(std::max<i64>(0, mip)));
        }
    } else if (type == "op.downscale") {
        if (const tex::Texture* img = inImage("image")) {
            tex::Texture work = toRGBA8(*img);
            const u32 levels = static_cast<u32>(paramInt(n, "factor", 0) + 1); // 1/2,1/4,1/8
            auto r = env.ts.downscale(work, levels);
            if (!r.success)
                errors.push_back("Downscale: " + r.message);
            out["image"].image = std::move(work);
        }
    } else if (type == "op.regenerate_mipmaps") {
        if (const tex::Texture* img = inImage("image")) {
            tex::Texture work = toRGBA8(*img);
            const i64 mode = paramInt(n, "mode", 0);
            u32 mips = work.mipCount();          // Current
            if (mode == 1)                       // Custom
                mips = static_cast<u32>(std::max<i64>(1, paramInt(n, "count", 1)));
            else if (mode == 2)                  // Maximum
                mips = 0;                        // 0 = full chain
            auto r = env.ts.regenerateMipmaps(work, mips);
            if (!r.success)
                errors.push_back("Regenerate mipmaps: " + r.message);
            out["image"].image = std::move(work);
        }
    } else if (type == "op.upscale") {
        // The AI upscaler runs asynchronously and needs installed model files;
        // it isn't executed inline yet — pass the image through unchanged.
        if (const tex::Texture* img = inImage("image"))
            out["image"].image = toRGBA8(*img);
        errors.push_back("Upscale: AI upscaling is not run inside pipelines yet (passed through)");
    } else if (type == "op.subpipeline") {
        // The node's pins mirror the selected pipeline's interface, so its input
        // pin names ARE the sub-pipeline's input port names — pass them straight
        // through, and copy each captured output port back onto the matching pin.
        const std::string name = paramStr(n, "pipeline");
        // Echo any "image" input on failure so a broken Subpipeline is a no-op.
        const auto passthrough = [&] {
            if (auto it = in.find("image"); it != in.end())
                out["image"] = it->second;
        };
        // Skip when any input pin lacks a value — e.g. this node sits on the
        // inactive branch of a Conditional.  This is what lets a recursive
        // pipeline terminate: at the base case the recursive call's inputs are
        // routed away, so the Subpipeline simply produces nothing instead of
        // recursing forever (route every recursive input through the branch).
        bool all_inputs_live = true;
        for (const auto& p : n.inputs())
            if (in.find(p.name) == in.end()) {
                all_inputs_live = false;
                break;
            }
        if (!n.inputs().empty() && !all_inputs_live) {
            // Inactive branch: produce nothing.
        } else if (name.empty()) {
            errors.push_back("Subpipeline: no pipeline selected (passed through)");
            passthrough();
        } else if (env.depth >= kMaxSubpipelineDepth) {
            errors.push_back("Subpipeline: nesting too deep (passed through)");
            passthrough();
        } else {
            std::ifstream f(env.pipelines_dir / name, std::ios::binary);
            nlohmann::json doc;
            bool loaded = false;
            if (f) {
                try {
                    f >> doc;
                    loaded = true;
                } catch (const nlohmann::json::exception&) {
                }
            }
            pipeline::NodeGraph sub;
            if (loaded && pipeline::fromJson(doc, sub, &errors)) {
                // subenv.graph = &sub so any local.call inside the file resolves
                // its frame against the sub-graph, not the parent.
                const ExecEnv subenv{env.presets_dir, env.pipelines_dir, env.ts, env.depth + 1,
                                     &sub};
                std::vector<std::string> sub_errors;
                // Bind POSITIONALLY: this node's pins were derived from the sub's
                // interface in order, so input pin i feeds the sub's i-th input
                // port and the sub's i-th output port drives output pin i.  This
                // stays correct even if the referenced pipeline's ports were
                // renamed after the node captured its pins (only order matters).
                const pipeline::PipelineInterface iface = sub.interface();
                PinMap bound;
                const auto node_ins = n.inputs();
                for (std::size_t i = 0; i < node_ins.size() && i < iface.inputs.size(); ++i)
                    if (const auto it = in.find(node_ins[i].name); it != in.end())
                        bound[iface.inputs[i].name] = it->second;

                PinMap sub_out = runGraphPorts(sub, bound, subenv, sub_errors);
                for (auto& e : sub_errors)
                    errors.push_back("Subpipeline '" + name + "': " + e);

                const auto node_outs = n.outputs();
                for (std::size_t i = 0; i < node_outs.size() && i < iface.outputs.size(); ++i)
                    if (const auto it = sub_out.find(iface.outputs[i].name); it != sub_out.end())
                        out[node_outs[i].name] = std::move(it->second);
            } else {
                errors.push_back("Subpipeline: could not load '" + name + "' (passed through)");
                passthrough();
            }
        }
    } else if (type == "local.call") {
        // Invoke a Local Pipeline frame from the SAME graph: run the subset of
        // member nodes, binding ports positionally — mirrors op.subpipeline.
        const std::string frame_name = paramStr(n, "frame");
        const auto passthrough = [&] {
            if (auto it = in.find("image"); it != in.end())
                out["image"] = it->second;
        };
        // Skip on a dead branch (any input pin missing) so recursion terminates.
        bool all_inputs_live = true;
        for (const auto& p : n.inputs())
            if (in.find(p.name) == in.end()) {
                all_inputs_live = false;
                break;
            }
        const Node* frame = nullptr;
        if (env.graph)
            for (const auto& up : env.graph->nodes())
                if (up->typeId() == "frame.local" && paramStr(*up, "name") == frame_name) {
                    frame = up.get();
                    break;
                }
        if (!n.inputs().empty() && !all_inputs_live) {
            // Inactive branch: produce nothing.
        } else if (!env.graph || frame == nullptr) {
            errors.push_back("Local call: frame '" + frame_name + "' not found (passed through)");
            passthrough();
        } else if (env.depth >= kMaxSubpipelineDepth) {
            errors.push_back("Local call: nesting too deep (passed through)");
            passthrough();
        } else {
            const std::vector<NodeId> member_list = env.graph->nodesInFrame(*frame);
            const std::unordered_set<NodeId> members(member_list.begin(), member_list.end());
            const pipeline::PipelineInterface iface = env.graph->localInterface(*frame);
            const ExecEnv subenv{env.presets_dir, env.pipelines_dir, env.ts, env.depth + 1,
                                 env.graph};
            std::vector<std::string> sub_errors;
            PinMap bound;
            const auto node_ins = n.inputs();
            for (std::size_t i = 0; i < node_ins.size() && i < iface.inputs.size(); ++i)
                if (const auto it = in.find(node_ins[i].name); it != in.end())
                    bound[iface.inputs[i].name] = it->second;

            PinMap sub_out = runGraphPortsImpl(*env.graph, &members, bound, subenv, sub_errors);
            for (auto& e : sub_errors)
                errors.push_back("Local '" + frame_name + "': " + e);

            const auto node_outs = n.outputs();
            for (std::size_t i = 0; i < node_outs.size() && i < iface.outputs.size(); ++i)
                if (const auto it = sub_out.find(iface.outputs[i].name); it != sub_out.end())
                    out[node_outs[i].name] = std::move(it->second);
        }
    } else if (type == "frame.local") {
        // A canvas container; inert (its member nodes form a callable pipeline).
    } else if (type == "output.standard") {
        // Captured by the caller from this node's input.
    } else if (type == "output.real" || type == "output.integer" || type == "output.channel" ||
               type == "output.number") {
        // Function-pipeline outputs; inert when run as a standard pipeline.
    } else {
        errors.push_back("Unknown node type '" + type + "' skipped");
    }

    return out;
}

// The "name" param of an interface (Input/Output) node, or empty if absent.
std::optional<std::string> portNameOf(const Node& n) {
    const Param* p = findParam(n, "name");
    if (p && std::holds_alternative<std::string>(p->value))
        return std::get<std::string>(p->value);
    return std::nullopt;
}

PinMap runGraphPortsImpl(const NodeGraph& graph, const std::unordered_set<NodeId>* members,
                         const PinMap& inputs, const ExecEnv& env,
                         std::vector<std::string>& errors) {
    PinMap captured; // Output-port name -> value.

    const auto isMember = [&](NodeId id) { return !members || members->count(id) != 0; };
    // A link participates only when BOTH endpoints are in scope; boundary-
    // crossing wires (one endpoint outside a frame's members) are ignored.
    const auto linkActive = [&](const Link& l) {
        return isMember(l.from.node) && isMember(l.to.node);
    };

    // Per-node outputs, keyed by node id then pin name.
    std::unordered_map<NodeId, PinMap> outputs;

    // Kahn topological order over the (in-scope) link DAG.
    std::unordered_map<NodeId, int> indeg;
    std::size_t node_count = 0;
    for (const auto& up : graph.nodes())
        if (isMember(up->id())) {
            indeg[up->id()] = 0;
            ++node_count;
        }
    for (const Link& l : graph.links())
        if (linkActive(l))
            ++indeg[l.to.node];

    std::deque<NodeId> ready;
    for (const auto& [id, d] : indeg)
        if (d == 0)
            ready.push_back(id);

    std::size_t processed = 0;
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        ++processed;
        const Node* n = graph.node(id);
        if (!n)
            continue;

        // Gather this node's inputs by following in-scope links into its pins.
        PinMap in;
        for (const Link& l : graph.links()) {
            if (l.to.node != id || !linkActive(l))
                continue;
            auto src = outputs.find(l.from.node);
            if (src == outputs.end())
                continue;
            auto val = src->second.find(l.from.pin);
            if (val != src->second.end())
                in[l.to.pin] = val->second;
        }

        // Named Input/Output nodes are the external interface: an Input emits
        // its bound value (or its own default), an Output captures by port name.
        // Everything else (resource/const sources included) is normal node work.
        const std::optional<std::string> port = portNameOf(*n);
        if (port && n->category() == NodeCategory::Input && !n->outputs().empty()) {
            if (auto it = inputs.find(*port); it != inputs.end()) {
                PinMap o;
                o[n->outputs().front().name] = it->second;
                outputs[id] = std::move(o);
            } else {
                outputs[id] = applyNode(*n, in, env, errors); // default value, if any
            }
        } else if (port && n->category() == NodeCategory::Output && !n->inputs().empty()) {
            if (auto it = in.find(n->inputs().front().name); it != in.end())
                captured[*port] = it->second;
            else
                errors.push_back("Output '" + *port + "' has no connected value");
            outputs[id] = {};
        } else {
            outputs[id] = applyNode(*n, in, env, errors);
        }

        for (const Link& l : graph.links()) {
            if (l.from.node != id || !linkActive(l))
                continue;
            if (--indeg[l.to.node] == 0)
                ready.push_back(l.to.node);
        }
    }

    if (processed < node_count)
        errors.push_back("Pipeline has a cycle; some nodes were not executed");

    return captured;
}

} // namespace

PipelineRunResult runStandardPipeline(const NodeGraph& graph, const tex::Texture& input,
                                      const std::filesystem::path& presets_dir,
                                      const std::filesystem::path& pipelines_dir,
                                      TextureService& texture_service, int depth) {
    PipelineRunResult result;
    const ExecEnv env{presets_dir, pipelines_dir, texture_service, depth, &graph};

    // Bind the supplied image to every Standard Input port (a standard pipeline
    // has exactly one).  Ports are keyed by the input node's "name" param.
    PinMap inputs;
    for (const auto& up : graph.nodes()) {
        if (up->typeId() != "input.standard")
            continue;
        PinData d;
        d.image = toRGBA8(input);
        inputs[portNameOf(*up).value_or(std::string{})] = std::move(d);
    }

    PinMap captured = runGraphPorts(graph, inputs, env, result.errors);

    // The standard output is the first Standard Output port's captured image.
    for (const auto& up : graph.nodes()) {
        if (up->typeId() != "output.standard")
            continue;
        if (auto it = captured.find(portNameOf(*up).value_or(std::string{}));
            it != captured.end() && it->second.image) {
            result.output = *it->second.image;
            break;
        }
    }

    if (!result.output)
        result.errors.push_back("Pipeline produced no Standard Output");

    return result;
}

MultiPipelineRunResult runVaryingPipeline(
    const NodeGraph& graph, const std::unordered_map<std::string, tex::Texture>& inputs,
    const std::filesystem::path& presets_dir, const std::filesystem::path& pipelines_dir,
    TextureService& texture_service, int depth) {
    MultiPipelineRunResult result;
    const ExecEnv env{presets_dir, pipelines_dir, texture_service, depth, &graph};

    // Bind each supplied image to its named input port.
    PinMap bound;
    for (const auto& [name, image] : inputs) {
        PinData d;
        d.image = toRGBA8(image);
        bound[name] = std::move(d);
    }

    PinMap captured = runGraphPorts(graph, bound, env, result.errors);

    // Collect every captured image output (skip non-image ports).
    for (auto& [name, data] : captured)
        if (data.image)
            result.outputs.emplace(name, std::move(*data.image));

    return result;
}

} // namespace whiteout::textool::services
