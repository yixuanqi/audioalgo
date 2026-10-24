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
                std::function<std::uint64_t()> epoch,
                std::function<void()> create_custom = {}, std::function<void()> open_draft = {}, std::function<void()> upgrade = {}, std::function<void()> recover = {})
        : clicked_(std::move(clicked)), epoch_(std::move(epoch)) {
        setInterceptsMouseClicks(false, true);
        addAndMakeVisible(title_);
        addAndMakeVisible(viewport_);
        addAndMakeVisible(new_custom_); addAndMakeVisible(open_draft_);
        addAndMakeVisible(recover_); recover_.setComponentID("authoring.recover"); recover_.onClick = std::move(recover);
        recover_.setEnabled(static_cast<bool>(recover_.onClick)); style_button(recover_);
        addAndMakeVisible(upgrade_); upgrade_.setComponentID("authoring.upgrade"); upgrade_.onClick = std::move(upgrade);
        upgrade_.setEnabled(static_cast<bool>(upgrade_.onClick)); style_button(upgrade_);
        new_custom_.setComponentID("authoring.new"); open_draft_.setComponentID("authoring.open");
        new_custom_.onClick = std::move(create_custom); open_draft_.onClick = std::move(open_draft);
        new_custom_.setEnabled(static_cast<bool>(new_custom_.onClick));
        open_draft_.setEnabled(static_cast<bool>(open_draft_.onClick));
        style_button(new_custom_, signal); style_button(open_draft_);
        title_.setText("NODE LIBRARY", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, muted);
        title_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        viewport_.setScrollBarsShown(true, false);
        refresh(catalog);
    }
    struct PreparedRefresh {
        std::unique_ptr<juce::Component> content;
        std::vector<std::unique_ptr<NodeLibraryButton>> buttons;
    };
    PreparedRefresh prepare_refresh(const NodeCatalog& catalog) const {
        PreparedRefresh replacement{std::make_unique<juce::Component>(), {}};
        for (const auto& entry : build_node_library_entries(catalog)) {
            auto button = std::make_unique<NodeLibraryButton>(utf8_string(entry.display_name), entry.type_id, clicked_, epoch_);
            button->setComponentID("library." + utf8_string(entry.type_id));
            style_button(*button);
            replacement.content->addAndMakeVisible(*button);
            replacement.buttons.push_back(std::move(button));
        }
        return replacement;
    }
    void publish_refresh(PreparedRefresh replacement) {
        const auto scroll = viewport_.getViewPosition();
        viewport_.setViewedComponent(replacement.content.get(), false);
        content_.swap(replacement.content); buttons_.swap(replacement.buttons);
        resized(); viewport_.setViewPosition(scroll);
    }
    void refresh(const NodeCatalog& catalog) { publish_refresh(prepare_refresh(catalog)); }
    void resized() override {
        title_.setBounds(18, 24, getWidth() - 34, 22);
        new_custom_.setBounds(12, 52, getWidth() - 24, 30);
        open_draft_.setBounds(12, 86, getWidth() - 24, 28);
        recover_.setBounds(12, 118, getWidth() - 24, 28);
        viewport_.setBounds(10, 150, getWidth() - 20, std::max(48, getHeight() - 206));
        upgrade_.setBounds(12, getHeight() - 40, getWidth() - 24, 28);
        const int width = std::max(96, viewport_.getWidth() - 20);
        int y = 8;
        for (auto& button : buttons_) {
            button->setBounds(2, y, width, 34);
            y += 42;
        }
        if (content_) content_->setSize(viewport_.getWidth() - 8, y);
    }
private:
    std::function<void(std::string)> clicked_;
    std::function<std::uint64_t()> epoch_;
    juce::TextButton new_custom_{"New custom node"}, open_draft_{"Open draft"};
    juce::TextButton upgrade_{"Upgrade project"};
    juce::TextButton recover_{"Recover draft"};
    juce::Label title_;
    std::unique_ptr<juce::Component> content_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<NodeLibraryButton>> buttons_;
};

} // namespace audioalgo::editor
