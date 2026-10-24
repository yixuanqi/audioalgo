#include "audio_project/ProjectModel.h"
#include <filesystem>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace audioalgo {
namespace {

const PortDescriptor* find_port(const NodeDescriptor& descriptor, const std::string& port_id, PortDirection direction) {
    const auto it = std::find_if(descriptor.ports.begin(), descriptor.ports.end(), [&](const PortDescriptor& port) {
        return port.id == port_id && port.direction == direction;
    });
    return it == descriptor.ports.end() ? nullptr : &*it;
}

bool parameter_matches(ParameterKind kind, const ParameterValue& value) {
    switch (kind) {
    case ParameterKind::boolean: return std::holds_alternative<bool>(value);
    case ParameterKind::integer: return std::holds_alternative<std::int64_t>(value);
    case ParameterKind::floating: return std::holds_alternative<double>(value);
    case ParameterKind::enumeration:
    case ParameterKind::string:
    case ParameterKind::path: return std::holds_alternative<std::string>(value);
    }
    return false;
}

} // namespace

std::vector<GraphDiagnostic> validate_managed_version_locks(const ProjectModel& project) {
    std::vector<GraphDiagnostic> errors;
    std::map<std::string, std::string> types, packages;
    for (const auto& node : project.nodes) {
        std::string key;
        if (node.managed_version) {
            const auto& v = *node.managed_version;
            const auto invalid = [&] { errors.push_back({"managed_version_invalid", "Invalid exact package version lock", node.id, {}}); };
            if (project.schema_version != 2 || v.package_id.empty() || v.wrapper_version.empty() || v.content_sha256.size() != 64 ||
                !std::all_of(v.content_sha256.begin(), v.content_sha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) invalid();
            const auto path = std::filesystem::u8path(v.package_path);
            if (path.empty() || path.is_absolute() || path.has_root_path() || path.lexically_normal() != path ||
                std::any_of(path.begin(), path.end(), [](const auto& p) { return p == ".."; })) invalid();
            key = v.package_id + ":" + v.content_sha256;
            const auto [entry, inserted] = packages.emplace(v.package_id, v.content_sha256);
            if (!inserted && entry->second != v.content_sha256)
                errors.push_back({"managed_version_conflict", "One project cannot bind two versions of the same package", node.id, {}});
        }
        const auto [entry, inserted] = types.emplace(node.type_id, key);
        if (!inserted && entry->second != key)
            errors.push_back({"managed_version_conflict", "One project cannot bind two versions of the same logical type", node.id, {}});
    }
    return errors;
}

static std::vector<GraphDiagnostic> validate_project_impl(const ProjectModel& project, const NodeCatalog& registry,
                                                          bool require_complete_inputs) {
    std::vector<GraphDiagnostic> diagnostics = validate_managed_version_locks(project);
    std::map<std::string, const NodeInstance*, std::less<>> nodes;
    std::map<std::string, const NodeDescriptor*, std::less<>> descriptors;

    for (const auto& node : project.nodes) {
        if (node.id.empty()) {
            diagnostics.push_back({"node_id_empty", "Node ID must not be empty", {}, {}});
            continue;
        }
        if (!nodes.emplace(node.id, &node).second) {
            diagnostics.push_back({"node_id_duplicate", "Node IDs must be unique", node.id, {}});
            continue;
        }
        const auto* descriptor = registry.find(node.type_id);
        if (descriptor == nullptr) {
            diagnostics.push_back({"node_type_missing", "No registered descriptor for node type " + node.type_id, node.id, {}});
            continue;
        }
        descriptors.emplace(node.id, descriptor);
        if (node.managed_version) {
            const auto& v = *node.managed_version;
            if (!descriptor->plugin_origin || descriptor->plugin_origin->plugin_id != v.package_id ||
                descriptor->plugin_origin->managed_content_sha256 != v.content_sha256 ||
                descriptor->plugin_origin->managed_wrapper_version != v.wrapper_version)
                diagnostics.push_back({"managed_version_unavailable", "The exact managed package version has not been resolved", node.id, {}});
        } else if (descriptor->plugin_origin && !descriptor->plugin_origin->managed_content_sha256.empty()) {
            diagnostics.push_back({"managed_version_required", "A managed node requires an exact project version lock", node.id, {}});
        }

        for (const auto& parameter : node.opaque_parameters)
            diagnostics.push_back({"parameter_value_unsupported",
                                   "The current host cannot interpret this serialized parameter value",
                                   node.id, parameter.name});

        for (const auto& parameter : node.parameters) {
            const auto spec = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(), [&](const ParameterDescriptor& candidate) {
                return candidate.id == parameter.first;
            });
            if (spec == descriptor->parameters.end()) {
                diagnostics.push_back({"parameter_unknown", "Unknown parameter " + parameter.first, node.id, parameter.first});
                continue;
            }
            if (!parameter_matches(spec->kind, parameter.second)) {
                diagnostics.push_back({"parameter_type", "Parameter has the wrong value type", node.id, parameter.first});
                continue;
            }
            // Value-level rules (range, finiteness, enum membership) come from the shared SDK schema
            // so descriptors, saved projects, controller commands and the runtime cannot drift apart.
            if (const auto value_error = validate_parameter_value(*spec, parameter.second)) {
                diagnostics.push_back({value_error->code, value_error->message, node.id, parameter.first});
            }
        }
    }

    std::set<std::tuple<std::string, std::string, std::string, std::string>> unique_connections;
    std::map<std::pair<std::string, std::string>, std::size_t> input_counts;
    std::map<std::string, std::vector<std::string>, std::less<>> adjacency;
    std::map<std::string, std::size_t, std::less<>> indegree;
    for (const auto& [id, node] : nodes) {
        (void)node;
        adjacency[id];
        indegree[id] = 0;
    }

    for (const auto& connection : project.connections) {
        if (!unique_connections.emplace(connection.from_node, connection.from_port, connection.to_node, connection.to_port).second) {
            diagnostics.push_back({"connection_duplicate", "Connection is duplicated", connection.to_node, connection.to_port});
            continue;
        }
        if (connection.from_node == connection.to_node)
            diagnostics.push_back({"connection_self", "A node cannot connect to itself", connection.to_node, connection.to_port});
        const auto source_node = nodes.find(connection.from_node);
        const auto target_node = nodes.find(connection.to_node);
        if (source_node == nodes.end()) {
            diagnostics.push_back({"connection_source_missing", "Connection source node does not exist", connection.from_node, connection.from_port});
            continue;
        }
        if (target_node == nodes.end()) {
            diagnostics.push_back({"connection_target_missing", "Connection target node does not exist", connection.to_node, connection.to_port});
            continue;
        }
        const auto source_descriptor = descriptors.find(connection.from_node);
        const auto target_descriptor = descriptors.find(connection.to_node);
        if (source_descriptor == descriptors.end() || target_descriptor == descriptors.end())
            continue;
        const auto* source_port = find_port(*source_descriptor->second, connection.from_port, PortDirection::output);
        const auto* target_port = find_port(*target_descriptor->second, connection.to_port, PortDirection::input);
        if (source_port == nullptr) {
            diagnostics.push_back({"connection_source_port_missing", "Output port does not exist", connection.from_node, connection.from_port});
            continue;
        }
        if (target_port == nullptr) {
            diagnostics.push_back({"connection_target_port_missing", "Input port does not exist", connection.to_node, connection.to_port});
            continue;
        }
        if (source_port->type_id != target_port->type_id) {
            diagnostics.push_back({"connection_type_mismatch", "Port types do not match", connection.to_node, connection.to_port});
            continue;
        }
        auto& count = input_counts[{connection.to_node, connection.to_port}];
        ++count;
        if (count > 1)
            diagnostics.push_back({"input_connection_duplicate", "An input port accepts at most one connection", connection.to_node, connection.to_port});
        if (connection.from_node != connection.to_node) {
            adjacency[connection.from_node].push_back(connection.to_node);
            ++indegree[connection.to_node];
        }
    }

    if (require_complete_inputs) {
        for (const auto& [node_id, descriptor] : descriptors) {
            for (const auto& port : descriptor->ports) {
                if (port.direction == PortDirection::input && port.required && input_counts[{node_id, port.id}] == 0)
                    diagnostics.push_back({"input_required", "Required input is not connected", node_id, port.id});
            }
        }
    }

    std::vector<std::string> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0) ready.push_back(id);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto current = ready.back();
        ready.pop_back();
        ++visited;
        for (const auto& next : adjacency[current])
            if (--indegree[next] == 0) ready.push_back(next);
    }
    if (visited != nodes.size())
        diagnostics.push_back({"graph_cycle", "Graph must be acyclic", {}, {}});

    if (!project.selected_output_node_id.empty() && !nodes.contains(project.selected_output_node_id))
        diagnostics.push_back({"selected_output_missing", "Selected output node does not exist", project.selected_output_node_id, {}});
    if (!project.selected_output_port_id.empty() && nodes.contains(project.selected_output_node_id) &&
        descriptors.contains(project.selected_output_node_id)) {
        const auto& descriptor = *descriptors.find(project.selected_output_node_id)->second;
        const auto& port_id = project.selected_output_port_id;
        const auto* port = find_port(descriptor, port_id, PortDirection::output);
        if (port == nullptr) {
            const auto* input_port = find_port(descriptor, port_id, PortDirection::input);
            diagnostics.push_back({input_port != nullptr ? "selected_output_port_direction"
                                                          : "selected_output_port_missing",
                                   input_port != nullptr
                                       ? "Selected output port must be an output port"
                                       : "Selected output port does not exist",
                                   project.selected_output_node_id, port_id});
        } else if (port->type_id != "audio.pcm@1") {
            diagnostics.push_back({"selected_output_port_type",
                                   "Selected output port must be an audio.pcm@1 port",
                                   project.selected_output_node_id, port_id});
        }
    }
    return diagnostics;
}

