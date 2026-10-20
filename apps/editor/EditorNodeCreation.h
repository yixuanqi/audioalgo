#pragma once

#include "audio_editor_application/EditorNodeCommands.h"
#include "audio_editor_application/EditorGraphCommands.h"
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

[[nodiscard]] inline std::vector<NodeLibraryEntry> build_node_library_entries(const NodeCatalog& registry) {
    std::vector<NodeLibraryEntry> entries;
    entries.reserve(registry.descriptors().size());
    for (const auto& [type_id, descriptor] : registry.descriptors())
        entries.push_back({type_id, descriptor.display_name, descriptor.execution_kind,
                           default_node_parameters(descriptor)});
    return entries;
}

[[nodiscard]] inline bool try_add_node_from_descriptor(ProjectModel& project, const NodeCatalog& registry,
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
    if (!try_move_node_layout(project, node_id, x, y))
        return false;
    dirty_state.mark_modified();
    return true;
}

[[nodiscard]] inline bool delete_node(ProjectModel& project, const NodeCatalog& registry,
                                      const std::string& node_id) {
    return try_delete_node(project, registry, node_id);
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
