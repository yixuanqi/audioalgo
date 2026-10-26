#pragma once
#include "EditorText.h"
#include "EditorColours.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <filesystem>
#include <optional>
#include <algorithm>
namespace audioalgo::editor {

class EditorLookAndFeel final : public juce::LookAndFeel_V4 {
    class CaptionButton final : public juce::Button {
    public:
        explicit CaptionButton(int kind)
            : juce::Button(kind == juce::DocumentWindow::closeButton ? "Close" :
                           kind == juce::DocumentWindow::maximiseButton ? "Maximise" : "Minimise"), kind_(kind) {}
        void paintButton(juce::Graphics& g, bool over, bool down) override {
            const bool close = kind_ == juce::DocumentWindow::closeButton;
            if (over || down) { g.setColour(close ? danger.darker(.35f) : raised.brighter(.12f)); g.fillAll(); }
            g.setColour((over ? ink : muted).withMultipliedAlpha(isEnabled() ? 1.f : .4f));
            const auto centre = getLocalBounds().toFloat().getCentre();
            if (close) {
                g.drawLine(centre.x - 4, centre.y - 4, centre.x + 4, centre.y + 4, 1.3f);
                g.drawLine(centre.x + 4, centre.y - 4, centre.x - 4, centre.y + 4, 1.3f);
            } else if (kind_ == juce::DocumentWindow::minimiseButton) {
                g.drawLine(centre.x - 5, centre.y + 3, centre.x + 5, centre.y + 3, 1.3f);
            } else {
                auto box = juce::Rectangle<float>(10, 9).withCentre(centre);
                if (getToggleState()) { g.drawRect(box.translated(2, -2), 1.f); box = box.translated(-1, 1); }
                g.drawRect(box, 1.f);
            }
        }
    private:
        int kind_;
    };
public:
    EditorLookAndFeel() {
        setColourScheme(ColourScheme(panel, raised, panel, border, ink, muted,
                                     juce::Colours::white, selection, ink));
        setDefaultSansSerifTypefaceName("Segoe UI");
        setColour(juce::TextButton::buttonColourId, raised);
        setColour(juce::TextButton::buttonOnColourId, selection);
        setColour(juce::TextButton::textColourOffId, ink);
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::Label::textColourId, ink);
        setColour(juce::TextEditor::backgroundColourId, raised);
        setColour(juce::TextEditor::textColourId, ink);
        setColour(juce::TextEditor::outlineColourId, border);
        setColour(juce::TextEditor::focusedOutlineColourId, signal);
        setColour(juce::TextEditor::highlightColourId, selection);
        setColour(juce::TextEditor::highlightedTextColourId, ink);
        setColour(juce::CaretComponent::caretColourId, ink);
        setColour(juce::ComboBox::backgroundColourId, raised);
        setColour(juce::ComboBox::textColourId, ink);
        setColour(juce::ComboBox::outlineColourId, border);
        setColour(juce::ComboBox::arrowColourId, ink);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::textColourId, ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, selection);
        setColour(juce::PopupMenu::highlightedTextColourId, ink);
        setColour(juce::ToggleButton::textColourId, ink);
        setColour(juce::ToggleButton::tickColourId, signal);
        setColour(juce::Slider::backgroundColourId, raised);
        setColour(juce::Slider::textBoxBackgroundColourId, raised);
        setColour(juce::Slider::textBoxTextColourId, ink);
        setColour(juce::Slider::textBoxOutlineColourId, border);
        setColour(juce::ScrollBar::thumbColourId, muted.withAlpha(.45f));
    }
    juce::Font getTextButtonFont(juce::TextButton&, int height) override {
        return juce::Font(juce::FontOptions(std::min(14.f, height * .55f)));
    }
    juce::Button* createDocumentWindowButton(int kind) override { return new CaptionButton(kind); }
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                              bool highlighted, bool down) override {
        auto bounds = button.getLocalBounds().toFloat().reduced(.5f);
        const auto fill = button.getToggleState() ? button.findColour(juce::TextButton::buttonOnColourId) : colour;
        g.setColour((down ? fill.darker(.15f) : highlighted ? fill.brighter(.12f) : fill)
                        .withMultipliedAlpha(button.isEnabled() ? 1.f : .4f));
        g.fillRoundedRectangle(bounds, 3.f);
        g.setColour(button.hasKeyboardFocus(true) ? signal : highlighted ? muted : border);
        g.drawRoundedRectangle(bounds, 3.f, 1.f);
    }
    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int width, int height,
                                    int title_x, int title_width, const juce::Image*, bool active) override {
        g.fillAll(panel); g.setColour(border); g.fillRect(0, height - 1, width, 1);
        g.setColour(active ? ink : muted); g.setFont(juce::FontOptions(14.f, juce::Font::bold));
        g.drawText(window.getName(), title_x + 12, 0, std::max(0, title_width - 24), height, juce::Justification::centredLeft);
    }
};

inline void style_editor_window(juce::DocumentWindow& window) {
    window.setUsingNativeTitleBar(false);
    window.setTitleBarHeight(32);
    window.setColour(juce::DocumentWindow::textColourId, ink);
    window.setTitleBarButtonsRequired(juce::DocumentWindow::allButtons, false);
}

inline juce::String native_user_log_notice(const NodeDescriptor& descriptor) {
    if(!descriptor.plugin_origin || descriptor.plugin_origin->runtime!="native")return {};
    const auto metadata=juce::JSON::parse(utf8_string(descriptor.backend_metadata_json));
    const auto capability=metadata["user_logs"].toString();
    if(capability=="sdk_v1")return "User logs: available.";
    if(capability=="unavailable")return "User logs: unavailable in this version.";
    return {};
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline juce::File to_juce_file(const std::filesystem::path& path) {
    const auto value = path_to_utf8(path);
    return juce::File(utf8_string(value));
}

inline std::filesystem::path path_from_juce_file(const juce::File& file) {
    return std::filesystem::u8path(to_utf8(file.getFullPathName()));
}

inline std::string new_id() {
    return juce::Uuid().toString().toStdString();
}

inline juce::var make_library_drag_description(const std::string& type_id) {
    auto* payload = new juce::DynamicObject();
    payload->setProperty("operation", "node-library-add");
    payload->setProperty("version", 1);
    payload->setProperty("type_id", utf8_string(type_id));
    return juce::var(payload);
}

inline std::optional<std::string> library_drag_type(const juce::var& description) {
    const auto* payload = description.getDynamicObject();
    if (payload == nullptr || payload->getProperty("operation").toString() != "node-library-add" ||
        static_cast<int>(payload->getProperty("version")) != 1)
        return std::nullopt;
    const auto type_id = to_utf8(payload->getProperty("type_id").toString());
    if (type_id.empty())
        return std::nullopt;
    return type_id;
}

inline const audioalgo::NodeInstance* find_node(const audioalgo::ProjectModel& project, const std::string& id) {
    const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) { return node.id == id; });
    return found == project.nodes.end() ? nullptr : &*found;
}

inline void style_button(juce::TextButton& button, juce::Colour accent = {}) {
        const bool prominent = accent == signal;
        button.setColour(juce::TextButton::buttonColourId, prominent ? primary : raised);
        button.setColour(juce::TextButton::buttonOnColourId, selection);
        button.setColour(juce::TextButton::textColourOffId, prominent ? juce::Colours::white : ink);
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    }


} // namespace audioalgo::editor
