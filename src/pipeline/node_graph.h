// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file node_graph.h
 * @brief Owns the nodes and links of one pipeline (the serializable model).
 *
 * Links are stored separately from nodes and reference pins by (node id, pin
 * name) — a link is one fact, not duplicated on both endpoints, and pin names
 * keep the JSON forward-compatible when a node type gains pins.
 *
 * The graph is a DAG; topological evaluation lands in a later stage.
 */

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pipeline/node.h"

namespace whiteout::textool::pipeline {

using LinkId = u32;

/// Contract of a pipeline's external interface.  Standard = exactly one
/// Standard Input and one Standard Output; Varying = multiple inputs/outputs;
/// Function = a named, typed input/output set (callable like a function).
enum class PipelineType : u8 { Standard, Varying, Function };

/// Endpoint of a link: a specific pin on a specific node.
struct PinRef {
    NodeId node = 0;
    std::string pin; ///< Pin name (stable id within the node).
};

/// A directed connection from an output pin (@c from) to an input pin (@c to).
struct Link {
    LinkId id = 0;
    PinRef from; ///< Source output pin.
    PinRef to;   ///< Destination input pin.
};

class NodeGraph {
public:
    NodeGraph() = default;

    NodeGraph(const NodeGraph&) = delete;
    NodeGraph& operator=(const NodeGraph&) = delete;
    NodeGraph(NodeGraph&&) noexcept = default;
    NodeGraph& operator=(NodeGraph&&) noexcept = default;

    // ── Nodes ──────────────────────────────────────────────────────────
    /// Construct a node by type-id via the registry and insert it.  Returns the
    /// inserted node (id assigned), or nullptr if the type is unknown.
    Node* createNode(std::string_view type_id);

    /// Insert an already-constructed node, assigning it a fresh id.
    Node* addNode(std::unique_ptr<Node> node);

    /// Insert a node keeping its already-set id (used by deserialization).
    /// Advances the id counter past the node's id so later inserts don't clash.
    Node* insertNode(std::unique_ptr<Node> node);

    /// Remove a node and every link touching it.  No-op if id is unknown.
    void removeNode(NodeId id);

    Node* node(NodeId id);
    const Node* node(NodeId id) const;
    std::span<const std::unique_ptr<Node>> nodes() const noexcept { return nodes_; }

    // ── Links ──────────────────────────────────────────────────────────
    /// Connect @p from (an output pin) to @p to (an input pin).  Validates that
    /// both pins exist, directions are correct, types are compatible, and the
    /// input pin is not already driven.  Returns the new link id, or 0 on
    /// rejection.
    LinkId addLink(PinRef from, PinRef to);

    void removeLink(LinkId id);
    std::span<const Link> links() const noexcept { return links_; }

    /// True if @p input (an input pin) already has an incoming link.
    bool isInputConnected(const PinRef& input) const;

    // ── Pipeline metadata ──────────────────────────────────────────────
    /// Human-readable pipeline name (shown in pickers instead of the file name).
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    // ── Pipeline type (external interface contract) ────────────────────
    PipelineType pipelineType() const noexcept { return type_; }
    void setPipelineType(PipelineType t) noexcept { type_ = t; }

    // ── Id bookkeeping (used by the serializer to restore counters) ─────
    NodeId nextNodeId() const noexcept { return next_node_id_; }
    LinkId nextLinkId() const noexcept { return next_link_id_; }
    void setNextNodeId(NodeId v) noexcept { next_node_id_ = v; }
    void setNextLinkId(LinkId v) noexcept { next_link_id_ = v; }

    void clear();

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Link> links_;
    std::string name_;
    PipelineType type_ = PipelineType::Standard;
    NodeId next_node_id_ = 1;
    LinkId next_link_id_ = 1;
};

} // namespace whiteout::textool::pipeline
