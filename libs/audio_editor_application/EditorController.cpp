#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/EditorGraphCommands.h"
#include "audio_editor_application/EditorInsertionCommands.h"
#include "audio_editor_application/EditorNodeCommands.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace audioalgo::editor {
namespace {

GraphDiagnostic parameter_diagnostic(std::string code, std::string message, const std::string& node_id,
                                     const std::string& parameter_id) {
    return GraphDiagnostic{std::move(code), std::move(message), node_id, parameter_id};
}

ProjectEditOutcome project_edit_rejected(std::string code, std::string message, std::string node_id = {},
                                         std::string port_id = {}) {
    return {ProjectEditStatus::rejected,
            {{std::move(code), std::move(message), std::move(node_id), std::move(port_id)}}};
}

} // namespace

EditorController::EditorController(const NodeCatalog& catalog) : catalog_(catalog) {}

void EditorController::adopt_project(ProjectModel project) {
    project_ = std::move(project);
    ++session_generation_;
    if (session_generation_ == 0)
        ++session_generation_;
    mark_document_changed();
    history_.clear();
    parameter_gesture_.reset();
    dirty_ = false;
    results_stale_ = false;
}

ProjectEditOutcome EditorController::set_project_name(std::string name) {
    if (project_.name == name)
        return {ProjectEditStatus::unchanged, {}};
    project_.name = std::move(name);
    dirty_ = true;
    mark_document_changed();
    return {ProjectEditStatus::applied, {}};
}

std::optional<NodeViewSnapshot> EditorController::node_snapshot(const std::string& node_id) const {
    const auto* node = find_node(node_id);
    if (node == nullptr)
        return std::nullopt;
    return NodeViewSnapshot{session_identity(), *node};
}

const NodeInstance* EditorController::find_node(const std::string& node_id) const noexcept {
    const auto found = std::find_if(project_.nodes.begin(), project_.nodes.end(), [&](const NodeInstance& node) {
        return node.id == node_id;
    });
    return found == project_.nodes.end() ? nullptr : &*found;
}

std::optional<GraphDiagnostic> EditorController::validate_parameter_edit(const std::string& node_id,
                                                                         const std::string& parameter_id,
                                                                         const ParameterValue& value) const {
    const auto* node = find_node(node_id);
    if (node == nullptr)
        return parameter_diagnostic("node_missing", "No node with this ID exists in the project", node_id,
                                    parameter_id);

    const auto* descriptor = catalog_.find(node->type_id);
    if (descriptor == nullptr)
        return parameter_diagnostic("node_type_missing", "No registered descriptor for node type " + node->type_id,
                                    node_id, parameter_id);

    const auto spec = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
                                   [&](const ParameterDescriptor& candidate) {
                                       return candidate.id == parameter_id;
                                   });
    if (spec == descriptor->parameters.end())
        return parameter_diagnostic("parameter_unknown", "Unknown parameter " + parameter_id, node_id,
                                    parameter_id);

    if (const auto error = validate_parameter_value(*spec, value))
        return parameter_diagnostic(error->code, error->message, node_id, parameter_id);
    return std::nullopt;
}

void EditorController::mark_edited() noexcept {
    dirty_ = true;
    results_stale_ = true;
    mark_document_changed();
}

void EditorController::mark_document_changed() noexcept {
    ++document_generation_;
    if (document_generation_ == 0)
        ++document_generation_;
}

