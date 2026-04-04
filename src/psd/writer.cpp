// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "psd/writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <Psd.h>
#include <PsdMallocAllocator.h>
#include <PsdNativeFile.h>
#include <PsdExport.h>
#include <PsdExportDocument.h>

PSD_USING_NAMESPACE;

namespace whiteout::textures::psd {

// ============================================================================
// MemoryWriteFile — writes to a growable in-memory buffer
// ============================================================================

class MemoryWriteFile : public File {
public:
    explicit MemoryWriteFile(Allocator* allocator)
        : File(allocator) {}

    std::vector<u8> takeBuffer() { return std::move(buffer_); }

private:
    bool DoOpenRead(const wchar_t*) override { return false; }
    bool DoOpenWrite(const wchar_t*) override { return true; }
    bool DoClose() override { return true; }

    ReadOperation DoRead(void*, uint32_t, uint64_t) override { return nullptr; }
    bool DoWaitForRead(ReadOperation&) override { return false; }

    WriteOperation DoWrite(const void* data, uint32_t count, uint64_t position) override {
        const size_t end = static_cast<size_t>(position) + count;
        if (end > buffer_.size())
            buffer_.resize(end, 0);
        std::memcpy(buffer_.data() + position, data, count);
        return reinterpret_cast<WriteOperation>(uintptr_t(1));
    }

    bool DoWaitForWrite(WriteOperation& op) override {
        op = nullptr;
        return true;
    }

    uint64_t DoGetSize() const override { return buffer_.size(); }

    std::vector<u8> buffer_;
};

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// De-interleave a single channel from RGBA interleaved data.
template <typename T>
void extractPlanar(const T* interleaved, T* planar, unsigned int pixelCount,
                   unsigned int channelIndex) {
    for (unsigned int i = 0; i < pixelCount; ++i) {
        planar[i] = interleaved[i * 4 + channelIndex];
    }
}

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

class Writer::Impl {
public:
    WriteMode mode;
    std::vector<std::string> issues;

    explicit Impl(WriteMode m) : mode(m) {}

    void clearIssues() { issues.clear(); }

    void addIssue(const std::string& msg) {
        if (mode == WriteMode::Strict)
            throw std::runtime_error(msg);
        issues.push_back(msg);
    }

    void writeToFile(File& file, const Texture& texture) {
        MallocAllocator allocator;

        Texture src = texture;

        // Determine bit depth and convert to an appropriate format.
        unsigned int bpc;
        PixelFormat srcFmt = src.format();
        if (srcFmt == PixelFormat::RGBA32F || srcFmt == PixelFormat::RG32F ||
            srcFmt == PixelFormat::R32F) {
            bpc = 32;
            if (srcFmt != PixelFormat::RGBA32F)
                src.format(PixelFormat::RGBA32F);
        } else if (srcFmt == PixelFormat::RGBA16 || srcFmt == PixelFormat::RG16 ||
                   srcFmt == PixelFormat::R16) {
            bpc = 16;
            if (srcFmt != PixelFormat::RGBA16)
                src.format(PixelFormat::RGBA16);
        } else {
            bpc = 8;
            if (srcFmt != PixelFormat::RGBA8)
                src.format(PixelFormat::RGBA8);
        }

        const unsigned int w = src.width();
        const unsigned int h = src.height();
        const unsigned int pixelCount = w * h;

        ExportDocument* document =
            CreateExportDocument(&allocator, w, h, bpc, exportColorMode::RGB);
        if (!document) {
            addIssue("PSD: failed to create export document");
            return;
        }

        // Add a single layer with the texture data.
        const unsigned int layerIdx = AddLayer(document, &allocator, "Background");

        // De-interleave and feed planar data to the layer.
        if (bpc == 8) {
            const auto* interleaved = reinterpret_cast<const uint8_t*>(src.dataPtr());
            auto* planeR = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            auto* planeG = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            auto* planeB = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            auto* planeA = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));

            extractPlanar(interleaved, planeR, pixelCount, 0);
            extractPlanar(interleaved, planeG, pixelCount, 1);
            extractPlanar(interleaved, planeB, pixelCount, 2);
            extractPlanar(interleaved, planeA, pixelCount, 3);

            UpdateLayer(document, &allocator, layerIdx, exportChannel::RED,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeR, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::GREEN,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeG, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::BLUE,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeB, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::ALPHA,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeA, compressionType::ZIP);

            // Also update merged image data for maximum compatibility.
            auto* mergedR = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            auto* mergedG = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            auto* mergedB = static_cast<uint8_t*>(allocator.Allocate(pixelCount, 16u));
            extractPlanar(interleaved, mergedR, pixelCount, 0);
            extractPlanar(interleaved, mergedG, pixelCount, 1);
            extractPlanar(interleaved, mergedB, pixelCount, 2);
            UpdateMergedImage(document, &allocator, mergedR, mergedG, mergedB);

