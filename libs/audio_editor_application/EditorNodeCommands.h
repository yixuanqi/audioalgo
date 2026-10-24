#pragma once

#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audioalgo::editor {

[[nodiscard]] inline NodeParameters default_node_parameters(const NodeDescriptor& descriptor) {
    NodeParameters parameters;
    for (const auto& parameter : descriptor.parameters)
        parameters.emplace(parameter.id, parameter.default_value);
    return parameters;
}

[[nodiscard]] inline std::optional<NodeInstance> make_node_instance(
    const NodeCatalog& registry, const std::vector<NodeInstance>& existing_nodes,
    const std::string& type_id, const std::string& node_id, double x, double y) {
    const auto* descriptor = registry.find(type_id);
    if (descriptor == nullptr || node_id.empty())
        return std::nullopt;
    if (std::any_of(existing_nodes.begin(), existing_nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        }))
        return std::nullopt;
    NodeInstance node{node_id, type_id, default_node_parameters(*descriptor), x, y};
    if (descriptor->plugin_origin && !descriptor->plugin_origin->managed_content_sha256.empty()) {
        const auto& origin = *descriptor->plugin_origin;
        node.managed_version = NodeInstance::ManagedVersion{origin.plugin_id, origin.managed_content_sha256,
            origin.managed_package_path, origin.managed_wrapper_version};
    }
    return node;
}

[[nodiscard]] inline bool can_add_node_of_execution_kind(
    const ProjectModel& project, const NodeCatalog& registry, NodeExecutionKind requested_kind) {
    if (requested_kind != NodeExecutionKind::source && requested_kind != NodeExecutionKind::sink)
        return true;
    return std::none_of(project.nodes.begin(), project.nodes.end(), [&](const auto& existing) {
        const auto* descriptor = registry.find(existing.type_id);
        return descriptor != nullptr && descriptor->execution_kind == requested_kind;
    });
}

[[nodiscard]] inline bool try_swap_adjacent_processing_nodes(
    ProjectModel& project, const NodeCatalog& registry,
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
