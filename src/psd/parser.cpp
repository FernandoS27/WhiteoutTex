// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "psd/parser.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <Psd.h>
#include <PsdMallocAllocator.h>
#include <PsdNativeFile.h>
#include <PsdDocument.h>
#include <PsdColorMode.h>
#include <PsdLayer.h>
#include <PsdChannel.h>
#include <PsdChannelType.h>
#include <PsdLayerMaskSection.h>
#include <PsdImageDataSection.h>
#include <PsdParseDocument.h>
#include <PsdParseLayerMaskSection.h>
#include <PsdParseImageDataSection.h>
#include <PsdInterleave.h>
#include <PsdPlanarImage.h>

PSD_USING_NAMESPACE;

namespace whiteout::textures::psd {

// ============================================================================
// MemoryFile — reads from an in-memory buffer via the psd_sdk File interface
// ============================================================================

class MemoryFile : public File {
public:
    explicit MemoryFile(Allocator* allocator)
        : File(allocator), data_(nullptr), size_(0) {}

    bool SetBuffer(const u8* data, size_t size) {
        data_ = data;
        size_ = size;
        return data != nullptr && size > 0;
    }

private:
    bool DoOpenRead(const wchar_t*) override { return false; }
    bool DoOpenWrite(const wchar_t*) override { return false; }
    bool DoClose() override { return true; }

    ReadOperation DoRead(void* buffer, uint32_t count, uint64_t position) override {
        if (position + count > size_) {
            const uint32_t available =
                position < size_ ? static_cast<uint32_t>(size_ - position) : 0;
            if (available > 0)
                std::memcpy(buffer, data_ + position, available);
            if (available < count)
                std::memset(static_cast<u8*>(buffer) + available, 0, count - available);
        } else {
            std::memcpy(buffer, data_ + position, count);
        }
        // Return a non-null sentinel so WaitForRead succeeds.
        return reinterpret_cast<ReadOperation>(uintptr_t(1));
    }

    bool DoWaitForRead(ReadOperation& op) override {
        op = nullptr;
        return true;
    }

    WriteOperation DoWrite(const void*, uint32_t, uint64_t) override { return nullptr; }
    bool DoWaitForWrite(WriteOperation&) override { return false; }

    uint64_t DoGetSize() const override { return size_; }

    const u8* data_;
    size_t size_;
};

// ============================================================================
// Impl
// ============================================================================

class Parser::Impl {
public:
    ParseMode mode;
    std::vector<std::string> issues;

    explicit Impl(ParseMode m) : mode(m) {}

    void clearIssues() { issues.clear(); }

    void addIssue(const std::string& msg) {
        if (mode == ParseMode::Strict)
            throw std::runtime_error(msg);
        issues.push_back(msg);
    }

    std::optional<Texture> parseFromFile(File& file) {
        MallocAllocator allocator;

        Document* document = CreateDocument(&file, &allocator);
        if (!document) {
            addIssue("PSD: failed to create document");
            return std::nullopt;
        }

        auto result = extractMergedImage(document, &file, &allocator);

        DestroyDocument(document, &allocator);
        return result;
    }