ManagedUpgradePreview EditorController::preview_managed_upgrade(const NodeCatalog& target, const std::string& package_id) const {
    ManagedUpgradePreview preview{session_identity(), document_generation_, package_id};
    auto candidate = project_;
    for (auto& node : candidate.nodes) {
        if (!node.managed_version || node.managed_version->package_id != package_id) continue;
        const auto* before = catalog_.find(node.type_id);
        const auto* after = target.find(node.type_id);
        if (!before || !after || !after->plugin_origin || after->plugin_origin->plugin_id != package_id ||
            after->plugin_origin->managed_content_sha256.empty()) {
            preview.diagnostics.push_back({"managed_upgrade_type_missing", "The package no longer provides this type", node.id});
            continue;
        }
        const auto& origin = *after->plugin_origin;
        preview.instances.push_back({node.id, node.type_id, node.managed_version->content_sha256, origin.managed_content_sha256});
        bool same_ports = before->ports.size() == after->ports.size() && before->execution_kind == after->execution_kind;
        for (const auto& port : before->ports) {
            const auto match = std::find_if(after->ports.begin(), after->ports.end(), [&](const PortDescriptor& next) {
                return port.id == next.id && port.type_id == next.type_id && port.direction == next.direction && port.required == next.required;
            });
            same_ports = same_ports && match != after->ports.end();
        }
        if (!same_ports) preview.diagnostics.push_back({"managed_upgrade_ports_incompatible", "Port changes require a new logical type; existing connections are preserved", node.id});
        for (const auto& parameter : before->parameters) {
            const auto match = std::find_if(after->parameters.begin(), after->parameters.end(), [&](const ParameterDescriptor& next) {
                return parameter.id == next.id && parameter.kind == next.kind;
            });
            if (match == after->parameters.end())
                preview.diagnostics.push_back({"managed_upgrade_parameter_incompatible", "Parameter removal or kind changes require a new logical type", node.id, parameter.id});
        }
        node.managed_version->content_sha256 = origin.managed_content_sha256;
        node.managed_version->package_path = origin.managed_package_path;
        node.managed_version->wrapper_version = origin.managed_wrapper_version;
    }
    auto diagnostics = validate_project_structure(candidate, target);
    preview.diagnostics.insert(preview.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    return preview;
}

ProjectEditOutcome EditorController::commit_managed_upgrade(const ManagedUpgradePreview& expected, const NodeCatalog& target) {
    if (expected.session != session_identity() || expected.document_generation != document_generation_)
        return project_edit_rejected("project_session_stale", "The project changed after the upgrade preview");
    if (parameter_gesture_) return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture");
    if (project_.revision == std::numeric_limits<std::uint64_t>::max()) return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced");
    const auto preview = preview_managed_upgrade(target, expected.package_id);
    if (preview.instances != expected.instances) return project_edit_rejected("managed_upgrade_stale", "The target version changed after the preview");
    if (!preview.diagnostics.empty()) return {ProjectEditStatus::rejected, preview.diagnostics};
    const bool changed = std::any_of(preview.instances.begin(), preview.instances.end(), [](const auto& instance) { return instance.from_sha256 != instance.to_sha256; });
    if (!changed) return {ProjectEditStatus::unchanged, {}};
    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        for (auto& node : edited.nodes) if (node.managed_version && node.managed_version->package_id == expected.package_id) {
            const auto& origin = *target.find(node.type_id)->plugin_origin;
            node.managed_version->content_sha256 = origin.managed_content_sha256;
            node.managed_version->package_path = origin.managed_package_path;
            node.managed_version->wrapper_version = origin.managed_wrapper_version;
        }
        return true;
    });
    if (!applied) return {ProjectEditStatus::unchanged, {}};
    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::commit_insertion(ProjectModel candidate, NodeInsertionResult insertion) {
    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited = std::move(candidate);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}, std::move(insertion)};
}

