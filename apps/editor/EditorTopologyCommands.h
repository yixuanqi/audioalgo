#pragma once

#include "EditorGeometry.h"
#include "EditorNodeCreation.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audioalgo::editor {

struct LibraryNodeInsertionResult {
    std::string node_id;
    std::string input_port_id;
    std::string output_port_id;
    EditorPoint position;
};

enum class LibraryDropRoute { independent_node, source_chooser, insert_on_connection, reject };

struct InsertionPortChoice {
    std::string input_port_id;
    std::string output_port_id;
};

[[nodiscard]] inline LibraryDropRoute decide_library_drop_route(
    const NodeDescriptor* descriptor, bool has_connection_target) noexcept {
    if (descriptor == nullptr)
        return LibraryDropRoute::reject;
    if (has_connection_target)
        return descriptor->execution_kind == NodeExecutionKind::block
            ? LibraryDropRoute::insert_on_connection : LibraryDropRoute::reject;
    return descriptor->execution_kind == NodeExecutionKind::source
        ? LibraryDropRoute::source_chooser : LibraryDropRoute::independent_node;
}

[[nodiscard]] inline std::vector<InsertionPortChoice> find_library_insertion_port_choices(
    const ProjectModel& project, const NodeRegistry& registry, const std::string& type_id,
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

namespace detail {

[[nodiscard]] inline std::optional<LibraryNodeInsertionResult> try_insert_node_on_connection(
    ProjectModel& project, const NodeRegistry& registry, NodeInstance inserted_node, bool reuse_existing_node,
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

    const auto port_choices = find_library_insertion_port_choices(
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
    return LibraryNodeInsertionResult{inserted_node_id, selected_ports.input_port_id,
                                      selected_ports.output_port_id,
                                      {static_cast<float>(x), static_cast<float>(y)}};
}

} // namespace detail

[[nodiscard]] inline std::optional<LibraryNodeInsertionResult> try_insert_library_node_on_connection(
    ProjectModel& project, const NodeRegistry& registry, const std::string& type_id,
    const std::string& node_id, const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice = std::nullopt) {
    const auto node = make_node_instance(registry, project.nodes, type_id, node_id, x, y);
    if (!node)
        return std::nullopt;
    return detail::try_insert_node_on_connection(
        project, registry, *node, false, target_connection, x, y, std::move(port_choice));
}

[[nodiscard]] inline std::optional<LibraryNodeInsertionResult> try_insert_existing_node_on_connection(
    ProjectModel& project, const NodeRegistry& registry, const std::string& node_id,
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
    return detail::try_insert_node_on_connection(
        project, registry, *node, true, target_connection, x, y, std::move(port_choice));
}

[[nodiscard]] inline bool try_swap_adjacent_processing_nodes(
    ProjectModel& project, const NodeRegistry& registry,
    const std::string& first_node_id, const std::string& second_node_id) {
    if (first_node_id.empty() || second_node_id.empty() || first_node_id == second_node_id ||
        project.revision == std::numeric_limits<std::uint64_t>::max())
        return false;

    const auto count_node = [&](const std::string& node_id) {
        return std::count_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        });
    };
    if (count_node(first_node_id) != 1 || count_node(second_node_id) != 1)
        return false;
    const auto first_node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == first_node_id;
    });
    const auto second_node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == second_node_id;
    });
    const auto* first_descriptor = registry.find(first_node->type_id);
    const auto* second_descriptor = registry.find(second_node->type_id);
    if (first_descriptor == nullptr || second_descriptor == nullptr ||
        first_descriptor->execution_kind != NodeExecutionKind::block ||
        second_descriptor->execution_kind != NodeExecutionKind::block)
        return false;

    std::vector<const Connection*> incoming_first;
    std::vector<const Connection*> outgoing_first;
    std::vector<const Connection*> incoming_second;
    std::vector<const Connection*> outgoing_second;
    for (const auto& edge : project.connections) {
        if (edge.to_node == first_node_id)
            incoming_first.push_back(&edge);
        if (edge.from_node == first_node_id)
            outgoing_first.push_back(&edge);
        if (edge.to_node == second_node_id)
            incoming_second.push_back(&edge);
        if (edge.from_node == second_node_id)
            outgoing_second.push_back(&edge);
    }
    const auto same_edge = [](const Connection& a, const Connection& b) {
        return a.from_node == b.from_node && a.from_port == b.from_port &&
               a.to_node == b.to_node && a.to_port == b.to_port;
    };
    if (incoming_first.size() != 1 || outgoing_first.size() != 1 ||
        incoming_second.size() != 1 || outgoing_second.size() != 1 ||
        !same_edge(*outgoing_first.front(), *incoming_second.front()) ||
        incoming_first.front()->from_node == first_node_id ||
        incoming_first.front()->from_node == second_node_id ||
        outgoing_second.front()->to_node == first_node_id ||
        outgoing_second.front()->to_node == second_node_id)
        return false;

    const auto previous = *incoming_first.front();
    const auto between = *outgoing_first.front();
    const auto next = *outgoing_second.front();
    auto candidate = project;
    const auto is_swapped_edge = [&](const Connection& edge) {
        return same_edge(edge, previous) || same_edge(edge, between) || same_edge(edge, next);
    };
    std::erase_if(candidate.connections, is_swapped_edge);
    if (candidate.connections.size() + 3 != project.connections.size())
        return false;
    candidate.connections.push_back(
        {previous.from_node, previous.from_port, second_node_id, between.to_port});
    candidate.connections.push_back(
        {second_node_id, next.from_port, first_node_id, previous.to_port});
    candidate.connections.push_back(
        {first_node_id, between.from_port, next.to_node, next.to_port});

    const auto candidate_first = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& node) {
        return node.id == first_node_id;
    });
    const auto candidate_second = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& node) {
        return node.id == second_node_id;
    });
    std::swap(candidate_first->x, candidate_second->x);
    std::swap(candidate_first->y, candidate_second->y);
    if (!validate_project_structure(candidate, registry).empty())
        return false;

    candidate.revision = project.revision + 1;
    project = std::move(candidate);
    return true;
}

} // namespace audioalgo::editor
