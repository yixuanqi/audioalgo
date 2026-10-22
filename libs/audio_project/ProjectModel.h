#pragma once

#include "audio_node_sdk/NodeSDK.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audioalgo {

// JSON retained by the host without interpreting its value. This keeps plugin- and
// version-specific project data round-trippable without adding JUCE to the model.
struct OpaqueJsonField {
    std::string name;
    std::string value_json;
    bool operator==(const OpaqueJsonField&) const = default;
};

struct NodeInstance {
    std::string id;
    std::string type_id;
    NodeParameters parameters;
    double x = 0.0;
    double y = 0.0;
    std::vector<OpaqueJsonField> opaque_parameters;
    std::vector<OpaqueJsonField> serialized_extensions;
};

struct Connection {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    std::vector<OpaqueJsonField> serialized_extensions;
};

struct ProjectModel {
    std::uint32_t schema_version = 1;
    std::string project_id;
    std::string name;
    std::uint64_t revision = 0;
    std::vector<NodeInstance> nodes;
    std::vector<Connection> connections;
    std::string selected_output_node_id;
    // Optional port-level selection: when set, the selected output is this output port of the
    // selected node (any node kind); when empty, legacy node-level selection applies (sink node).
    std::string selected_output_port_id;
    std::vector<OpaqueJsonField> serialized_extensions;
};

struct GraphDiagnostic {
    std::string code;
    std::string message;
    std::string node_id;
    std::string port_id;
};

[[nodiscard]] std::vector<GraphDiagnostic> validate_project(const ProjectModel& project, const NodeCatalog& registry);
[[nodiscard]] std::vector<GraphDiagnostic> validate_project_structure(const ProjectModel& project,
                                                                      const NodeCatalog& registry);
[[nodiscard]] std::vector<GraphDiagnostic> try_add_connection(ProjectModel& project, Connection connection,
                                                              const NodeCatalog& registry);
[[nodiscard]] std::vector<std::string> topological_order(const ProjectModel& project, const NodeCatalog& registry,
                                                           std::vector<GraphDiagnostic>& diagnostics);

} // namespace audioalgo