ProjectEditOutcome EditorController::add_node(const std::string& type_id, const std::string& node_id,
                                              double x, double y) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced", node_id);

    const auto* descriptor = catalog_.find(type_id);
    if (descriptor == nullptr)
        return project_edit_rejected("node_type_missing", "No registered descriptor for node type " + type_id,
                                     node_id);
    if (descriptor->execution_kind == NodeExecutionKind::source)
        return project_edit_rejected("source_requires_resource",
                                     "Source nodes must be created with a selected resource", node_id);
    if (!can_add_node_of_execution_kind(project_, catalog_, descriptor->execution_kind))
        return project_edit_rejected("node_kind_duplicate", "A node of this execution kind already exists", node_id);

    const auto node = make_node_instance(catalog_, project_.nodes, type_id, node_id, x, y);
    if (!node)
        return project_edit_rejected("node_id_invalid", "The node ID is empty or already exists", node_id);

    auto candidate = project_;
    candidate.nodes.push_back(*node);
    if (node->managed_version) candidate.schema_version = 2;
    if (descriptor->execution_kind == NodeExecutionKind::sink)
        candidate.selected_output_node_id = node_id;
    auto diagnostics = validate_project_structure(candidate, catalog_);
    if (!diagnostics.empty())
        return {ProjectEditStatus::rejected, std::move(diagnostics)};

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited.nodes.push_back(*node);
        if (node->managed_version) edited.schema_version = 2;
        if (descriptor->execution_kind == NodeExecutionKind::sink)
            edited.selected_output_node_id = node_id;
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::set_selected_output_port(const std::string& node_id,
                                                               const std::string& port_id) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced", node_id);
    if (project_.selected_output_node_id == node_id && project_.selected_output_port_id == port_id)
        return {ProjectEditStatus::unchanged, {}};

    auto candidate = project_;
    candidate.selected_output_node_id = node_id;
    candidate.selected_output_port_id = port_id;
    auto diagnostics = validate_project_structure(candidate, catalog_);
    if (!diagnostics.empty())
        return {ProjectEditStatus::rejected, std::move(diagnostics)};

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited.selected_output_node_id = node_id;
        edited.selected_output_port_id = port_id;
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::add_source_node(const std::string& type_id, const std::string& node_id,
                                                      const std::string& resource_path, double x, double y) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced", node_id);
    if (resource_path.empty())
        return project_edit_rejected("source_resource_missing", "Source nodes require a selected resource", node_id);

    const auto* descriptor = catalog_.find(type_id);
    if (descriptor == nullptr)
        return project_edit_rejected("node_type_missing", "No registered descriptor for node type " + type_id,
                                     node_id);
    if (descriptor->execution_kind != NodeExecutionKind::source)
        return project_edit_rejected("node_kind_not_source", "The requested node is not a source", node_id);
    if (!can_add_node_of_execution_kind(project_, catalog_, NodeExecutionKind::source))
        return project_edit_rejected("node_kind_duplicate", "A source node already exists in the project", node_id);

    const auto path_parameter = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
        [](const auto& parameter) { return parameter.kind == ParameterKind::path; });
    if (path_parameter == descriptor->parameters.end())
        return project_edit_rejected("source_path_parameter_missing",
                                     "The source descriptor does not declare a path parameter", node_id);
    auto node = make_node_instance(catalog_, project_.nodes, type_id, node_id, x, y);
    if (!node)
        return project_edit_rejected("node_id_invalid", "The node ID is empty or already exists", node_id);
    node->parameters[path_parameter->id] = resource_path;

    auto candidate = project_;
    candidate.nodes.push_back(*node);
    const auto diagnostics = validate_project_structure(candidate, catalog_);
    if (!diagnostics.empty())
        return {ProjectEditStatus::rejected, diagnostics};

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited.nodes.push_back(*node);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};
    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::swap_processing_nodes(const std::string& first_node_id,
                                                            const std::string& second_node_id) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     first_node_id);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced", first_node_id);
    const auto applied = history_.apply(project_, [&](ProjectModel& candidate) {
        return try_swap_adjacent_processing_nodes(candidate, catalog_, first_node_id, second_node_id);
    });
    if (!applied)
        return project_edit_rejected("node_swap_invalid", "The selected nodes cannot be swapped", first_node_id);
    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::connect(const Connection& connection) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     connection.to_node, connection.to_port);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced",
                                     connection.to_node, connection.to_port);

    auto candidate = project_;
    auto diagnostics = try_add_connection(candidate, connection, catalog_);
    if (!diagnostics.empty())
        return {ProjectEditStatus::rejected, std::move(diagnostics)};

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited = std::move(candidate);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::disconnect(const Connection& connection) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     connection.to_node, connection.to_port);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced",
                                     connection.to_node, connection.to_port);

    const auto matches = [&](const auto& edge) {
        return edge.from_node == connection.from_node && edge.from_port == connection.from_port &&
               edge.to_node == connection.to_node && edge.to_port == connection.to_port;
    };
    const auto count = std::count_if(project_.connections.begin(), project_.connections.end(), matches);
    if (count == 0)
        return project_edit_rejected("connection_missing", "The selected connection does not exist",
                                     connection.to_node, connection.to_port);
    if (count != 1)
        return project_edit_rejected("connection_ambiguous", "The selected connection is not unique",
                                     connection.to_node, connection.to_port);

    auto candidate = project_;
    if (!try_disconnect_connection(candidate, connection))
        return project_edit_rejected("connection_missing", "The selected connection could not be removed",
                                     connection.to_node, connection.to_port);

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited = std::move(candidate);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::delete_node(const std::string& node_id) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id);
    const auto node_count = std::count_if(project_.nodes.begin(), project_.nodes.end(), [&](const auto& node) {
        return node.id == node_id;
    });
    if (node_count == 0)
        return project_edit_rejected("node_missing", "The selected node does not exist", node_id);
    if (node_count != 1)
        return project_edit_rejected("node_id_ambiguous", "The selected node ID is not unique", node_id);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced", node_id);

    auto candidate = project_;
    if (!try_delete_node(candidate, catalog_, node_id))
        return project_edit_rejected("node_delete_invalid", "The node could not be deleted", node_id);

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited = std::move(candidate);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::delete_nodes(const std::vector<std::string>& node_ids) {
    if (node_ids.empty())
        return {ProjectEditStatus::unchanged, {}};
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_ids.front());
    for (const auto& node_id : node_ids) {
        const auto node_count = std::count_if(project_.nodes.begin(), project_.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        });
        if (node_count != 1)
            return project_edit_rejected("node_missing", "A selected node does not exist or is not unique", node_id);
    }
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced",
                                     node_ids.front());

    auto candidate = project_;
    for (const auto& node_id : node_ids)
        if (!try_delete_node(candidate, catalog_, node_id))
            return project_edit_rejected("node_delete_invalid", "A selected node could not be deleted", node_id);

    const auto applied = history_.apply(project_, [&](ProjectModel& edited) {
        edited = std::move(candidate);
        return true;
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    mark_edited();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::move_node_layout(const std::string& node_id, double x, double y) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id);
    const auto node_count = std::count_if(project_.nodes.begin(), project_.nodes.end(), [&](const auto& node) {
        return node.id == node_id;
    });
    if (node_count == 0)
        return project_edit_rejected("node_missing", "The selected node does not exist", node_id);
    if (node_count != 1)
        return project_edit_rejected("node_id_ambiguous", "The selected node ID is not unique", node_id);
    if (!std::isfinite(x) || !std::isfinite(y))
        return project_edit_rejected("node_position_invalid", "Node coordinates must be finite", node_id);

    const auto* current_node = find_node(node_id);
    if (current_node == nullptr)
        return project_edit_rejected("node_missing", "The selected node does not exist", node_id);
    if (current_node->x == x && current_node->y == y)
        return {ProjectEditStatus::unchanged, {}};

    const auto applied = history_.apply_layout(project_, [&](ProjectModel& candidate) {
        return try_move_node_layout(candidate, node_id, x, y);
    });
    if (!applied)
        return {ProjectEditStatus::unchanged, {}};

    dirty_ = true;
    mark_document_changed();
    return {ProjectEditStatus::applied, {}};
}

