// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pipeline/node_graph.h"

#include <algorithm>
#include <unordered_set>
#include <variant>

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

std::size_t NodeGraph::pruneInvalidLinks() {
    const std::size_t before = links_.size();
    std::erase_if(links_, [this](const Link& l) {
        const Node* fn = node(l.from.node);
        const Node* tn = node(l.to.node);
        return !fn || !tn || !fn->findPin(l.from.pin, /*is_input=*/false) ||
               !tn->findPin(l.to.pin, /*is_input=*/true);
    });
    return before - links_.size();
}

void NodeGraph::clear() {
    nodes_.clear();
    links_.clear();
    name_.clear();
    type_ = PipelineType::Standard;
    next_node_id_ = 1;
    next_link_id_ = 1;
}

namespace {
/// The "name" param of an interface (Input/Output) node, or nullptr.
const std::string* portName(const Node& n) {
    for (const Param& p : n.params())
        if (p.name == "name" && std::holds_alternative<std::string>(p.value))
            return &std::get<std::string>(p.value);
    return nullptr;
}
/// Read a real-valued param, or @p def.
f64 realParam(const Node& n, std::string_view name, f64 def) {
    for (const Param& p : n.params())
        if (p.name == name && std::holds_alternative<f64>(p.value))
            return std::get<f64>(p.value);
    return def;
}
/// Frame / Local-Call nodes are transparent to frame membership.
bool isFrameOrCall(const Node& n) {
    return n.typeId() == "frame.local" || n.typeId() == "local.call";
}
} // namespace

PipelineInterface NodeGraph::interface() const {
    // Input/Output nodes that live inside a Local Pipeline frame belong to that
    // frame's local interface, NOT this pipeline's external interface — exclude
    // them so a Subpipeline node mirrors only the top-level ports.
    std::unordered_set<NodeId> framed;
    for (const auto& up : nodes_)
        if (up->typeId() == "frame.local")
            for (NodeId id : nodesInFrame(*up))
                framed.insert(id);

    PipelineInterface iface;
    for (const auto& up : nodes_) {
        const Node& n = *up;
        if (framed.find(n.id()) != framed.end())
            continue; // belongs to a local pipeline frame
        const std::string* nm = portName(n);
        if (!nm)
            continue; // not an external port (e.g. resource/const nodes)
        if (n.category() == NodeCategory::Input && !n.outputs().empty())
            iface.inputs.push_back({*nm, n.outputs().front().type});
        else if (n.category() == NodeCategory::Output && !n.inputs().empty())
            iface.outputs.push_back({*nm, n.inputs().front().type});
    }
    return iface;
}

std::vector<NodeId> NodeGraph::nodesInFrame(const Node& frame) const {
    const Vec2 fp = frame.position();
    const f32 fw = static_cast<f32>(realParam(frame, "w", 0.0));
    const f32 fh = static_cast<f32>(realParam(frame, "h", 0.0));
    std::vector<NodeId> members;
    for (const auto& up : nodes_) {
        const Node& n = *up;
        if (isFrameOrCall(n))
            continue; // frames and calls are transparent to membership
        const Vec2 p = n.position();
        if (p.x >= fp.x && p.x <= fp.x + fw && p.y >= fp.y && p.y <= fp.y + fh)
            members.push_back(n.id());
    }
    return members;
}

PipelineType NodeGraph::nonFunctionType() const {
    // Nodes inside a Local Pipeline frame belong to that frame, not this graph.
    std::unordered_set<NodeId> framed;
    for (const auto& up : nodes_)
        if (up->typeId() == "frame.local")
            for (NodeId id : nodesInFrame(*up))
                framed.insert(id);

    int standard_in = 0, standard_out = 0;
    for (const auto& up : nodes_) {
        if (framed.find(up->id()) != framed.end())
            continue;
        if (up->typeId() == "input.standard")
            ++standard_in;
        else if (up->typeId() == "output.standard")
            ++standard_out;
    }
    return (standard_in == 1 && standard_out == 1) ? PipelineType::Standard : PipelineType::Varying;
}

PipelineInterface NodeGraph::localInterface(const Node& frame) const {
    const std::vector<NodeId> members = nodesInFrame(frame);
    PipelineInterface iface;
    for (NodeId id : members) {
        const Node* n = node(id);
        if (!n)
            continue;
        const std::string* nm = portName(*n);
        if (!nm)
            continue;
        if (n->category() == NodeCategory::Input && !n->outputs().empty())
            iface.inputs.push_back({*nm, n->outputs().front().type});
        else if (n->category() == NodeCategory::Output && !n->inputs().empty())
            iface.outputs.push_back({*nm, n->inputs().front().type});
    }
    return iface;
}

void applyPipelineInterface(Node& node, const PipelineInterface& iface) {
    std::vector<Pin> inputs, outputs;
    inputs.reserve(iface.inputs.size());
    outputs.reserve(iface.outputs.size());
    for (const PipelinePort& p : iface.inputs)
        inputs.push_back({p.name, p.type, /*is_input=*/true});
    for (const PipelinePort& p : iface.outputs)
        outputs.push_back({p.name, p.type, /*is_input=*/false});
    node.setPins(std::move(inputs), std::move(outputs));
}

} // namespace whiteout::textool::pipeline