std::vector<GraphDiagnostic> validate_project(const ProjectModel& project, const NodeCatalog& registry) {
    return validate_project_impl(project, registry, true);
}

std::vector<GraphDiagnostic> validate_project_structure(const ProjectModel& project, const NodeCatalog& registry) {
    return validate_project_impl(project, registry, false);
}

std::vector<GraphDiagnostic> try_add_connection(ProjectModel& project, Connection connection,
                                                const NodeCatalog& registry) {
    auto candidate = project;
    candidate.connections.push_back(std::move(connection));
    auto diagnostics = validate_project_structure(candidate, registry);
    if (!diagnostics.empty())
        return diagnostics;

    ++candidate.revision;
    project = std::move(candidate);
    return {};
}

std::vector<std::string> topological_order(const ProjectModel& project, const NodeCatalog& registry,
                                           std::vector<GraphDiagnostic>& diagnostics) {
    diagnostics = validate_project(project, registry);
    if (!diagnostics.empty())
        return {};

    std::map<std::string, std::vector<std::string>, std::less<>> adjacency;
    std::map<std::string, std::size_t, std::less<>> indegree;
    for (const auto& node : project.nodes) {
        adjacency[node.id];
        indegree[node.id] = 0;
    }
    for (const auto& connection : project.connections) {
        adjacency[connection.from_node].push_back(connection.to_node);
        ++indegree[connection.to_node];
    }
    std::vector<std::string> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0) ready.push_back(id);
    std::vector<std::string> order;
    while (!ready.empty()) {
        const auto current = ready.back();
        ready.pop_back();
        order.push_back(current);
        for (const auto& next : adjacency[current])
            if (--indegree[next] == 0) ready.push_back(next);
    }
    return order;
}

} // namespace audioalgo
