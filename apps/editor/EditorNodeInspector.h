#pragma once

#include "EditorNodeCreation.h"
#include "EditorNodeParameterBinding.h"
#include "EditorParameterPanel.h"
#include "EditorText.h"
#include "EditorUiSupport.h"

#include <algorithm>
#include <vector>

namespace audioalgo::editor {

class NodeInspector final : public juce::Component {
public:
    NodeInspector(audioalgo::editor::EditorController& controller,
                  audioalgo::editor::NodeSelectionState& selection, std::function<void()> changed)
        : controller_(controller), selection_(selection), changed_(std::move(changed)) {
        addAndMakeVisible(panel_);
        addAndMakeVisible(disconnect_all_button_);
        disconnect_all_button_.setComponentID("inspector.disconnect-all-connections");
        disconnect_all_button_.setTooltip(
            "Remove all connections to or from this node; keep the node and its settings.");
        disconnect_all_button_.onClick = [this] { disconnect_all_connections(); };
        disconnect_all_button_.setVisible(false);
        addAndMakeVisible(disconnect_selected_button_);
        disconnect_selected_button_.setComponentID("inspector.disconnect-selected-node-connections");
        disconnect_selected_button_.setTooltip(
            "Remove connections touching the selected nodes; keep the nodes and their settings.");
        disconnect_selected_button_.onClick = [this] { disconnect_selected_connections(); };
        disconnect_selected_button_.setVisible(false);
    }

    void show_node(const std::string& node_id) {
        if (active_gesture_parameter_) {
            if (snapshot_)
                (void)controller_.cancel_parameter_gesture(snapshot_->session);
            active_gesture_parameter_.reset();
        }
        // Presentation must not replace the canvas's multi-selection with its primary node.
        snapshot_ = controller_.node_snapshot(node_id);
        if (!snapshot_) {
            disconnect_all_button_.setVisible(false);
            disconnect_selected_button_.setVisible(false);
            panel_.clear("Inspector", node_id.empty() ? "Select a node to edit its parameters." : "Node not found.");
            resized();
            return;
        }
        update_disconnect_buttons();

        const auto& node = snapshot_->node;
        const auto* descriptor = descriptors_->find(node.type_id);
        if (descriptor == nullptr) {
            panel_.clear("Missing node", utf8_string(node.type_id + "\nID " + node.id));
            resized();
            return;
        }
        const audioalgo::editor::EditorNodeParameterBinding binding(controller_, *snapshot_);
        auto details=utf8_string(node.type_id + "\nID " + node.id);
        if(const auto notice=native_user_log_notice(*descriptor);notice.isNotEmpty())details+="\n"+notice;
        panel_.show_parameters(*descriptor, node.parameters,
            [this, binding](std::string parameter_id, audioalgo::ParameterValue value) {
                apply_parameter(binding, std::move(parameter_id), std::move(value));
            },
            [this, binding](std::string parameter_id, audioalgo::editor::ParameterGesturePhase phase,
                   audioalgo::ParameterValue value) {
                apply_parameter_gesture(binding, std::move(parameter_id), phase, std::move(value));
            },
            details);
        std::vector<std::pair<std::string, std::string>> opaque;
        for (const auto& field : node.opaque_parameters)
            opaque.emplace_back(field.name, field.value_json);
        panel_.show_opaque_parameters(opaque);
    }

    void set_descriptor_registry(const audioalgo::NodeCatalog& descriptors) noexcept { descriptors_ = &descriptors; }

    void resized() override {
        auto bounds = getLocalBounds();
        const auto disconnect_bounds = disconnect_all_button_.isVisible() || disconnect_selected_button_.isVisible()
            ? bounds.removeFromBottom(38).reduced(12, 4)
            : juce::Rectangle<int>{};
        disconnect_all_button_.setBounds(disconnect_all_button_.isVisible() ? disconnect_bounds : juce::Rectangle<int>{});
        disconnect_selected_button_.setBounds(disconnect_selected_button_.isVisible()
            ? disconnect_bounds : juce::Rectangle<int>{});
        panel_.setBounds(bounds);
    }

private:
    void update_disconnect_buttons() {
        const auto node_id = snapshot_ ? snapshot_->node.id : std::string{};
        std::vector<std::string> selected_ids = selection_.selected_node_ids();
        if (selected_ids.empty() && !node_id.empty())
            selected_ids.push_back(node_id);
        const bool multiple = selected_ids.size() > 1;
        const auto is_selected = [&](const std::string& id) {
            return std::find(selected_ids.begin(), selected_ids.end(), id) != selected_ids.end();
        };
        const bool has_connections = !selected_ids.empty() && std::any_of(
            controller_.project().connections.begin(), controller_.project().connections.end(),
            [&](const auto& connection) { return is_selected(connection.from_node) || is_selected(connection.to_node); });
        disconnect_all_button_.setVisible(snapshot_.has_value() && !multiple);
        disconnect_all_button_.setEnabled(!multiple && has_connections);
        disconnect_selected_button_.setVisible(snapshot_.has_value() && multiple);
        disconnect_selected_button_.setEnabled(multiple && has_connections);
        resized();
    }

    void disconnect_all_connections() {
        if (!snapshot_)
            return;
        disconnect_node_set({snapshot_->node.id});
    }

