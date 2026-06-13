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
    if (!graph.category().empty())
        doc["category"] = graph.category();
    // Multilingual display strings travel inside the file (pipelines are
    // transferable).  Written only when the pipeline is marked Multilingual;
    // entries are display-only overrides — canonical names stay authoritative.
    if (graph.multilingual()) {
        doc["multilingual"] = true;
        json i18n = json::object();
        for (const auto& [code, tr] : graph.translations()) {
            if (tr.empty())
                continue;
            json entry = json::object();
            if (!tr.name.empty())
                entry["name"] = tr.name;
            if (!tr.category.empty())
                entry["category"] = tr.category;
            if (!tr.ports.empty()) {
                json ports = json::object();
                for (const auto& [port, label] : tr.ports)
                    if (!label.empty())
                        ports[port] = label;
                if (!ports.empty())
                    entry["ports"] = std::move(ports);
            }
            if (!entry.empty())
                i18n[code] = std::move(entry);
        }
        if (!i18n.empty())
            doc["i18n"] = std::move(i18n);
    }
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

        // Dynamic-pin nodes (e.g. Subpipeline) don't re-derive their pins from
        // the ctor, so persist them — names+types are link-bearing on load.
        if (n->hasDynamicPins()) {
            const auto dumpPins = [](std::span<const Pin> pins) {
                json arr = json::array();
                for (const Pin& p : pins)
                    arr.push_back({{"name", p.name}, {"type", pinTypeName(p.type)}});
                return arr;
            };
            jn["pins"] = {{"inputs", dumpPins(n->inputs())},
                          {"outputs", dumpPins(n->outputs())}};
        }

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
    graph.setCategory(doc.value("category", std::string{}));
    // Optional multilingual block (absent in classic pipelines — they load
    // unchanged).  A file carrying an i18n table is multilingual even without
    // the explicit flag.
    if (const auto it = doc.find("i18n"); it != doc.end() && it->is_object()) {
        for (const auto& [code, entry] : it->items()) {
            if (!entry.is_object())
                continue;
            PipelineTranslation tr;
            tr.name = entry.value("name", std::string{});
            tr.category = entry.value("category", std::string{});
            if (const auto pit = entry.find("ports"); pit != entry.end() && pit->is_object())
                for (const auto& [port, label] : pit->items())
                    if (label.is_string())
                        tr.ports[port] = label.get<std::string>();
            if (!tr.empty())
                graph.translations()[code] = std::move(tr);
        }
    }
    graph.setMultilingual(doc.value("multilingual", false) || !graph.translations().empty());
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

        // Restore a dynamic-pin node's persisted pins before links are added,
        // so this node's link endpoints resolve against the right pin set.
        if (node->hasDynamicPins()) {
            if (auto it = jn.find("pins"); it != jn.end() && it->is_object()) {
                const auto loadPins = [](const json& arr, bool is_input) {
                    std::vector<Pin> pins;
                    for (const auto& jp : arr)
                        pins.push_back({jp.value("name", std::string{}),
                                        pinTypeFromName(jp.value("type", std::string{"rgba"})),
                                        is_input});
                    return pins;
                };
                node->setPins(loadPins(it->value("inputs", json::array()), true),
                              loadPins(it->value("outputs", json::array()), false));
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
