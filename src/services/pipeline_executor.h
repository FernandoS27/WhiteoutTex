// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file pipeline_executor.h
 * @brief Runs a pipeline::NodeGraph over a texture.
 *
 * The pipeline model (src/pipeline) is pure data; this is where it is executed,
 * using app types (Texture, TextureService).  A Standard pipeline takes one
 * Standard Input (the supplied texture) and yields the Standard Output node's
 * result.  Operations are applied via TextureService + direct RGBA8 pixel ops.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>

#include "pipeline/node_graph.h"

namespace whiteout::textool::services {

class TextureService;

struct PipelineRunResult {
    std::optional<whiteout::textures::Texture> output; ///< Standard Output result.
    std::vector<std::string> errors; ///< Non-fatal issues (unsupported / skipped nodes).
    bool ok() const { return output.has_value(); }
};

/// Execute @p graph as a Standard pipeline: @p input feeds Standard Input nodes,
/// and the Standard Output node's incoming image is returned.
/// @param presets_dir    resources/presets root for Resource input nodes.
/// @param pipelines_dir  pipelines folder, for Subpipeline nodes to resolve.
/// @param depth          Recursion depth (Subpipeline nesting guard).
PipelineRunResult runStandardPipeline(const pipeline::NodeGraph& graph,
                                      const whiteout::textures::Texture& input,
                                      const std::filesystem::path& presets_dir,
                                      const std::filesystem::path& pipelines_dir,
                                      TextureService& texture_service, int depth = 0);

} // namespace whiteout::textool::services