    void disconnect_selected_connections() {
        if (!snapshot_ || selection_.selected_node_ids().size() < 2)
            return;
        disconnect_node_set(selection_.selected_node_ids());
    }

    void disconnect_node_set(const std::vector<std::string>& node_ids) {
        if (!snapshot_)
            return;
        const auto primary_node_id = snapshot_->node.id;
        const bool stale_selection = snapshot_->session != controller_.session_identity() ||
            std::any_of(node_ids.begin(), node_ids.end(), [&](const auto& id) {
                return !controller_.node_snapshot(id).has_value();
            });
        if (stale_selection) {
            clear_stale_snapshot();
            return;
        }

        const auto outcome = controller_.disconnect_node_connections(node_ids);
        if (outcome.status == audioalgo::editor::ProjectEditStatus::applied) {
            show_node(primary_node_id);
            if (changed_)
                changed_();
        } else if (outcome.status == audioalgo::editor::ProjectEditStatus::rejected &&
                   !outcome.diagnostics.empty()) {
            const auto& diagnostic = outcome.diagnostics.front();
            panel_.show_parameter_error("connections", diagnostic.code + ": " + diagnostic.message);
        }
    }

    void clear_stale_snapshot() {
        snapshot_.reset();
        active_gesture_parameter_.reset();
        (void)selection_.select(controller_.project(), {});
        disconnect_all_button_.setVisible(false);
        disconnect_selected_button_.setVisible(false);
        panel_.clear("Inspector", "The project changed. Select a node to continue editing.");
        resized();
    }

    void apply_parameter(const audioalgo::editor::EditorNodeParameterBinding& binding,
                         std::string parameter_id,
                         audioalgo::ParameterValue value) {
        const auto outcome = binding.set_parameter_if_current(snapshot_, std::move(parameter_id), std::move(value));
        if (outcome)
            finish_parameter_edit(binding, *outcome);
    }

    void apply_parameter_gesture(const audioalgo::editor::EditorNodeParameterBinding& binding,
                                 std::string parameter_id,
                                 audioalgo::editor::ParameterGesturePhase phase,
                                 audioalgo::ParameterValue value) {
        if (!binding.matches(snapshot_))
            return;
        if (phase == audioalgo::editor::ParameterGesturePhase::started) {
            if (active_gesture_parameter_)
                (void)controller_.cancel_parameter_gesture(binding.session());
            const auto started = controller_.begin_parameter_gesture(binding.session(), binding.node_id(), parameter_id);
            if (started.status == audioalgo::editor::ParameterEditStatus::started)
                active_gesture_parameter_ = std::move(parameter_id);
            else if (started.diagnostic && started.diagnostic->code == "project_session_stale")
                clear_stale_snapshot();
            return;
        }

        if (phase == audioalgo::editor::ParameterGesturePhase::updated) {
            if (active_gesture_parameter_ && *active_gesture_parameter_ == parameter_id) {
                const auto updated = controller_.update_parameter_gesture(binding.session(), std::move(value));
                if (updated.status == audioalgo::editor::ParameterEditStatus::rejected && updated.diagnostic &&
                    updated.diagnostic->code == "project_session_stale")
                    clear_stale_snapshot();
                return;
            }
            apply_parameter(binding, std::move(parameter_id), std::move(value));
            return;
        }

        if (active_gesture_parameter_ && *active_gesture_parameter_ == parameter_id) {
            const auto committed = controller_.commit_parameter_gesture(binding.session());
            active_gesture_parameter_.reset();
            finish_parameter_edit(binding, committed);
        }
    }

    void finish_parameter_edit(const audioalgo::editor::EditorNodeParameterBinding& binding,
                               const audioalgo::editor::ParameterEditOutcome& outcome) {
        if (!binding.matches(snapshot_))
            return;
        if (outcome.status == audioalgo::editor::ParameterEditStatus::applied) {
            panel_.clear_parameter_error();
            auto updated_snapshot = controller_.node_snapshot(binding.node_id());
            if (!binding.matches(updated_snapshot)) {
                clear_stale_snapshot();
                return;
            }
            snapshot_ = std::move(updated_snapshot);
            panel_.synchronize_values(snapshot_->node.parameters);
            if (changed_)
                changed_();
        } else if (outcome.diagnostic && outcome.diagnostic->code == "project_session_stale") {
            clear_stale_snapshot();
        } else if (outcome.status == audioalgo::editor::ParameterEditStatus::rejected) {
            if (outcome.diagnostic)
                panel_.show_parameter_error(outcome.diagnostic->port_id,
                    outcome.diagnostic->code + ": " + outcome.diagnostic->message);
        } else if (outcome.status == audioalgo::editor::ParameterEditStatus::unchanged) {
            panel_.clear_parameter_error();
        }
    }

    audioalgo::editor::EditorController& controller_;
    std::optional<audioalgo::editor::NodeViewSnapshot> snapshot_;
    const audioalgo::NodeCatalog* descriptors_ = nullptr;
    audioalgo::editor::NodeSelectionState& selection_;
    std::function<void()> changed_;
    std::optional<std::string> active_gesture_parameter_;
    audioalgo::editor::ParameterPanel panel_;
    juce::TextButton disconnect_all_button_{"Disconnect all connections"};
    juce::TextButton disconnect_selected_button_{"Disconnect selected nodes"};
};


} // namespace audioalgo::editor