ProjectEditOutcome EditorController::insert_node(const std::string& type_id, const std::string& node_id,
                                                  const Connection& target_connection, double x, double y,
                                                  std::optional<InsertionPortChoice> port_choice) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     target_connection.to_node, target_connection.to_port);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced",
                                     target_connection.to_node, target_connection.to_port);

    const auto matches_target = [&](const auto& edge) {
        return edge.from_node == target_connection.from_node && edge.from_port == target_connection.from_port &&
               edge.to_node == target_connection.to_node && edge.to_port == target_connection.to_port;
    };
    const auto target_count = std::count_if(project_.connections.begin(), project_.connections.end(), matches_target);
    if (target_count == 0)
        return project_edit_rejected("connection_target_missing", "The selected connection no longer exists",
                                     target_connection.to_node, target_connection.to_port);
    if (target_count != 1)
        return project_edit_rejected("connection_target_ambiguous", "The selected connection is not unique",
                                     target_connection.to_node, target_connection.to_port);

    const auto* descriptor = catalog_.find(type_id);
    if (descriptor == nullptr)
        return project_edit_rejected("node_type_missing", "No registered descriptor for node type " + type_id,
                                     node_id);
    if (descriptor->execution_kind != NodeExecutionKind::block)
        return project_edit_rejected("node_kind_not_insertable", "Only block nodes can be inserted on a connection",
                                     node_id);

    const auto node = make_node_instance(catalog_, project_.nodes, type_id, node_id, x, y);
    if (!node)
        return project_edit_rejected("node_id_invalid", "The node ID is empty or already exists", node_id);

    const auto choices = find_node_insertion_port_choices(project_, catalog_, type_id, target_connection);
    if (choices.empty())
        return project_edit_rejected("connection_type_mismatch", "No compatible insertion port pair exists",
                                     target_connection.to_node, target_connection.to_port);
    if (!port_choice && choices.size() != 1)
        return project_edit_rejected("connection_insertion_choice_required",
                                     "Select a compatible input/output port pair", node_id);
    if (port_choice && std::none_of(choices.begin(), choices.end(), [&](const auto& choice) {
            return choice.input_port_id == port_choice->input_port_id &&
                   choice.output_port_id == port_choice->output_port_id;
        }))
        return project_edit_rejected("connection_insertion_choice_invalid",
                                     "The selected insertion port pair is not compatible", node_id,
                                     port_choice->input_port_id);

    auto candidate = project_;
    const auto insertion = apply_node_insertion_on_connection(
        candidate, catalog_, *node, false, target_connection, x, y, std::move(port_choice));
    if (!insertion)
        return project_edit_rejected("connection_insertion_invalid",
                                     "The candidate insertion does not form a valid project structure",
                                     target_connection.to_node, target_connection.to_port);

    return commit_insertion(std::move(candidate), *insertion);
}

