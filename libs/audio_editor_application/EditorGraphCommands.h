#pragma once

#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace audioalgo::editor {

struct InsertionPortChoice {
    std::string input_port_id;
    std::string output_port_id;
};

struct NodeInsertionResult {
    std::string node_id;
    std::string input_port_id;
    std::string output_port_id;
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] inline bool try_move_node_layout(ProjectModel& project, const std::string& node_id, double x,
                                               double y) {
    if (node_id.empty() || !std::isfinite(x) || !std::isfinite(y) ||
        std::count_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        }) != 1)
        return false;
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
        return candidate.id == node_id;
    });
    if (node->x == x && node->y == y)
        return false;
    node->x = x;
    node->y = y;
    return true;
}

[[nodiscard]] inline bool try_disconnect_connection(ProjectModel& project,
                                                   const Connection& selected_connection) {
    const auto matches = [&](const auto& edge) {
        return edge.from_node == selected_connection.from_node && edge.from_port == selected_connection.from_port &&
               edge.to_node == selected_connection.to_node && edge.to_port == selected_connection.to_port;
    };
    const auto connection = std::find_if(project.connections.begin(), project.connections.end(), matches);
    if (connection == project.connections.end() ||
        std::count_if(project.connections.begin(), project.connections.end(), matches) != 1 ||
        project.revision == std::numeric_limits<std::uint64_t>::max())
        return false;
    project.connections.erase(connection);
    ++project.revision;
    return true;
}

[[nodiscard]] inline bool try_delete_node(ProjectModel& project, const NodeCatalog& registry,
                                          const std::string& node_id) {
    const auto node_count = std::count_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == node_id;
    });
    if (node_id.empty() || node_count != 1 || project.revision == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
        return candidate.id == node_id;
    });

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
    const auto candidate_node = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& item) {
        return item.id == node_id;
    });
    candidate.nodes.erase(candidate_node);
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

    if (candidate.selected_output_node_id == node_id) {
        candidate.selected_output_node_id.clear();
        candidate.selected_output_port_id.clear();
    }
    candidate.revision = project.revision + 1;
    project = std::move(candidate);
    return true;
}

} // namespace audioalgo::editor