            allocator.Free(planeR);
            allocator.Free(planeG);
            allocator.Free(planeB);
            allocator.Free(planeA);
            allocator.Free(mergedR);
            allocator.Free(mergedG);
            allocator.Free(mergedB);
        } else if (bpc == 16) {
            const auto* interleaved = reinterpret_cast<const uint16_t*>(src.dataPtr());
            const size_t planeBytes = pixelCount * sizeof(uint16_t);
            auto* planeR = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeG = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeB = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeA = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));

            extractPlanar(interleaved, planeR, pixelCount, 0);
            extractPlanar(interleaved, planeG, pixelCount, 1);
            extractPlanar(interleaved, planeB, pixelCount, 2);
            extractPlanar(interleaved, planeA, pixelCount, 3);

            UpdateLayer(document, &allocator, layerIdx, exportChannel::RED,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeR, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::GREEN,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeG, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::BLUE,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeB, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::ALPHA,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeA, compressionType::ZIP);

            auto* mergedR = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            auto* mergedG = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            auto* mergedB = static_cast<uint16_t*>(allocator.Allocate(planeBytes, 16u));
            extractPlanar(interleaved, mergedR, pixelCount, 0);
            extractPlanar(interleaved, mergedG, pixelCount, 1);
            extractPlanar(interleaved, mergedB, pixelCount, 2);
            UpdateMergedImage(document, &allocator, mergedR, mergedG, mergedB);

            allocator.Free(planeR);
            allocator.Free(planeG);
            allocator.Free(planeB);
            allocator.Free(planeA);
            allocator.Free(mergedR);
            allocator.Free(mergedG);
            allocator.Free(mergedB);
        } else {
            const auto* interleaved = reinterpret_cast<const float32_t*>(src.dataPtr());
            const size_t planeBytes = pixelCount * sizeof(float32_t);
            auto* planeR = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeG = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeB = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            auto* planeA = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));

            extractPlanar(interleaved, planeR, pixelCount, 0);
            extractPlanar(interleaved, planeG, pixelCount, 1);
            extractPlanar(interleaved, planeB, pixelCount, 2);
            extractPlanar(interleaved, planeA, pixelCount, 3);

            UpdateLayer(document, &allocator, layerIdx, exportChannel::RED,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeR, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::GREEN,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeG, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::BLUE,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeB, compressionType::ZIP);
            UpdateLayer(document, &allocator, layerIdx, exportChannel::ALPHA,
                        0, 0, static_cast<int>(w), static_cast<int>(h),
                        planeA, compressionType::ZIP);

            auto* mergedR = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            auto* mergedG = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            auto* mergedB = static_cast<float32_t*>(allocator.Allocate(planeBytes, 16u));
            extractPlanar(interleaved, mergedR, pixelCount, 0);
            extractPlanar(interleaved, mergedG, pixelCount, 1);
            extractPlanar(interleaved, mergedB, pixelCount, 2);
            UpdateMergedImage(document, &allocator, mergedR, mergedG, mergedB);

            allocator.Free(planeR);
            allocator.Free(planeG);
            allocator.Free(planeB);
            allocator.Free(planeA);
            allocator.Free(mergedR);
            allocator.Free(mergedG);
            allocator.Free(mergedB);
        }

        WriteDocument(document, &allocator, &file);
        DestroyExportDocument(document, &allocator);
    }
};

// ============================================================================
// Construction / destruction
// ============================================================================

Writer::Writer(WriteMode writeMode) : pImpl(std::make_unique<Impl>(writeMode)) {}
Writer::~Writer() = default;

// ============================================================================
// Write to file
// ============================================================================

void Writer::write(const std::string& filePath, const Texture& texture) {
    pImpl->clearIssues();

    MallocAllocator allocator;
    NativeFile file(&allocator);

    auto wpath = std::filesystem::path(filePath).wstring();
    if (!file.OpenWrite(wpath.c_str())) {
        pImpl->addIssue("PSD: cannot open file for writing: " + filePath);
        return;
    }

    pImpl->writeToFile(file, texture);
    file.Close();
}

// ============================================================================
// Write to buffer
// ============================================================================

std::vector<u8> Writer::write(const Texture& texture) {
    pImpl->clearIssues();

    MallocAllocator allocator;
    MemoryWriteFile file(&allocator);

    // MemoryWriteFile is always "open"
    pImpl->writeToFile(file, texture);
    return file.takeBuffer();
}

// ============================================================================
// Issue reporting
// ============================================================================

bool Writer::hasIssues() const { return !pImpl->issues.empty(); }
const std::vector<std::string>& Writer::getIssues() const { return pImpl->issues; }

} // namespace whiteout::textures::psd
