#pragma once

#include "EditorGeometry.h"
#include "EditorNodeCreation.h"
#include "audio_editor_application/EditorNodeCommands.h"
#include "audio_editor_application/EditorInsertionCommands.h"

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
    const ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const Connection& target_connection) {
    return find_node_insertion_port_choices(project, registry, type_id, target_connection);
}

[[nodiscard]] inline std::optional<LibraryNodeInsertionResult> try_insert_library_node_on_connection(
    ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const std::string& node_id, const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice = std::nullopt) {
    const auto result = apply_library_node_insertion(
        project, registry, type_id, node_id, target_connection, x, y, std::move(port_choice));
    if (!result)
        return std::nullopt;
    return LibraryNodeInsertionResult{result->node_id, result->input_port_id, result->output_port_id,
                                      {static_cast<float>(result->x), static_cast<float>(result->y)}};
}

[[nodiscard]] inline std::optional<LibraryNodeInsertionResult> try_insert_existing_node_on_connection(
    ProjectModel& project, const NodeCatalog& registry, const std::string& node_id,
    const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice = std::nullopt) {
    const auto result = apply_existing_node_insertion(
        project, registry, node_id, target_connection, x, y, std::move(port_choice));
    if (!result)
        return std::nullopt;
    return LibraryNodeInsertionResult{result->node_id, result->input_port_id, result->output_port_id,
                                      {static_cast<float>(result->x), static_cast<float>(result->y)}};
}

} // namespace audioalgo::editor