ProjectEditOutcome EditorController::insert_existing_node(
    const std::string& node_id, const Connection& target_connection, double x, double y,
    std::optional<InsertionPortChoice> port_choice) {
    if (parameter_gesture_.has_value())
        return project_edit_rejected("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     target_connection.to_node, target_connection.to_port);
    if (project_.revision == std::numeric_limits<std::uint64_t>::max())
        return project_edit_rejected("revision_exhausted", "The project revision cannot be advanced",
                                     target_connection.to_node, target_connection.to_port);

    const auto matches_target = [&](const auto& edge) {
        return edge.from_node == target_connection.from_node && edge.from_port == target_connection.from_port &&
               edge.to_node == target_connection.to_node && edge.to_port == target_connection.to_port;
    };
    const auto target_count = std::count_if(project_.connections.begin(), project_.connections.end(), matches_target);
    if (target_count == 0)
        return project_edit_rejected("connection_target_missing", "The selected connection no longer exists",
                                     target_connection.to_node, target_connection.to_port);
    if (target_count != 1)
        return project_edit_rejected("connection_target_ambiguous", "The selected connection is not unique",
                                     target_connection.to_node, target_connection.to_port);

    const auto node_count = std::count_if(project_.nodes.begin(), project_.nodes.end(), [&](const auto& node) {
        return node.id == node_id;
    });
    if (node_count == 0)
        return project_edit_rejected("node_missing", "The insertion node does not exist", node_id);
    if (node_count != 1)
        return project_edit_rejected("node_id_ambiguous", "The insertion node ID is not unique", node_id);

    const auto* node = find_node(node_id);
    const auto* descriptor = node == nullptr ? nullptr : catalog_.find(node->type_id);
    if (descriptor == nullptr)
        return project_edit_rejected("node_type_missing", "No registered descriptor exists for the insertion node",
                                     node_id);
    if (descriptor->execution_kind != NodeExecutionKind::block)
        return project_edit_rejected("node_kind_not_insertable", "Only block nodes can be inserted on a connection",
                                     node_id);
    if (std::any_of(project_.connections.begin(), project_.connections.end(), [&](const auto& edge) {
            return edge.from_node == node_id || edge.to_node == node_id;
        }))
        return project_edit_rejected("insertion_node_connected", "The existing insertion node must be unconnected",
                                     node_id);

    const auto choices = find_node_insertion_port_choices(project_, catalog_, node->type_id, target_connection);
    if (choices.empty())
        return project_edit_rejected("connection_type_mismatch", "No compatible insertion port pair exists",
                                     target_connection.to_node, target_connection.to_port);
    if (!port_choice && choices.size() != 1)
        return project_edit_rejected("connection_insertion_choice_required",
                                     "Select a compatible input/output port pair", node_id);
    if (port_choice && std::none_of(choices.begin(), choices.end(), [&](const auto& choice) {
            return choice.input_port_id == port_choice->input_port_id &&
                   choice.output_port_id == port_choice->output_port_id;
        }))
        return project_edit_rejected("connection_insertion_choice_invalid",
                                     "The selected insertion port pair is not compatible", node_id,
                                     port_choice->input_port_id);

    auto candidate = project_;
    const auto insertion = apply_existing_node_insertion(
        candidate, catalog_, node_id, target_connection, x, y, std::move(port_choice));
    if (!insertion)
        return project_edit_rejected("connection_insertion_invalid",
                                     "The candidate insertion does not form a valid project structure",
                                     target_connection.to_node, target_connection.to_port);

    return commit_insertion(std::move(candidate), *insertion);
}

ParameterEditOutcome EditorController::set_parameter(const std::string& node_id,
                                                    const std::string& parameter_id,
                                                    ParameterValue value) {
    if (parameter_gesture_.has_value())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_gesture_active", "Finish or cancel the active parameter gesture",
                                     node_id, parameter_id)};

    if (auto error = validate_parameter_edit(node_id, parameter_id, value))
        return {ParameterEditStatus::rejected, std::move(error)};

    const auto* node = find_node(node_id);
    if (node == nullptr)
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("node_missing", "No node with this ID exists in the project", node_id,
                                     parameter_id)};
    const auto current = node->parameters.find(parameter_id);
    if (current != node->parameters.end() && current->second == value)
        return {ParameterEditStatus::unchanged, std::nullopt};

    const auto applied = history_.apply(project_, [&](ProjectModel& candidate) {
        const auto target = std::find_if(candidate.nodes.begin(), candidate.nodes.end(),
                                         [&](const NodeInstance& instance) { return instance.id == node_id; });
        if (target == candidate.nodes.end())
            return false;
        target->parameters[parameter_id] = value;
        return true;
    });
    if (!applied)
        return {ParameterEditStatus::unchanged, std::nullopt};

    mark_edited();
    return {ParameterEditStatus::applied, std::nullopt};
}

