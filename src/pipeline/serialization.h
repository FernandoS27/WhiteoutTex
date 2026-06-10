// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file serialization.h
 * @brief JSON (de)serialization for a NodeGraph.
 *
 * Schema (version 1):
 *   {
 *     "version": 1,
 *     "nodes": [ { "id": 1, "type": "op.resize", "pos": [x, y],
 *                  "params": { "width": 512, "height": 512 } }, ... ],
 *     "links": [ { "from": [1, "image"], "to": [2, "image"] }, ... ]
 *   }
 *
 * Pins are referenced by name and links stored separately, so the format stays
 * forward-compatible when a node type gains pins.  Params are written/read
 * generically by visiting ParamValue — no per-node serialization code.
 *
 * Deserialization is construct-then-populate: the registry builds the node
 * (which declares each param with its concrete type), then values are read into
 * those typed slots — so JSON `42` lands as i64 or f64 per the node's schema,
 * never guessed from the literal.  Unknown node types are skipped with a
 * warning rather than failing the whole load.
 */

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/node_graph.h"

namespace whiteout::textool::pipeline {

/// Current on-disk schema version.
inline constexpr int kPipelineSchemaVersion = 1;

/// Serialize @p graph to a JSON object.
nlohmann::json toJson(const NodeGraph& graph);

/// Rebuild @p graph from @p json (graph is cleared first).  Non-fatal problems
/// (unknown node type, dropped/invalid link, unknown param) are appended to
/// @p warnings if non-null.  Returns false only on a structurally invalid
/// document (e.g. missing/!= supported version, malformed root).
bool fromJson(const nlohmann::json& json, NodeGraph& graph,
              std::vector<std::string>* warnings = nullptr);

} // namespace whiteout::textool::pipeline
