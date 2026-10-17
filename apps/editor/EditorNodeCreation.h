#pragma once

#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace audioalgo::editor {

struct NodeLibraryEntry {
    std::string type_id;
    std::string display_name;
    NodeExecutionKind execution_kind = NodeExecutionKind::block;
    NodeParameters default_parameters;
};

[[nodiscard]] inline NodeParameters default_node_parameters(const NodeDescriptor& descriptor) {
    NodeParameters parameters;
    for (const auto& parameter : descriptor.parameters)
        parameters.emplace(parameter.id, parameter.default_value);
    return parameters;
}

[[nodiscard]] inline std::vector<NodeLibraryEntry> build_node_library_entries(const NodeRegistry& registry) {
    std::vector<NodeLibraryEntry> entries;
    entries.reserve(registry.descriptors().size());
    for (const auto& [type_id, descriptor] : registry.descriptors())
        entries.push_back({type_id, descriptor.display_name, descriptor.execution_kind,
                           default_node_parameters(descriptor)});
    return entries;
}

[[nodiscard]] inline std::optional<NodeInstance> make_node_instance(
    const NodeRegistry& registry, const std::vector<NodeInstance>& existing_nodes,
    const std::string& type_id, const std::string& node_id, double x, double y) {
    const auto* descriptor = registry.find(type_id);
    if (descriptor == nullptr || node_id.empty())
        return std::nullopt;
    if (std::any_of(existing_nodes.begin(), existing_nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        }))
        return std::nullopt;
    return NodeInstance{node_id, type_id, default_node_parameters(*descriptor), x, y};
}

[[nodiscard]] inline bool can_add_node_of_execution_kind(const ProjectModel& project, const NodeRegistry& registry,
                                                         NodeExecutionKind requested_kind) {
    if (requested_kind != NodeExecutionKind::source && requested_kind != NodeExecutionKind::sink)
        return true;
    return std::none_of(project.nodes.begin(), project.nodes.end(), [&](const auto& existing) {
        const auto* descriptor = registry.find(existing.type_id);
        return descriptor != nullptr && descriptor->execution_kind == requested_kind;
    });
}

[[nodiscard]] inline bool try_add_node_from_descriptor(ProjectModel& project, const NodeRegistry& registry,
                                                       const std::string& type_id, const std::string& node_id,
                                                       double x, double y) {
    const auto* descriptor = registry.find(type_id);
    if (descriptor == nullptr || descriptor->execution_kind == NodeExecutionKind::source)
        return false;
    if (!can_add_node_of_execution_kind(project, registry, descriptor->execution_kind))
        return false;

    auto node = make_node_instance(registry, project.nodes, type_id, node_id, x, y);
    if (!node)
        return false;
    project.nodes.push_back(std::move(*node));
    if (descriptor->execution_kind == NodeExecutionKind::sink)
        project.selected_output_node_id = node_id;
    ++project.revision;
    return true;
}

class ProjectDirtyState {
public:
    void mark_modified() noexcept { modified_ = true; }
    void mark_saved() noexcept { modified_ = false; }
    [[nodiscard]] bool is_modified() const noexcept { return modified_; }

private:
    bool modified_ = false;
};

[[nodiscard]] inline bool move_node_layout(ProjectModel& project, ProjectDirtyState& dirty_state,
                                           const std::string& node_id, double x, double y) {
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
        return candidate.id == node_id;
    });
    if (node == project.nodes.end() || (node->x == x && node->y == y))
        return false;
    node->x = x;
    node->y = y;
    dirty_state.mark_modified();
    return true;
}

[[nodiscard]] inline bool delete_node(ProjectModel& project, const NodeRegistry& registry,
                                      const std::string& node_id) {
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
        return candidate.id == node_id;
    });
    if (node == project.nodes.end())
        return false;

    std::optional<Connection> incoming;
    std::optional<Connection> outgoing;
    auto bypass_insert_position = std::size_t{0};
    bool found_incoming_position = false;
    auto incoming_count = std::size_t{0};
    auto outgoing_count = std::size_t{0};
    for (const auto& connection : project.connections) {
        const bool incident = connection.from_node == node_id || connection.to_node == node_id;
        if (connection.to_node == node_id) {
            if (incoming_count++ == 0)
                incoming = connection;
            found_incoming_position = true;
        }
        if (connection.from_node == node_id) {
            if (outgoing_count++ == 0)
                outgoing = connection;
        }
        if (!found_incoming_position && !incident)
            ++bypass_insert_position;
    }
    if (incoming_count != 1)
        incoming.reset();
    if (outgoing_count != 1)
        outgoing.reset();
    if (incoming && outgoing) {
        const auto* descriptor = registry.find(node->type_id);
        if (descriptor == nullptr || descriptor->execution_kind != NodeExecutionKind::block)
            incoming.reset();
    }

    auto candidate = project;
    candidate.nodes.erase(std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& item) {
        return item.id == node_id;
    }));
    std::erase_if(candidate.connections, [&](const auto& connection) {
        return connection.from_node == node_id || connection.to_node == node_id;
    });

    if (incoming && outgoing && incoming->from_node != outgoing->to_node) {
        auto bypassed = candidate;
        const auto position = std::min(bypass_insert_position, bypassed.connections.size());
        bypassed.connections.insert(bypassed.connections.begin() + static_cast<std::ptrdiff_t>(position),
                                    {incoming->from_node, incoming->from_port, outgoing->to_node,
                                     outgoing->to_port});
        if (validate_project_structure(bypassed, registry).empty())
            candidate = std::move(bypassed);
    }

    if (candidate.selected_output_node_id == node_id)
        candidate.selected_output_node_id.clear();
    candidate.revision = project.revision + 1;
    project = std::move(candidate);
    return true;
}

[[nodiscard]] inline bool try_disconnect_connection(ProjectModel& project, const Connection& selected_connection) {
    const auto connection = std::find_if(project.connections.begin(), project.connections.end(), [&](const auto& edge) {
        return edge.from_node == selected_connection.from_node && edge.from_port == selected_connection.from_port &&
               edge.to_node == selected_connection.to_node && edge.to_port == selected_connection.to_port;
    });
    if (connection == project.connections.end())
        return false;
    project.connections.erase(connection);
    ++project.revision;
    return true;
}

template <typename IdGenerator>
[[nodiscard]] inline std::optional<std::string> make_unique_node_id(
    const std::vector<NodeInstance>& existing_nodes, IdGenerator&& generate) {
    constexpr std::size_t max_attempts = 64;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        auto id = generate();
        if (id.empty())
            continue;
        const bool already_used = std::any_of(existing_nodes.begin(), existing_nodes.end(), [&](const auto& node) {
            return node.id == id;
        });
        if (!already_used)
            return id;
    }
    return std::nullopt;
}

class NodeSelectionState {
public:
    [[nodiscard]] bool select(const ProjectModel& project, const std::string& node_id) {
        if (node_id.empty()) {
            selected_node_id_.clear();
            return true;
        }
        const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        });
        if (found == project.nodes.end())
            return false;
        selected_node_id_ = node_id;
        return true;
    }

    [[nodiscard]] const std::string& selected_node_id() const noexcept { return selected_node_id_; }

private:
    std::string selected_node_id_;
};

} // namespace audioalgo::editor
