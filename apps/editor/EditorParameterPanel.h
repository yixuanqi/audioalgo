#pragma once

#include "audio_node_sdk/NodeSDK.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audioalgo::editor {

enum class ParameterGesturePhase { started, updated, committed };

class ParameterPanel final : public juce::Component {
public:
    using ParameterChanged = std::function<void(std::string, ParameterValue)>;
    using ParameterGestureChanged =
        std::function<void(std::string, ParameterGesturePhase, ParameterValue)>;

    ParameterPanel();
    ~ParameterPanel() override;

    void clear(const juce::String& title, const juce::String& details);
    void show_parameters(const NodeDescriptor& descriptor, const NodeParameters& values,
                         ParameterChanged on_change,
                         ParameterGestureChanged on_gesture = {},
                         const juce::String& details = {});
    void show_parameter_error(const std::string& parameter_id, const std::string& message);
    void clear_parameter_error();
    void synchronize_values(const NodeParameters& values);
    void show_opaque_parameters(const std::vector<std::pair<std::string, std::string>>& fields);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    struct ControlRow;

    void remove_controls();
    void emit_change(const std::string& parameter_id, ParameterValue value);
    void apply_conditions(const NodeParameters& values);

    juce::Label title_;
    juce::Label details_;
    juce::Label error_;
    ParameterChanged on_change_;
    ParameterGestureChanged on_gesture_;
    std::vector<std::unique_ptr<ControlRow>> controls_;
    std::vector<ParameterDescriptor> schema_;
};

} // namespace audioalgo::editor
