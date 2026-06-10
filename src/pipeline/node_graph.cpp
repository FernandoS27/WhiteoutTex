// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pipeline/node_graph.h"

#include <algorithm>

#include "pipeline/node_registry.h"

namespace whiteout::textool::pipeline {

Node* NodeGraph::createNode(std::string_view type_id) {
    auto node = NodeRegistry::instance().create(type_id);
    if (!node)
        return nullptr;
    return addNode(std::move(node));
}

Node* NodeGraph::addNode(std::unique_ptr<Node> node) {
    if (!node)
        return nullptr;
    node->setId(next_node_id_++);
    nodes_.push_back(std::move(node));
    return nodes_.back().get();
}

Node* NodeGraph::insertNode(std::unique_ptr<Node> node) {
    if (!node)
        return nullptr;
    next_node_id_ = std::max(next_node_id_, node->id() + 1);
    nodes_.push_back(std::move(node));
    return nodes_.back().get();
}

void NodeGraph::removeNode(NodeId id) {
    // Drop every link that touches the node first.
    std::erase_if(links_, [id](const Link& l) { return l.from.node == id || l.to.node == id; });
    std::erase_if(nodes_, [id](const std::unique_ptr<Node>& n) { return n->id() == id; });
}

Node* NodeGraph::node(NodeId id) {
    for (auto& n : nodes_) {
        if (n->id() == id)
            return n.get();
    }
    return nullptr;
}

const Node* NodeGraph::node(NodeId id) const {
    for (const auto& n : nodes_) {
        if (n->id() == id)
            return n.get();
    }
    return nullptr;
}

bool NodeGraph::isInputConnected(const PinRef& input) const {
    return std::any_of(links_.begin(), links_.end(), [&](const Link& l) {
        return l.to.node == input.node && l.to.pin == input.pin;
    });
}

LinkId NodeGraph::addLink(PinRef from, PinRef to) {
    const Node* from_node = node(from.node);
    const Node* to_node = node(to.node);
    if (!from_node || !to_node)
        return 0;

    const Pin* out_pin = from_node->findPin(from.pin, /*is_input=*/false);
    const Pin* in_pin = to_node->findPin(to.pin, /*is_input=*/true);
    if (!out_pin || !in_pin)
        return 0;

    // Type compatibility and single-driver rule for the input pin.
    if (!pinTypesCompatible(out_pin->type, in_pin->type))
        return 0;
    if (isInputConnected(to))
        return 0;

    const LinkId id = next_link_id_++;
    links_.push_back({id, std::move(from), std::move(to)});
    return id;
}

void NodeGraph::removeLink(LinkId id) {
    std::erase_if(links_, [id](const Link& l) { return l.id == id; });
}

void NodeGraph::clear() {
    nodes_.clear();
    links_.clear();
    name_.clear();
    type_ = PipelineType::Standard;
    next_node_id_ = 1;
    next_link_id_ = 1;
}

} // namespace whiteout::textool::pipeline
