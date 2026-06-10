// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "pipeline/serialization.h"

#include "pipeline/node_registry.h"

namespace whiteout::textool::pipeline {

using nlohmann::json;

namespace {

void warn(std::vector<std::string>* warnings, std::string msg) {
    if (warnings)
        warnings->push_back(std::move(msg));
}

// ── Params ─────────────────────────────────────────────────────────────────

/// Serialize a scalar ParamValue to JSON by visiting the active alternative.
json paramToJson(const ParamValue& v) {
    return std::visit([](const auto& x) -> json { return x; }, v);
}

/// Read @p j into @p slot, choosing the target type from the alternative the
/// freshly-constructed node already declared (construct-then-populate).  Bad
/// JSON for a given slot leaves the default in place and warns.
void paramFromJson(ParamValue& slot, const json& j, std::string_view ctx,
                   std::vector<std::string>* warnings) {
    std::visit(
        [&](auto& dst) {
            using T = std::decay_t<decltype(dst)>;
            try {
                dst = j.get<T>();
            } catch (const json::exception&) {
                warn(warnings, std::string("param '") + std::string(ctx) +
                                   "': type mismatch, keeping default");
            }
        },
        slot);
}

} // namespace

json toJson(const NodeGraph& graph) {
    json doc;
    doc["version"] = kPipelineSchemaVersion;
    doc["name"] = graph.name();
    doc["type"] = graph.pipelineType() == PipelineType::Function ? "function"
                  : graph.pipelineType() == PipelineType::Varying ? "varying"
                                                                  : "standard";

    json nodes = json::array();
    for (const auto& n : graph.nodes()) {
        json jn;
        jn["id"] = n->id();
        jn["type"] = n->typeId();
        jn["pos"] = {n->position().x, n->position().y};

        json params = json::object();
        for (const auto& p : n->params())
            params[p.name] = paramToJson(p.value);
        jn["params"] = std::move(params);

        nodes.push_back(std::move(jn));
    }
    doc["nodes"] = std::move(nodes);

    json links = json::array();
    for (const auto& l : graph.links()) {
        json jl;
        jl["from"] = {l.from.node, l.from.pin};
        jl["to"] = {l.to.node, l.to.pin};
        links.push_back(std::move(jl));
    }
    doc["links"] = std::move(links);

    return doc;
}

bool fromJson(const json& doc, NodeGraph& graph, std::vector<std::string>* warnings) {
    if (!doc.is_object()) {
        warn(warnings, "pipeline: root is not a JSON object");
        return false;
    }
    const int version = doc.value("version", -1);
    if (version != kPipelineSchemaVersion) {
        warn(warnings, "pipeline: unsupported schema version " + std::to_string(version));
        return false;
    }

    graph.clear();

    graph.setName(doc.value("name", std::string{}));
    const std::string ptype = doc.value("type", std::string{"standard"});
    graph.setPipelineType(ptype == "function" ? PipelineType::Function
                          : ptype == "varying" ? PipelineType::Varying
                                               : PipelineType::Standard);

    // ── Nodes (construct-then-populate) ────────────────────────────────
    for (const auto& jn : doc.value("nodes", json::array())) {
        const std::string type = jn.value("type", std::string{});
        auto node = NodeRegistry::instance().create(type);
        if (!node) {
            warn(warnings, "pipeline: unknown node type '" + type + "', skipped");
            continue;
        }

        node->setId(jn.value("id", NodeId{0}));
        if (auto it = jn.find("pos"); it != jn.end() && it->is_array() && it->size() == 2)
            node->setPosition({it->at(0).get<f32>(), it->at(1).get<f32>()});

        if (auto it = jn.find("params"); it != jn.end() && it->is_object()) {
            for (auto& p : node->params()) {
                if (auto pit = it->find(p.name); pit != it->end())
                    paramFromJson(p.value, *pit, p.name, warnings);
            }
        }

        graph.insertNode(std::move(node));
    }

    // ── Links (revalidated via addLink; ids regenerated) ───────────────
    for (const auto& jl : doc.value("links", json::array())) {
        const auto& jf = jl.value("from", json::array());
        const auto& jt = jl.value("to", json::array());
        if (!jf.is_array() || jf.size() != 2 || !jt.is_array() || jt.size() != 2) {
            warn(warnings, "pipeline: malformed link entry, skipped");
            continue;
        }
        PinRef from{jf.at(0).get<NodeId>(), jf.at(1).get<std::string>()};
        PinRef to{jt.at(0).get<NodeId>(), jt.at(1).get<std::string>()};
        if (graph.addLink(std::move(from), std::move(to)) == 0)
            warn(warnings, "pipeline: link rejected (missing pin / type / already driven)");
    }

    return true;
}

} // namespace whiteout::textool::pipeline