ParameterEditOutcome EditorController::set_parameter(const ProjectSessionIdentity& expected_session,
                                                       const std::string& node_id,
                                                       const std::string& parameter_id,
                                                       ParameterValue value) {
    if (expected_session != session_identity())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("project_session_stale",
                                     "The view intent belongs to a replaced project session", node_id,
                                     parameter_id)};
    return set_parameter(node_id, parameter_id, std::move(value));
}

ParameterEditOutcome EditorController::begin_parameter_gesture(const std::string& node_id,
                                                               const std::string& parameter_id) {
    return begin_parameter_gesture(session_identity(), node_id, parameter_id);
}

ParameterEditOutcome EditorController::begin_parameter_gesture(const ProjectSessionIdentity& expected_session,
                                                               const std::string& node_id,
                                                               const std::string& parameter_id) {
    if (expected_session != session_identity())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("project_session_stale", "The project changed before this parameter edit",
                                     node_id, parameter_id)};
    if (parameter_gesture_.has_value())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_gesture_active", "A parameter gesture is already active", node_id,
                                     parameter_id)};
    const auto* node = find_node(node_id);
    if (node == nullptr)
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("node_missing", "No node with this ID exists in the project", node_id,
                                     parameter_id)};
    const auto* descriptor = catalog_.find(node->type_id);
    if (descriptor == nullptr)
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("node_type_missing", "No registered descriptor for node type " + node->type_id,
                                     node_id, parameter_id)};
    if (std::none_of(descriptor->parameters.begin(), descriptor->parameters.end(), [&](const ParameterDescriptor& spec) {
            return spec.id == parameter_id;
        }))
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_unknown", "No registered parameter matches this ID", node_id,
                                     parameter_id)};
    const auto current = node->parameters.find(parameter_id);
    const std::optional<ParameterValue> initial =
        current == node->parameters.end() ? std::nullopt : std::optional<ParameterValue>{current->second};
    parameter_gesture_ = ParameterGesture{expected_session, node_id, parameter_id, initial, initial};
    return {ParameterEditStatus::started, std::nullopt};
}

ParameterEditOutcome EditorController::update_parameter_gesture(ParameterValue value) {
    return update_parameter_gesture(session_identity(), std::move(value));
}

