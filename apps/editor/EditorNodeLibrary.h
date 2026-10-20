#pragma once
#include "EditorUiSupport.h"
#include "EditorNodeCreation.h"
#include "EditorNodeDragDrop.h"
#include <functional>
#include <memory>
namespace audioalgo::editor {
class NodeLibraryButton final : public juce::TextButton {
public:
    NodeLibraryButton(const juce::String& text, std::string type_id,
                      std::function<void(std::string)> click_callback,
                      std::function<std::uint64_t()> project_epoch)
        : juce::TextButton(text), type_id_(std::move(type_id)), click_callback_(std::move(click_callback)),
          project_epoch_(std::move(project_epoch)) {
        onClick = [this] {
            if (click_callback_)
                click_callback_(type_id_);
        };
    }

    void mouseDown(const juce::MouseEvent& event) override {
        gesture_.begin({event.position.x, event.position.y});
        gesture_project_epoch_ = project_epoch_ ? project_epoch_() : 0;
        juce::TextButton::mouseDown(event);
    }

    [[nodiscard]] std::uint64_t gesture_project_epoch() const noexcept { return gesture_project_epoch_; }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (!gesture_.move({event.position.x, event.position.y})) {
            juce::TextButton::mouseDrag(event);
            return;
        }
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        if (container == nullptr)
            return;
        setState(juce::Button::buttonNormal);
        container->startDragging(make_library_drag_description(type_id_), this, juce::ScaledImage{}, false,
                                 nullptr, &event.source);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        const auto release = gesture_.finish(getLocalBounds().toFloat().contains(event.position));
        if (release == audioalgo::editor::LibraryNodeDragGesture::Release::drag) {
            setState(juce::Button::buttonNormal);
        } else {
            juce::TextButton::mouseUp(event);
        }
    }

private:
    std::string type_id_;
    std::function<void(std::string)> click_callback_;
    std::function<std::uint64_t()> project_epoch_;
    audioalgo::editor::LibraryNodeDragGesture gesture_;
    std::uint64_t gesture_project_epoch_ = 0;
};

class NodeLibrary final : public juce::Component {
public:
    NodeLibrary(const NodeCatalog& catalog, std::function<void(std::string)> clicked,
                std::function<std::uint64_t()> epoch) {
        setInterceptsMouseClicks(false, true);
        addAndMakeVisible(title_);
        addAndMakeVisible(viewport_);
        title_.setText("NODE LIBRARY", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, muted);
        title_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        for (const auto& entry : build_node_library_entries(catalog)) {
            auto button = std::make_unique<NodeLibraryButton>(utf8_string(entry.display_name), entry.type_id, clicked, epoch);
            button->setComponentID("library." + utf8_string(entry.type_id));
            style_button(*button);
            content_.addAndMakeVisible(*button);
            buttons_.push_back(std::move(button));
        }
    }
    void resized() override {
        title_.setBounds(18, 24, getWidth() - 34, 22);
        viewport_.setBounds(10, 54, getWidth() - 20, std::max(48, getHeight() - 110));
        const int width = std::max(96, viewport_.getWidth() - 20);
        int y = 8;
        for (auto& button : buttons_) {
            button->setBounds(2, y, width, 34);
            y += 42;
        }
        content_.setSize(viewport_.getWidth() - 8, y);
    }
private:
    juce::Label title_;
    juce::Component content_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<NodeLibraryButton>> buttons_;
};

} // namespace audioalgo::editor
