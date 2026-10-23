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
            clear();
            return true;
        }
        if (!exists(project, node_id))
            return false;
        selected_node_id_ = node_id;
        selected_node_ids_ = {node_id};
        return true;
    }

    // Selects several validated nodes at once (rubber-band); the first id in the
    // given order becomes the primary selection driving the inspector.
    [[nodiscard]] bool select_many(const ProjectModel& project, const std::vector<std::string>& node_ids) {
        std::vector<std::string> chosen;
        for (const auto& node_id : node_ids) {
            if (node_id.empty() || std::find(chosen.begin(), chosen.end(), node_id) != chosen.end())
                continue;
            if (!exists(project, node_id))
                return false;
            chosen.push_back(node_id);
        }
        if (chosen.empty()) {
            clear();
            return true;
        }
        selected_node_ids_ = std::move(chosen);
        selected_node_id_ = selected_node_ids_.front();
        return true;
    }

    [[nodiscard]] const std::string& selected_node_id() const noexcept { return selected_node_id_; }
    [[nodiscard]] const std::vector<std::string>& selected_node_ids() const noexcept { return selected_node_ids_; }
    [[nodiscard]] bool is_selected(const std::string& node_id) const noexcept {
        return node_id == selected_node_id_ ||
               std::find(selected_node_ids_.begin(), selected_node_ids_.end(), node_id) != selected_node_ids_.end();
    }

private:
    static bool exists(const ProjectModel& project, const std::string& node_id) {
        return std::any_of(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        });
    }
    void clear() noexcept {
        selected_node_id_.clear();
        selected_node_ids_.clear();
    }

    std::string selected_node_id_;
    std::vector<std::string> selected_node_ids_;
};

} // namespace audioalgo::editor
