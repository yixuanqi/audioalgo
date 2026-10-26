#pragma once
#include "EditorUiSupport.h"
#include <functional>

namespace audioalgo::editor {
class PanelSplitter final : public juce::Component, public juce::SettableTooltipClient {
public:
    void configure(const juce::String& id, const juce::String& name, bool vertical,
                   std::function<void(int)> move, std::function<void()> reset) {
        setComponentID(id); setTitle(name); setTooltip(name + "; drag to resize, double-click to reset");
        vertical_ = vertical; move_ = std::move(move); reset_ = std::move(reset);
        setMouseCursor(vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
        setWantsKeyboardFocus(true);
    }
    void mouseDown(const juce::MouseEvent& event) override {
        dragging_ = event.mods.isLeftButtonDown(); previous_ = event.getScreenPosition(); repaint();
    }
    void mouseDrag(const juce::MouseEvent& event) override {
        if (!dragging_) return;
        const auto point = event.getScreenPosition();
        const auto delta = vertical_ ? point.x - previous_.x : point.y - previous_.y;
        previous_ = point;
        if (move_ && delta) move_(delta);
    }
    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; repaint(); }
    void mouseDoubleClick(const juce::MouseEvent& event) override {
        if (event.mods.isLeftButtonDown() && reset_) reset_();
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    bool keyPressed(const juce::KeyPress& key) override {
        const int code = key.getKeyCode();
        const int delta = vertical_ ? (code == juce::KeyPress::leftKey ? -8 : code == juce::KeyPress::rightKey ? 8 : 0)
                                    : (code == juce::KeyPress::upKey ? -8 : code == juce::KeyPress::downKey ? 8 : 0);
        if (!delta || !move_) return false;
        move_(delta); return true;
    }
    void paint(juce::Graphics& g) override {
        const bool active = dragging_ || isMouseOver() || hasKeyboardFocus(true);
        g.fillAll(active ? raised : background);
        g.setColour(active ? signal : muted.withAlpha(.45f));
        auto grip = getLocalBounds().toFloat().withSizeKeepingCentre(vertical_ ? 2.f : 36.f, vertical_ ? 36.f : 2.f);
        g.fillRoundedRectangle(grip, 1.f);
    }
private:
    bool vertical_ = true, dragging_ = false;
    juce::Point<int> previous_;
    std::function<void(int)> move_;
    std::function<void()> reset_;
};
}