ParameterEditOutcome EditorController::update_parameter_gesture(const ProjectSessionIdentity& expected_session,
                                                               ParameterValue value) {
    if (expected_session != session_identity() ||
        (parameter_gesture_.has_value() && expected_session != parameter_gesture_->session)) {
        const auto node_id = parameter_gesture_ ? parameter_gesture_->node_id : std::string{};
        const auto parameter_id = parameter_gesture_ ? parameter_gesture_->parameter_id : std::string{};
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("project_session_stale", "The project changed before this parameter edit",
                                     node_id, parameter_id)};
    }
    if (!parameter_gesture_.has_value())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_gesture_missing", "No parameter gesture is active", {}, {})};
    if (auto error = validate_parameter_edit(parameter_gesture_->node_id, parameter_gesture_->parameter_id, value))
        return {ParameterEditStatus::rejected, std::move(error)};
    parameter_gesture_->draft_value = std::move(value);
    return {ParameterEditStatus::staged, std::nullopt};
}

ParameterEditOutcome EditorController::commit_parameter_gesture() {
    return commit_parameter_gesture(session_identity());
}

ParameterEditOutcome EditorController::commit_parameter_gesture(const ProjectSessionIdentity& expected_session) {
    if (expected_session != session_identity() ||
        (parameter_gesture_.has_value() && expected_session != parameter_gesture_->session)) {
        const auto node_id = parameter_gesture_ ? parameter_gesture_->node_id : std::string{};
        const auto parameter_id = parameter_gesture_ ? parameter_gesture_->parameter_id : std::string{};
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("project_session_stale", "The project changed before this parameter edit",
                                     node_id, parameter_id)};
    }
    if (!parameter_gesture_.has_value())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_gesture_missing", "No parameter gesture is active", {}, {})};
    if (parameter_gesture_->initial_value == parameter_gesture_->draft_value) {
        parameter_gesture_.reset();
        return {ParameterEditStatus::unchanged, std::nullopt};
    }

    const auto gesture = *parameter_gesture_;
    if (gesture.draft_value) {
        if (auto error = validate_parameter_edit(gesture.node_id, gesture.parameter_id, *gesture.draft_value))
            return {ParameterEditStatus::rejected, std::move(error)};
    }
    const auto applied = history_.apply(project_, [&](ProjectModel& candidate) {
        const auto target = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const NodeInstance& node) {
            return node.id == gesture.node_id;
        });
        if (target == candidate.nodes.end())
            return false;
        if (gesture.draft_value)
            target->parameters[gesture.parameter_id] = *gesture.draft_value;
        else
            target->parameters.erase(gesture.parameter_id);
        return true;
    });
    if (!applied)
        return {ParameterEditStatus::unchanged, std::nullopt};
    parameter_gesture_.reset();
    mark_edited();
    return {ParameterEditStatus::applied, std::nullopt};
}

ParameterEditOutcome EditorController::cancel_parameter_gesture() {
    return cancel_parameter_gesture(session_identity());
}

ParameterEditOutcome EditorController::cancel_parameter_gesture(const ProjectSessionIdentity& expected_session) {
    if (expected_session != session_identity() ||
        (parameter_gesture_.has_value() && expected_session != parameter_gesture_->session)) {
        const auto node_id = parameter_gesture_ ? parameter_gesture_->node_id : std::string{};
        const auto parameter_id = parameter_gesture_ ? parameter_gesture_->parameter_id : std::string{};
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("project_session_stale", "The project changed before this parameter edit",
                                     node_id, parameter_id)};
    }
    if (!parameter_gesture_.has_value())
        return {ParameterEditStatus::rejected,
                parameter_diagnostic("parameter_gesture_missing", "No parameter gesture is active", {}, {})};
    parameter_gesture_.reset();
    return {ParameterEditStatus::cancelled, std::nullopt};
}

bool EditorController::undo() {
    if (parameter_gesture_.has_value())
        return false;
    const auto changes_compute_revision = history_.undo_changes_compute_revision();
    if (!history_.undo(project_))
        return false;
    dirty_ = true;
    mark_document_changed();
    if (changes_compute_revision)
        results_stale_ = true;
    return true;
}

bool EditorController::redo() {
    if (parameter_gesture_.has_value())
        return false;
    const auto changes_compute_revision = history_.redo_changes_compute_revision();
    if (!history_.redo(project_))
        return false;
    dirty_ = true;
    mark_document_changed();
    if (changes_compute_revision)
        results_stale_ = true;
    return true;
}

} // namespace audioalgo::editor
