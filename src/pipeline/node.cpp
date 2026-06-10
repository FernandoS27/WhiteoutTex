// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pipeline/node.h"

namespace whiteout::textool::pipeline {

const Pin* Node::findPin(std::string_view name, bool is_input) const noexcept {
    const auto& pins = is_input ? inputs_ : outputs_;
    for (const auto& p : pins) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

Param* Node::findParam(std::string_view name) noexcept {
    for (auto& p : params_) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

} // namespace whiteout::textool::pipeline
