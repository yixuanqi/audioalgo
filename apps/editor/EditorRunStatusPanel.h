#pragma once
#include "EditorUiSupport.h"
namespace audioalgo::editor {
class RunStatusPanel final : public juce::Component {
public:
    RunStatusPanel() {
        addAndMakeVisible(label_);
        label_.setFont(juce::FontOptions(11.5f));
        label_.setColour(juce::Label::textColourId, muted);
    }
    void set_status(const juce::String& message, bool error) {
        label_.setText(message, juce::dontSendNotification);
        label_.setColour(juce::Label::textColourId, error ? danger : muted);
        label_.setTooltip(message);
    }
    juce::String status_text() const { return label_.getText(); }
    void resized() override { label_.setBounds(getLocalBounds()); }
private:
    juce::Label label_;
};
} // namespace audioalgo::editor
