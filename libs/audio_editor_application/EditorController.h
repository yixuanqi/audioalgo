#pragma once

#include "audio_editor_application/EditorGraphCommands.h"
#include "audio_editor_application/EditorTopologyHistory.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace audioalgo::editor {

enum class ParameterEditStatus {
    started,
    staged,
    applied,
    rejected,
    unchanged,
    cancelled,
};

struct ParameterEditOutcome {
    ParameterEditStatus status = ParameterEditStatus::rejected;
    std::optional<GraphDiagnostic> diagnostic;
};

enum class ProjectEditStatus { applied, rejected, unchanged };

struct ProjectEditOutcome {
    ProjectEditStatus status = ProjectEditStatus::rejected;
    std::vector<GraphDiagnostic> diagnostics;
    std::optional<NodeInsertionResult> insertion;
};

struct ProjectSessionIdentity {
    std::string project_id;
    std::uint64_t generation = 0;
    bool operator==(const ProjectSessionIdentity&) const = default;
};

// A value copy for presentation. It contains no pointer or reference into the editable project.
struct NodeViewSnapshot {
    ProjectSessionIdentity session;
    NodeInstance node;
};

// Owns the edited project, the command/history stack and the presentation-visible state.
// It depends only on the pure SDK/project model: no JUCE, no component, no file dialog.
// The view sends intents here and renders the state it reads back.
class EditorController {
public:
    explicit EditorController(const NodeCatalog& catalog);

    // New/Open: replaces the edited project and resets history, dirty and stale state.
    void adopt_project(ProjectModel project);

    [[nodiscard]] const ProjectModel& project() const noexcept { return project_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return project_.revision; }
    [[nodiscard]] ProjectSessionIdentity session_identity() const { return {project_.project_id, session_generation_}; }
    [[nodiscard]] std::uint64_t document_generation() const noexcept { return document_generation_; }
    [[nodiscard]] std::optional<NodeViewSnapshot> node_snapshot(const std::string& node_id) const;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    // True when a newer computing revision exists than the last executed result.
    [[nodiscard]] bool results_stale() const noexcept { return results_stale_; }

    [[nodiscard]] ProjectEditOutcome add_node(const std::string& type_id, const std::string& node_id,
                                              double x, double y);
    [[nodiscard]] ProjectEditOutcome add_source_node(const std::string& type_id, const std::string& node_id,
                                                     const std::string& resource_path, double x, double y);
    [[nodiscard]] ProjectEditOutcome swap_processing_nodes(const std::string& first_node_id,
                                                          const std::string& second_node_id);
    [[nodiscard]] ProjectEditOutcome connect(const Connection& connection);
    [[nodiscard]] ProjectEditOutcome disconnect(const Connection& connection);
    [[nodiscard]] ProjectEditOutcome insert_node(const std::string& type_id, const std::string& node_id,
                                                 const Connection& target_connection, double x, double y,
                                                 std::optional<InsertionPortChoice> port_choice = std::nullopt);
    [[nodiscard]] ProjectEditOutcome insert_existing_node(const std::string& node_id,
                                                          const Connection& target_connection, double x, double y,
                                                          std::optional<InsertionPortChoice> port_choice = std::nullopt);
    [[nodiscard]] ProjectEditOutcome delete_node(const std::string& node_id);
    [[nodiscard]] ProjectEditOutcome move_node_layout(const std::string& node_id, double x, double y);
    // Port-level output selection (AR-14.c): one undoable revision that revalidates the
    // selected (node_id, port_id) against the descriptor schema before applying.
    [[nodiscard]] ProjectEditOutcome set_selected_output_port(const std::string& node_id,
                                                              const std::string& port_id);

    void mark_saved() noexcept { dirty_ = false; }
    [[nodiscard]] ProjectEditOutcome set_project_name(std::string name);
    // Called when a run finishes for the current revision.
    void mark_result_current() noexcept { results_stale_ = false; }

    // The single parameter mutation path. It validates against the shared descriptor schema and
    // applies through the topology history so one edit is exactly one undoable revision.
    [[nodiscard]] ParameterEditOutcome set_parameter(const std::string& node_id,
                                                     const std::string& parameter_id,
                                                     ParameterValue value);
    [[nodiscard]] ParameterEditOutcome set_parameter(const ProjectSessionIdentity& expected_session,
                                                     const std::string& node_id,
                                                     const std::string& parameter_id,
                                                     ParameterValue value);

    [[nodiscard]] ParameterEditOutcome begin_parameter_gesture(const std::string& node_id,
                                                               const std::string& parameter_id);
    [[nodiscard]] ParameterEditOutcome begin_parameter_gesture(const ProjectSessionIdentity& expected_session,
                                                               const std::string& node_id,
                                                               const std::string& parameter_id);
    [[nodiscard]] ParameterEditOutcome update_parameter_gesture(ParameterValue value);
    [[nodiscard]] ParameterEditOutcome update_parameter_gesture(const ProjectSessionIdentity& expected_session,
                                                               ParameterValue value);
    [[nodiscard]] ParameterEditOutcome commit_parameter_gesture();
    [[nodiscard]] ParameterEditOutcome commit_parameter_gesture(const ProjectSessionIdentity& expected_session);
    [[nodiscard]] ParameterEditOutcome cancel_parameter_gesture();
    [[nodiscard]] ParameterEditOutcome cancel_parameter_gesture(const ProjectSessionIdentity& expected_session);

    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    [[nodiscard]] const EditorTopologyHistory& history() const noexcept { return history_; }

private:
    struct ParameterGesture {
        ProjectSessionIdentity session;
        std::string node_id;
        std::string parameter_id;
        std::optional<ParameterValue> initial_value;
        std::optional<ParameterValue> draft_value;
    };

    [[nodiscard]] const NodeInstance* find_node(const std::string& node_id) const noexcept;
    [[nodiscard]] std::optional<GraphDiagnostic> validate_parameter_edit(const std::string& node_id,
                                                                          const std::string& parameter_id,
                                                                          const ParameterValue& value) const;
    void mark_edited() noexcept;
    [[nodiscard]] ProjectEditOutcome commit_insertion(ProjectModel candidate, NodeInsertionResult insertion);
    void mark_document_changed() noexcept;

    const NodeCatalog& catalog_;
    ProjectModel project_;
    std::uint64_t session_generation_ = 0;
    std::uint64_t document_generation_ = 0;
    EditorTopologyHistory history_;
    std::optional<ParameterGesture> parameter_gesture_;
    bool dirty_ = false;
    bool results_stale_ = false;
};

} // namespace audioalgo::editor
