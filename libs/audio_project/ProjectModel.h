#pragma once

#include "audio_node_sdk/NodeSDK.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audioalgo {

struct NodeInstance {
    std::string id;
    std::string type_id;
    NodeParameters parameters;
    double x = 0.0;
    double y = 0.0;
};

struct Connection {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
};

struct ProjectModel {
    std::uint32_t schema_version = 1;
    std::string project_id;
    std::string name;
    std::uint64_t revision = 0;
    std::vector<NodeInstance> nodes;
    std::vector<Connection> connections;
    std::string selected_output_node_id;
};

struct GraphDiagnostic {
    std::string code;
    std::string message;
    std::string node_id;
    std::string port_id;
};

[[nodiscard]] std::vector<GraphDiagnostic> validate_project(const ProjectModel& project, const NodeRegistry& registry);
[[nodiscard]] std::vector<GraphDiagnostic> validate_project_structure(const ProjectModel& project,
                                                                      const NodeRegistry& registry);
[[nodiscard]] std::vector<GraphDiagnostic> try_add_connection(ProjectModel& project, Connection connection,
                                                              const NodeRegistry& registry);
[[nodiscard]] std::vector<std::string> topological_order(const ProjectModel& project, const NodeRegistry& registry,
                                                           std::vector<GraphDiagnostic>& diagnostics);

} // namespace audioalgo
