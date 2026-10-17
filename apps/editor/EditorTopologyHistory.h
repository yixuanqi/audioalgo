#pragma once

#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace audioalgo::editor {

class EditorTopologyHistory {
public:
    template <typename Command>
    [[nodiscard]] bool apply(ProjectModel& project, Command&& command) {
        if (project.revision == std::numeric_limits<std::uint64_t>::max())
            return false;

        auto candidate = project;
        if (!static_cast<bool>(std::forward<Command>(command)(candidate)) || same_content(candidate, project))
            return false;

        candidate.revision = project.revision + 1;
        undo_.push_back({project, candidate});
        redo_.clear();
        project = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool undo(ProjectModel& project) {
        if (undo_.empty() || !same_topology(project, undo_.back().after) ||
            project.revision == std::numeric_limits<std::uint64_t>::max())
            return false;

        auto restored = restore_entry_state(project, undo_.back().before, undo_.back());
        restored.revision = project.revision + 1;
        redo_.push_back(undo_.back());
        undo_.pop_back();
        project = std::move(restored);
        return true;
    }

    [[nodiscard]] bool redo(ProjectModel& project) {
        if (redo_.empty() || !same_topology(project, redo_.back().before) ||
            project.revision == std::numeric_limits<std::uint64_t>::max())
            return false;

        auto restored = restore_entry_state(project, redo_.back().after, redo_.back());
        restored.revision = project.revision + 1;
        undo_.push_back(redo_.back());
        redo_.pop_back();
        project = std::move(restored);
        return true;
    }

    void clear() noexcept {
        undo_.clear();
        redo_.clear();
    }

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_size() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_size() const noexcept { return redo_.size(); }

private:
    struct Entry {
        ProjectModel before;
        ProjectModel after;
    };

    [[nodiscard]] static bool same_content(const ProjectModel& left, const ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.selected_output_node_id != right.selected_output_node_id ||
            left.nodes.size() != right.nodes.size() || left.connections.size() != right.connections.size())
            return false;

        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            const auto& a = left.nodes[index];
            const auto& b = right.nodes[index];
            if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters ||
                a.x != b.x || a.y != b.y)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& a = left.connections[index];
            const auto& b = right.connections[index];
            if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node ||
                a.to_port != b.to_port)
                return false;
        }
        return true;
    }

    [[nodiscard]] static bool same_topology(const ProjectModel& left, const ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.selected_output_node_id != right.selected_output_node_id ||
            left.nodes.size() != right.nodes.size() || left.connections.size() != right.connections.size())
            return false;

        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            if (left.nodes[index].id != right.nodes[index].id || left.nodes[index].type_id != right.nodes[index].type_id)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& a = left.connections[index];
            const auto& b = right.connections[index];
            if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node ||
                a.to_port != b.to_port)
                return false;
        }
        return true;
    }

    [[nodiscard]] static const NodeInstance* find_node(const ProjectModel& project, const std::string& id) {
        const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == id;
        });
        return found == project.nodes.end() ? nullptr : &*found;
    }

    [[nodiscard]] static bool position_changed_by_entry(const Entry& entry, const std::string& id) {
        const auto* before = find_node(entry.before, id);
        const auto* after = find_node(entry.after, id);
        return before != nullptr && after != nullptr && (before->x != after->x || before->y != after->y);
    }

    [[nodiscard]] static ProjectModel restore_entry_state(const ProjectModel& current,
                                                          const ProjectModel& target,
                                                          const Entry& entry) {
        auto restored = target;
        restored.schema_version = current.schema_version;
        restored.project_id = current.project_id;
        restored.name = current.name;
        for (auto& node : restored.nodes) {
            const auto* current_node = find_node(current, node.id);
            if (current_node == nullptr)
                continue;
            node.parameters = current_node->parameters;
            if (!position_changed_by_entry(entry, node.id)) {
                node.x = current_node->x;
                node.y = current_node->y;
            }
        }
        return restored;
    }

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

} // namespace audioalgo::editor
