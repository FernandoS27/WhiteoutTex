// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "services/pipeline_executor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common_types.h"
#include "pipeline/serialization.h"
#include "services/texture_service.h"

namespace whiteout::textool::services {

namespace tex = whiteout::textures;
using pipeline::Link;
using pipeline::Node;
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
    int depth = 0; // Subpipeline nesting depth.
};

constexpr int kMaxSubpipelineDepth = 8;

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

enum class ArithOp { Add, Mul };

PinData binaryArith(const PinData& a, const PinData& b, ArithOp op,
                    std::vector<std::string>& errors) {
    PinData out;
    const auto apply = [op](float x, float y) { return op == ArithOp::Add ? x + y : x * y; };
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
        out.integer = op == ArithOp::Add ? (*a.integer + *b.integer) : (*a.integer * *b.integer);
    } else { // any real involved -> real
        out.real = op == ArithOp::Add ? scalarOf(a) + scalarOf(b) : scalarOf(a) * scalarOf(b);
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
    } else if (type == "input.channel") {
        // A function input; no channel source when run as a standard pipeline.
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
    } else if (type == "op.add" || type == "op.multiply") {
        const auto ia = in.find("a");
        const auto ib = in.find("b");
        const ArithOp aop = type == "op.add" ? ArithOp::Add : ArithOp::Mul;
        if (ia != in.end() && ib != in.end())
            out["result"] = binaryArith(ia->second, ib->second, aop, errors);
        else if (ia != in.end())
            out["result"] = ia->second; // single operand -> passthrough
        else if (ib != in.end())
            out["result"] = ib->second;
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
        const tex::Texture* img = inImage("image");
        if (img) {
            const std::string name = paramStr(n, "pipeline");
            tex::Texture in_rgba = toRGBA8(*img);
            if (name.empty()) {
                errors.push_back("Subpipeline: no pipeline selected (passed through)");
                out["image"].image = std::move(in_rgba);
            } else if (env.depth >= kMaxSubpipelineDepth) {
                errors.push_back("Subpipeline: nesting too deep (passed through)");
                out["image"].image = std::move(in_rgba);
            } else {
                // Load + parse the referenced pipeline and run it on this image.
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
                    auto sub_res = runStandardPipeline(sub, in_rgba, env.presets_dir,
                                                       env.pipelines_dir, env.ts, env.depth + 1);
                    for (auto& e : sub_res.errors)
                        errors.push_back("Subpipeline '" + name + "': " + e);
                    out["image"].image =
                        sub_res.output ? std::move(*sub_res.output) : std::move(in_rgba);
                } else {
                    errors.push_back("Subpipeline: could not load '" + name + "' (passed through)");
                    out["image"].image = std::move(in_rgba);
                }
            }
        }
    } else if (type == "output.standard") {
        // Captured by the caller from this node's input.
    } else if (type == "output.real" || type == "output.integer" || type == "output.channel") {
        // Function-pipeline outputs; inert when run as a standard pipeline.
    } else {
        errors.push_back("Unknown node type '" + type + "' skipped");
    }

    return out;
}

} // namespace

PipelineRunResult runStandardPipeline(const NodeGraph& graph, const tex::Texture& input,
                                      const std::filesystem::path& presets_dir,
                                      const std::filesystem::path& pipelines_dir,
                                      TextureService& texture_service, int depth) {
    PipelineRunResult result;
    const ExecEnv env{presets_dir, pipelines_dir, texture_service, depth};

    // Outputs computed per node, keyed by (node id, pin name).
    std::unordered_map<NodeId, PinMap> outputs;

    // Kahn topological order over the link DAG.
    std::unordered_map<NodeId, int> indeg;
    for (const auto& up : graph.nodes())
        indeg[up->id()] = 0;
    for (const Link& l : graph.links())
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

        // Gather this node's inputs by following links into its input pins.
        PinMap in;
        for (const Link& l : graph.links()) {
            if (l.to.node != id)
                continue;
            auto src = outputs.find(l.from.node);
            if (src == outputs.end())
                continue;
            auto val = src->second.find(l.from.pin);
            if (val != src->second.end())
                in[l.to.pin] = val->second;
        }

        // Standard Input emits the supplied texture; Standard Output captures.
        if (n->typeId() == "input.standard") {
            PinMap o;
            o["image"].image = toRGBA8(input);
            outputs[id] = std::move(o);
        } else if (n->typeId() == "output.standard") {
            if (auto it = in.find("image"); it != in.end() && it->second.image)
                result.output = *it->second.image;
            else
                result.errors.push_back("Standard Output has no connected image");
            outputs[id] = {};
        } else {
            outputs[id] = applyNode(*n, in, env, result.errors);
        }

        for (const Link& l : graph.links()) {
            if (l.from.node != id)
                continue;
            if (--indeg[l.to.node] == 0)
                ready.push_back(l.to.node);
        }
    }

    if (processed < graph.nodes().size())
        result.errors.push_back("Pipeline has a cycle; some nodes were not executed");
    if (!result.output)
        result.errors.push_back("Pipeline produced no Standard Output");

    return result;
}

} // namespace whiteout::textool::services
