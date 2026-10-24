#pragma once
#include "EditorUiSupport.h"
#include "audio_editor_application/EditorController.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace audioalgo::editor {
class NodeUpgradePanel final : public juce::Component {
public:
    NodeUpgradePanel(const ManagedUpgradePreview& preview, std::function<void()> confirm, std::function<void()> cancel) {
        setComponentID("upgrade.panel");
        juce::String text = "Upgrade package " + utf8_string(preview.package_id) + "\n\n";
        for (const auto& instance : preview.instances)
            text += utf8_string(instance.type_id) + " / " + utf8_string(instance.node_id) + "\n" +
                utf8_string(instance.from_sha256) + " → " + utf8_string(instance.to_sha256) + "\n\n";
        for (const auto& error : preview.diagnostics)
            text += utf8_string(error.node_id) + " / " + utf8_string(error.port_id) + ": " + utf8_string(error.code) + " — " + utf8_string(error.message) + "\n";
        summary_.setComponentID("upgrade.summary"); summary_.setText(text, juce::dontSendNotification);
        summary_.setJustificationType(juce::Justification::topLeft); summary_.setMinimumHorizontalScale(1.0f);
        summary_.setColour(juce::Label::textColourId, ink); viewport_.setViewedComponent(&summary_, false); viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_); addAndMakeVisible(confirm_); addAndMakeVisible(cancel_);
        confirm_.setComponentID("upgrade.confirm"); cancel_.setComponentID("upgrade.cancel");
        style_button(confirm_, signal); style_button(cancel_);
        confirm_.setEnabled(preview.diagnostics.empty() && !preview.instances.empty());
        confirm_.onClick = [confirm = std::move(confirm)] { auto action = confirm; if (action) action(); };
        cancel_.onClick = [cancel = std::move(cancel)] { auto action = cancel; if (action) action(); };
        rows_ = static_cast<int>(preview.instances.size()) * 4 + static_cast<int>(preview.diagnostics.size()) * 3 + 4;
    }
    void paint(juce::Graphics& g) override { g.fillAll(panel); }
    void resized() override {
        viewport_.setBounds(18, 18, getWidth() - 36, std::max(40, getHeight() - 78));
        summary_.setBounds(0, 0, std::max(40, viewport_.getWidth() - 20), std::max(viewport_.getHeight(), rows_ * 24));
        cancel_.setBounds(getWidth() - 262, getHeight() - 46, 112, 30); confirm_.setBounds(getWidth() - 140, getHeight() - 46, 122, 30);
    }
private:
    juce::Label summary_;
    juce::Viewport viewport_;
    juce::TextButton confirm_{"Upgrade all"}, cancel_{"Cancel"};
    int rows_ = 0;
};
}
