// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file node_registry.h
 * @brief Single source of truth for the set of node types (Factory + palette).
 *
 * Mirrors format_registry's idiom: one declarative descriptor per node type;
 * everything derives from it.  Deserialization looks a node up by its stable
 * type-id string and calls make(); the Node Palette is just a view over all().
 *
 * Adding a node type = one registerType() call + one Node subclass.
 */

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pipeline/node.h"

namespace whiteout::textool::pipeline {

/// Static metadata + factory for one node type.
struct NodeDescriptor {
    std::string type_id;      ///< Stable JSON/registry key, e.g. "resize".
    NodeCategory category;    ///< Drives palette grouping + topology checks.
    std::string display_name; ///< i18n key for the palette label.
    std::function<std::unique_ptr<Node>()> make; ///< Factory (freshly declares pins/params).
};

/// Process-wide registry of node types.  Populated once at startup via
/// registerBuiltinNodes().
class NodeRegistry {
public:
    static NodeRegistry& instance();

    void registerType(NodeDescriptor descriptor);

    /// Construct a fresh node of @p type_id, or nullptr if unknown.  The node
    /// has its pins/params declared but id/position unset (the caller assigns).
    std::unique_ptr<Node> create(std::string_view type_id) const;

    const NodeDescriptor* find(std::string_view type_id) const;
    std::span<const NodeDescriptor> all() const noexcept { return types_; }

private:
    NodeRegistry() = default;
    std::vector<NodeDescriptor> types_;
};

/// Register every built-in node type.  Idempotent; call once at startup.
void registerBuiltinNodes();

} // namespace whiteout::textool::pipeline
