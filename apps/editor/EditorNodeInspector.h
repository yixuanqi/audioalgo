#pragma once

#include "EditorNodeCreation.h"
#include "EditorNodeParameterBinding.h"
#include "EditorParameterPanel.h"
#include "EditorText.h"

namespace audioalgo::editor {

class NodeInspector final : public juce::Component {
public:
    NodeInspector(audioalgo::editor::EditorController& controller,
                  audioalgo::editor::NodeSelectionState& selection, std::function<void()> changed)
        : controller_(controller), selection_(selection), changed_(std::move(changed)) {
        addAndMakeVisible(panel_);
    }

    void show_node(const std::string& node_id) {
        if (active_gesture_parameter_) {
            if (snapshot_)
                (void)controller_.cancel_parameter_gesture(snapshot_->session);
            active_gesture_parameter_.reset();
        }
        const bool selected = selection_.select(controller_.project(), node_id);
        snapshot_ = selected ? controller_.node_snapshot(node_id) : std::nullopt;
        if (!snapshot_) {
            panel_.clear("Inspector", node_id.empty() ? "Select a node to edit its parameters." : "Node not found.");
            return;
        }

        const auto& node = snapshot_->node;
        const auto* descriptor = descriptors_->find(node.type_id);
        if (descriptor == nullptr) {
            panel_.clear("Missing node", utf8_string(node.type_id + "\nID " + node.id));
            return;
        }
        const audioalgo::editor::EditorNodeParameterBinding binding(controller_, *snapshot_);
        panel_.show_parameters(*descriptor, node.parameters,
            [this, binding](std::string parameter_id, audioalgo::ParameterValue value) {
                apply_parameter(binding, std::move(parameter_id), std::move(value));
            },
            [this, binding](std::string parameter_id, audioalgo::editor::ParameterGesturePhase phase,
                   audioalgo::ParameterValue value) {
                apply_parameter_gesture(binding, std::move(parameter_id), phase, std::move(value));
            },
            utf8_string(node.type_id + "\nID " + node.id));
        std::vector<std::pair<std::string, std::string>> opaque;
        for (const auto& field : node.opaque_parameters)
            opaque.emplace_back(field.name, field.value_json);
        panel_.show_opaque_parameters(opaque);
    }

    void set_descriptor_registry(const audioalgo::NodeCatalog& descriptors) noexcept { descriptors_ = &descriptors; }

    void resized() override {
        panel_.setBounds(getLocalBounds());
    }

private:
    void clear_stale_snapshot() {
        snapshot_.reset();
        active_gesture_parameter_.reset();
        (void)selection_.select(controller_.project(), {});
        panel_.clear("Inspector", "The project changed. Select a node to continue editing.");
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
};


} // namespace audioalgo::editor
