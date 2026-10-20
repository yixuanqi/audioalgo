#pragma once

#include "audio_editor_application/EditorGraphCommands.h"
#include "audio_editor_application/EditorNodeCommands.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audioalgo::editor {

[[nodiscard]] inline std::vector<InsertionPortChoice> find_node_insertion_port_choices(
    const ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const Connection& target_connection) {
    const auto target_count = std::count_if(project.connections.begin(), project.connections.end(), [&](const auto& edge) {
        return edge.from_node == target_connection.from_node && edge.from_port == target_connection.from_port &&
               edge.to_node == target_connection.to_node && edge.to_port == target_connection.to_port;
    });
    const auto* descriptor = registry.find(type_id);
    if (target_count != 1 || descriptor == nullptr || descriptor->execution_kind != NodeExecutionKind::block)
        return {};

    const auto source = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == target_connection.from_node;
    });
    const auto target = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == target_connection.to_node;
    });
    if (source == project.nodes.end() || target == project.nodes.end())
        return {};
    const auto* source_descriptor = registry.find(source->type_id);
    const auto* target_descriptor = registry.find(target->type_id);
    if (source_descriptor == nullptr || target_descriptor == nullptr)
        return {};
    const auto source_port = std::find_if(source_descriptor->ports.begin(), source_descriptor->ports.end(),
        [&](const auto& port) { return port.id == target_connection.from_port && port.direction == PortDirection::output; });
    const auto target_port = std::find_if(target_descriptor->ports.begin(), target_descriptor->ports.end(),
        [&](const auto& port) { return port.id == target_connection.to_port && port.direction == PortDirection::input; });
    if (source_port == source_descriptor->ports.end() || target_port == target_descriptor->ports.end())
        return {};

    std::vector<InsertionPortChoice> choices;
    for (const auto& input : descriptor->ports) {
        if (input.direction != PortDirection::input || input.type_id != source_port->type_id)
            continue;
        for (const auto& output : descriptor->ports) {
            if (output.direction == PortDirection::output && output.type_id == target_port->type_id)
                choices.push_back({input.id, output.id});
        }
    }
    return choices;
}

[[nodiscard]] inline std::optional<NodeInsertionResult> apply_node_insertion_on_connection(
    ProjectModel& project, const NodeCatalog& registry, NodeInstance inserted_node, bool reuse_existing_node,
    const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice) {
    const auto matches_target = [&](const auto& edge) {
        return edge.from_node == target_connection.from_node && edge.from_port == target_connection.from_port &&
               edge.to_node == target_connection.to_node && edge.to_port == target_connection.to_port;
    };
    const auto target_edge = std::find_if(project.connections.begin(), project.connections.end(), matches_target);
    if (target_edge == project.connections.end() ||
        std::count_if(project.connections.begin(), project.connections.end(), matches_target) != 1)
        return std::nullopt;

    const auto* descriptor = registry.find(inserted_node.type_id);
    if (descriptor == nullptr || descriptor->execution_kind != NodeExecutionKind::block)
        return std::nullopt;

    const auto port_choices = find_node_insertion_port_choices(
        project, registry, inserted_node.type_id, target_connection);
    if (port_choices.empty())
        return std::nullopt;
    const auto chosen = port_choice
        ? std::find_if(port_choices.begin(), port_choices.end(), [&](const auto& option) {
              return option.input_port_id == port_choice->input_port_id &&
                     option.output_port_id == port_choice->output_port_id;
          })
        : (port_choices.size() == 1 ? port_choices.begin() : port_choices.end());
    if (chosen == port_choices.end())
        return std::nullopt;
    const auto selected_ports = *chosen;

    const auto matching_node_count = std::count_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == inserted_node.id;
    });
    if (reuse_existing_node) {
        if (matching_node_count != 1 || std::any_of(project.connections.begin(), project.connections.end(),
            [&](const auto& edge) { return edge.from_node == inserted_node.id || edge.to_node == inserted_node.id; }))
            return std::nullopt;
    } else if (matching_node_count != 0) {
        return std::nullopt;
    }

    const auto inserted_node_id = inserted_node.id;
    auto candidate = project;
    const auto candidate_edge = std::find_if(candidate.connections.begin(), candidate.connections.end(), matches_target);
    if (candidate_edge == candidate.connections.end())
        return std::nullopt;
    candidate.connections.erase(candidate_edge);
    if (reuse_existing_node) {
        const auto candidate_node = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& node) {
            return node.id == inserted_node_id;
        });
        if (candidate_node == candidate.nodes.end())
            return std::nullopt;
        candidate_node->x = x;
        candidate_node->y = y;
    } else {
        inserted_node.x = x;
        inserted_node.y = y;
        candidate.nodes.push_back(std::move(inserted_node));
    }
    candidate.connections.push_back({target_connection.from_node, target_connection.from_port,
                                     inserted_node_id, selected_ports.input_port_id});
    candidate.connections.push_back({inserted_node_id, selected_ports.output_port_id,
                                     target_connection.to_node, target_connection.to_port});

    if (!validate_project_structure(candidate, registry).empty())
        return std::nullopt;
    candidate.revision = project.revision + 1;
    project = std::move(candidate);
    return NodeInsertionResult{inserted_node_id, selected_ports.input_port_id, selected_ports.output_port_id, x, y};
}

[[nodiscard]] inline std::optional<NodeInsertionResult> apply_library_node_insertion(
    ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const std::string& node_id, const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice = std::nullopt) {
    const auto node = make_node_instance(registry, project.nodes, type_id, node_id, x, y);
    if (!node)
        return std::nullopt;
    return apply_node_insertion_on_connection(
        project, registry, *node, false, target_connection, x, y, std::move(port_choice));
}

[[nodiscard]] inline std::optional<NodeInsertionResult> apply_existing_node_insertion(
    ProjectModel& project, const NodeCatalog& registry, const std::string& node_id,
    const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice = std::nullopt) {
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
        return candidate.id == node_id;
    });
    if (node == project.nodes.end() ||
        std::count_if(project.nodes.begin(), project.nodes.end(), [&](const auto& candidate) {
            return candidate.id == node_id;
        }) != 1)
        return std::nullopt;
    return apply_node_insertion_on_connection(
        project, registry, *node, true, target_connection, x, y, std::move(port_choice));
}

} // namespace audioalgo::editor