    std::optional<Texture> extractMergedImage(Document* document, File* file,
                                              Allocator* allocator) {
        if (document->colorMode != colorMode::RGB) {
            addIssue("PSD: only RGB color mode is supported (got " +
                     std::string(colorMode::ToString(document->colorMode)) + ")");
            return std::nullopt;
        }

        const unsigned int bpc = document->bitsPerChannel;
        if (bpc != 8 && bpc != 16 && bpc != 32) {
            addIssue("PSD: unsupported bits per channel: " + std::to_string(bpc));
            return std::nullopt;
        }

        // Determine if there's a transparency mask by parsing the layer mask section.
        bool hasTransparencyMask = false;
        LayerMaskSection* layerMaskSection = ParseLayerMaskSection(document, file, allocator);
        if (layerMaskSection) {
            hasTransparencyMask = layerMaskSection->hasTransparencyMask;
            DestroyLayerMaskSection(layerMaskSection, allocator);
        }

        if (document->imageDataSection.length == 0) {
            addIssue("PSD: no merged image data section (file may not have been "
                     "saved with 'Maximize Compatibility')");
            return std::nullopt;
        }

        ImageDataSection* imageData = ParseImageDataSection(document, file, allocator);
        if (!imageData) {
            addIssue("PSD: failed to parse image data section");
            return std::nullopt;
        }

        const unsigned int w = document->width;
        const unsigned int h = document->height;
        const unsigned int imageCount = imageData->imageCount;

        // Determine if the merged image is RGB or RGBA.
        bool hasAlpha = false;
        if (imageCount >= 4 && hasTransparencyMask)
            hasAlpha = true;

        PixelFormat fmt;
        if (bpc == 8)
            fmt = PixelFormat::RGBA8;
        else if (bpc == 16)
            fmt = PixelFormat::RGBA16;
        else
            fmt = PixelFormat::RGBA32F;

        auto tex = Texture::create2D(fmt, w, h, 1);
        u8* dst = tex.dataPtr();

        if (bpc == 8) {
            if (hasAlpha) {
                imageUtil::InterleaveRGBA(
                    static_cast<const uint8_t*>(imageData->images[0].data),
                    static_cast<const uint8_t*>(imageData->images[1].data),
                    static_cast<const uint8_t*>(imageData->images[2].data),
                    static_cast<const uint8_t*>(imageData->images[3].data),
                    dst, w, h);
            } else {
                imageUtil::InterleaveRGB(
                    static_cast<const uint8_t*>(imageData->images[0].data),
                    static_cast<const uint8_t*>(imageData->images[1].data),
                    static_cast<const uint8_t*>(imageData->images[2].data),
                    uint8_t(255), dst, w, h);
            }
        } else if (bpc == 16) {
            auto* dst16 = reinterpret_cast<uint16_t*>(dst);
            if (hasAlpha) {
                imageUtil::InterleaveRGBA(
                    static_cast<const uint16_t*>(imageData->images[0].data),
                    static_cast<const uint16_t*>(imageData->images[1].data),
                    static_cast<const uint16_t*>(imageData->images[2].data),
                    static_cast<const uint16_t*>(imageData->images[3].data),
                    dst16, w, h);
            } else {
                imageUtil::InterleaveRGB(
                    static_cast<const uint16_t*>(imageData->images[0].data),
                    static_cast<const uint16_t*>(imageData->images[1].data),
                    static_cast<const uint16_t*>(imageData->images[2].data),
                    uint16_t(65535), dst16, w, h);
            }
        } else {
            auto* dst32 = reinterpret_cast<float32_t*>(dst);
            if (hasAlpha) {
                imageUtil::InterleaveRGBA(
                    static_cast<const float32_t*>(imageData->images[0].data),
                    static_cast<const float32_t*>(imageData->images[1].data),
                    static_cast<const float32_t*>(imageData->images[2].data),
                    static_cast<const float32_t*>(imageData->images[3].data),
                    dst32, w, h);
            } else {
                imageUtil::InterleaveRGB(
                    static_cast<const float32_t*>(imageData->images[0].data),
                    static_cast<const float32_t*>(imageData->images[1].data),
                    static_cast<const float32_t*>(imageData->images[2].data),
                    float32_t(1.0f), dst32, w, h);
            }
        }

        DestroyImageDataSection(imageData, allocator);
        return tex;
    }
};

// ============================================================================
// Construction / destruction
// ============================================================================

Parser::Parser(ParseMode parseMode) : pImpl(std::make_unique<Impl>(parseMode)) {}
Parser::~Parser() = default;

// ============================================================================
// Parse from file
// ============================================================================

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->clearIssues();

    MallocAllocator allocator;
    NativeFile file(&allocator);

    auto wpath = std::filesystem::path(filePath).wstring();
    if (!file.OpenRead(wpath.c_str())) {
        pImpl->addIssue("PSD: cannot open file: " + filePath);
        return std::nullopt;
    }

    auto result = pImpl->parseFromFile(file);
    file.Close();
    return result;
}

// ============================================================================
// Parse from buffer
// ============================================================================

std::optional<Texture> Parser::parse(std::span<const u8> buffer) {
    pImpl->clearIssues();

    if (buffer.empty()) {
        pImpl->addIssue("PSD: empty buffer");
        return std::nullopt;
    }

    MallocAllocator allocator;
    MemoryFile file(&allocator);
    file.SetBuffer(buffer.data(), buffer.size());

    return pImpl->parseFromFile(file);
}

// ============================================================================
// Issue reporting
// ============================================================================

bool Parser::hasIssues() const { return !pImpl->issues.empty(); }
const std::vector<std::string>& Parser::getIssues() const { return pImpl->issues; }

} // namespace whiteout::textures::psd
