// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pipeline/node_registry.h"

namespace whiteout::textool::pipeline {

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerType(NodeDescriptor descriptor) {
    // Replace on duplicate type-id so re-registration stays idempotent.
    for (auto& existing : types_) {
        if (existing.type_id == descriptor.type_id) {
            existing = std::move(descriptor);
            return;
        }
    }
    types_.push_back(std::move(descriptor));
}

std::unique_ptr<Node> NodeRegistry::create(std::string_view type_id) const {
    const NodeDescriptor* d = find(type_id);
    return d ? d->make() : nullptr;
}

const NodeDescriptor* NodeRegistry::find(std::string_view type_id) const {
    for (const auto& d : types_) {
        if (d.type_id == type_id)
            return &d;
    }
    return nullptr;
}

} // namespace whiteout::textool::pipeline
