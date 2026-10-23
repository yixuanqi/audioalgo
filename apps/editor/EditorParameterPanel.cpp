#include "EditorParameterPanel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

namespace audioalgo::editor {
namespace {

constexpr juce::uint32 ink = 0xffe8edf5;
constexpr juce::uint32 muted = 0xffaab6c7;
constexpr juce::uint32 amber = 0xffffbd69;
constexpr juce::uint32 signal = 0xff55d6be;
constexpr juce::uint32 panel = 0xff17212c;
constexpr juce::uint32 raised = 0xff263442;

juce::String to_juce_string(std::string_view value) {
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}

std::string label_for(const ParameterDescriptor& descriptor) {
    return descriptor.display_name.empty() ? descriptor.id : descriptor.display_name;
}

ParameterValue current_value(const ParameterDescriptor& descriptor, const NodeParameters& values) {
    const auto existing = values.find(descriptor.id);
    return existing == values.end() ? descriptor.default_value : existing->second;
}

ParameterValue slider_value(ParameterKind kind, double value) {
    if (kind == ParameterKind::integer)
        return static_cast<std::int64_t>(std::llround(value));
    return value;
}

} // namespace

struct ParameterPanel::ControlRow {
    bool read_only_structure = false;
    ParameterDescriptor descriptor;
    ParameterKind kind = ParameterKind::string;
    std::string id;
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::Component> editor;
    std::unique_ptr<juce::TextButton> reset;
};

ParameterPanel::ParameterPanel() {
    addAndMakeVisible(title_);
    addAndMakeVisible(details_);
    addAndMakeVisible(error_);
    error_.setComponentID("parameter.error");
    error_.setColour(juce::Label::textColourId, juce::Colour(0xffff8e87));
    error_.setJustificationType(juce::Justification::topLeft);
    title_.setColour(juce::Label::textColourId, juce::Colour(ink));
    title_.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    details_.setColour(juce::Label::textColourId, juce::Colour(muted));
    details_.setFont(juce::FontOptions(11.5f));
}

ParameterPanel::~ParameterPanel() = default;

void ParameterPanel::clear(const juce::String& title, const juce::String& details) {
    clear_parameter_error();
    remove_controls();
    on_change_ = {};
    on_gesture_ = {};
    title_.setText(title, juce::dontSendNotification);
    details_.setText(details, juce::dontSendNotification);
    resized();
    repaint();
}

void ParameterPanel::show_parameters(const NodeDescriptor& descriptor, const NodeParameters& values,
                                     ParameterChanged on_change,
                                     ParameterGestureChanged on_gesture,
                                     const juce::String& details) {
    clear_parameter_error();
    remove_controls();
    on_change_ = std::move(on_change);
    schema_ = descriptor.parameters;
    on_gesture_ = std::move(on_gesture);
    title_.setText(to_juce_string(descriptor.display_name), juce::dontSendNotification);
    details_.setText(details, juce::dontSendNotification);

    for (const auto& parameter : descriptor.parameters) {
        auto row = std::make_unique<ControlRow>();
        row->descriptor = parameter;
        row->kind = parameter.kind;
        row->id = parameter.id;
        const auto label = to_juce_string(label_for(parameter));
        const auto tooltip = to_juce_string(parameter.help);
        const auto value = current_value(parameter, values);

        if (parameter.kind != ParameterKind::boolean) {
            row->label = std::make_unique<juce::Label>();
            row->label->setComponentID("label." + to_juce_string(parameter.id));
            row->label->setText(label, juce::dontSendNotification);
            row->label->setColour(juce::Label::textColourId, juce::Colour(muted));
            row->label->setTooltip(tooltip);
            addAndMakeVisible(*row->label);
        }

        switch (parameter.kind) {
        case ParameterKind::boolean: {
            auto editor = std::make_unique<juce::ToggleButton>(label);
            editor->setComponentID("parameter." + to_juce_string(parameter.id));
            editor->setTooltip(tooltip);
            if (const auto* boolean_value = std::get_if<bool>(&value))
                editor->setToggleState(*boolean_value, juce::dontSendNotification);
            const auto id = parameter.id;
            editor->onClick = [this, id, control = editor.get()] {
                emit_change(id, control->getToggleState());
            };
            row->editor = std::move(editor);
            break;
        }
        case ParameterKind::integer:
        case ParameterKind::floating: {
            auto editor = std::make_unique<juce::Slider>();
            editor->setComponentID("parameter." + to_juce_string(parameter.id));
            editor->setTooltip(tooltip);
            const auto minimum = parameter.minimum.value_or(-1000000.0);
            const auto maximum = parameter.maximum.value_or(1000000.0);
            const auto interval = parameter.step.value_or(
                parameter.kind == ParameterKind::integer ? 1.0 : 0.1);
            editor->setRange(minimum, maximum, interval);
            editor->setSliderStyle(juce::Slider::LinearHorizontal);
            editor->setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 26);
            editor->setTextValueSuffix(parameter.unit.empty() ? juce::String{} : " " + to_juce_string(parameter.unit));
            // Fine-grained plugin parameters must remain visible/editable at their
            // declared step, e.g. 0.00001 must not appear as 0.000 in the text box.
            const auto decimals = interval > 0.0 ? std::max(3, static_cast<int>(std::ceil(-std::log10(interval)))) : 3;
            editor->setNumDecimalPlacesToDisplay(parameter.kind == ParameterKind::integer ? 0 : std::min(15, decimals));
            editor->setColour(juce::Slider::thumbColourId, juce::Colour(amber));
            editor->setColour(juce::Slider::trackColourId, juce::Colour(signal));
            if (const auto* integer_value = std::get_if<std::int64_t>(&value))
                editor->setValue(static_cast<double>(*integer_value), juce::dontSendNotification);
            else if (const auto* floating_value = std::get_if<double>(&value))
                editor->setValue(*floating_value, juce::dontSendNotification);
            // JUCE's default parser accepts a numeric prefix and turns arbitrary text into zero.
            // Validate the complete edit before Slider can clamp/snap it or emit a command.
            editor->valueFromTextFunction = [this, parameter, control = editor.get()](const juce::String& text) {
                auto input = text.trim().toStdString();
                if (!input.empty() && input.front() == '+')
                    input.erase(input.begin());
                double number = 0.0;
                const auto parsed = std::from_chars(input.data(), input.data() + input.size(), number);
                const auto reject = [&](const std::string& message) {
                    show_parameter_error(parameter.id, message);
                    return control->getValue();
                };
                if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
                    return reject("Enter a complete numeric value.");
                if (!std::isfinite(number))
                    return reject("Parameter must be finite.");
                if (parameter.kind == ParameterKind::integer &&
                    (std::trunc(number) != number || number < -9223372036854775808.0 ||
                     number >= 9223372036854775808.0))
                    return reject("Enter a representable whole number.");
                const auto candidate = slider_value(parameter.kind, number);
                if (const auto invalid = validate_parameter_value(parameter, candidate))
                    return reject(invalid->code + ": " + invalid->message);
                clear_parameter_error();
                return number;
            };
            const auto id = parameter.id;
            const auto kind = parameter.kind;
            editor->onDragStart = [this, id, kind, control = editor.get()] {
                if (on_gesture_)
                    on_gesture_(id, ParameterGesturePhase::started, slider_value(kind, control->getValue()));
            };
            editor->onValueChange = [this, id, kind, control = editor.get()] {
                auto changed_value = slider_value(kind, control->getValue());
                if (on_gesture_)
                    on_gesture_(id, ParameterGesturePhase::updated, std::move(changed_value));
                else
                    emit_change(id, std::move(changed_value));
            };
            editor->onDragEnd = [this, id, kind, control = editor.get()] {
                if (on_gesture_)
                    on_gesture_(id, ParameterGesturePhase::committed, slider_value(kind, control->getValue()));
            };
            row->editor = std::move(editor);
            break;
        }
        case ParameterKind::enumeration: {
            auto editor = std::make_unique<juce::ComboBox>();
            editor->setComponentID("parameter." + to_juce_string(parameter.id));
            editor->setTooltip(tooltip);
            std::vector<std::string> stable_values;
            stable_values.reserve(parameter.choices.size());
            for (std::size_t index = 0; index < parameter.choices.size(); ++index) {
                const auto& choice = parameter.choices[index];
                stable_values.push_back(choice.value);
                editor->addItem(to_juce_string(choice.label), static_cast<int>(index + 1));
                if (const auto* default_value = std::get_if<std::string>(&value);
                    default_value != nullptr && *default_value == choice.value)
                    editor->setSelectedId(static_cast<int>(index + 1), juce::dontSendNotification);
            }
            const auto id = parameter.id;
            editor->onChange = [this, id, stable_values = std::move(stable_values), control = editor.get()] {
                const auto selected = control->getSelectedId();
                if (selected <= 0 || static_cast<std::size_t>(selected) > stable_values.size())
                    return;
                emit_change(id, stable_values[static_cast<std::size_t>(selected - 1)]);
            };
            row->editor = std::move(editor);
            break;
        }
        case ParameterKind::string:
        case ParameterKind::path: {
            auto editor = std::make_unique<juce::TextEditor>();
            editor->setComponentID("parameter." + to_juce_string(parameter.id));
            editor->setTooltip(tooltip);
            editor->setMultiLine(false);
            if (const auto* text_value = std::get_if<std::string>(&value))
                editor->setText(to_juce_string(*text_value), juce::dontSendNotification);
            const auto id = parameter.id;
            editor->onReturnKey = [this, id, control = editor.get()] {
                emit_change(id, control->getText().toStdString());
            };
            editor->onFocusLost = [this, id, control = editor.get()] {
                emit_change(id, control->getText().toStdString());
            };
            row->editor = std::move(editor);
            break;
        }
        }

        addAndMakeVisible(*row->editor);
        row->reset = std::make_unique<juce::TextButton>("Reset");
        row->reset->setComponentID("reset." + to_juce_string(parameter.id));
        row->reset->setTooltip("Restore the described default value");
        row->reset->onClick = [this, id = parameter.id, default_value = parameter.default_value] {
            emit_change(id, default_value);
        };
        addAndMakeVisible(*row->reset);
        controls_.push_back(std::move(row));
    }

    synchronize_values(values);
    resized();
    repaint();
}

void ParameterPanel::paint(juce::Graphics& graphics) {
    graphics.fillAll(juce::Colour(panel));
    graphics.setColour(juce::Colour(raised));
    graphics.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
}

void ParameterPanel::resized() {
    auto bounds = getLocalBounds().reduced(16);
    title_.setBounds(bounds.removeFromTop(28));
    details_.setBounds(bounds.removeFromTop(54));
    error_.setBounds(bounds.removeFromTop(error_.getText().isEmpty() ? 0 : 54));
    bounds.removeFromTop(10);
    for (const auto& row : controls_) {
        if (!row->editor->isVisible())
            continue;
        auto area = bounds.removeFromTop(row->read_only_structure ? 96 : row->kind == ParameterKind::boolean ? 36 : 54);
        if (row->reset) {
            row->reset->setBounds(area.removeFromRight(52).withSizeKeepingCentre(52, 26));
            area.removeFromRight(6);
        }
        if (row->kind == ParameterKind::boolean) {
            row->editor->setBounds(area);
        } else {
            row->label->setBounds(area.removeFromTop(20));
            row->editor->setBounds(area);
        }
        bounds.removeFromTop(8);
    }
}

void ParameterPanel::synchronize_values(const NodeParameters& values) {
    for (const auto& row : controls_) {
        if (row->read_only_structure)
            continue;
        const auto value = current_value(row->descriptor, values);
        if (auto* toggle = dynamic_cast<juce::ToggleButton*>(row->editor.get())) {
            if (const auto* boolean = std::get_if<bool>(&value))
                toggle->setToggleState(*boolean, juce::dontSendNotification);
        } else if (auto* slider = dynamic_cast<juce::Slider*>(row->editor.get())) {
            if (const auto* number = std::get_if<double>(&value))
                slider->setValue(*number, juce::dontSendNotification);
            else if (const auto* integer = std::get_if<std::int64_t>(&value))
                slider->setValue(static_cast<double>(*integer), juce::dontSendNotification);
        } else if (auto* combo = dynamic_cast<juce::ComboBox*>(row->editor.get())) {
            combo->setSelectedId(0, juce::dontSendNotification);
            if (const auto* stable_value = std::get_if<std::string>(&value))
                for (std::size_t index = 0; index < row->descriptor.choices.size(); ++index)
                    if (row->descriptor.choices[index].value == *stable_value)
                        combo->setSelectedId(static_cast<int>(index + 1), juce::dontSendNotification);
        } else if (auto* text = dynamic_cast<juce::TextEditor*>(row->editor.get())) {
            if (const auto* string = std::get_if<std::string>(&value))
                text->setText(to_juce_string(*string), juce::dontSendNotification);
        }
    }
    apply_conditions(values);
    resized();
}

void ParameterPanel::apply_conditions(const NodeParameters& values) {
    const auto matches = [&](const std::optional<ParameterCondition>& condition) {
        if (!condition)
            return true;
        const auto source = std::find_if(schema_.begin(), schema_.end(), [&](const auto& spec) {
            return spec.id == condition->parameter_id;
        });
        return source != schema_.end() && current_value(*source, values) == condition->equals;
    };
    for (const auto& row : controls_) {
        if (row->read_only_structure)
            continue;
        const bool visible = matches(row->descriptor.visible_if);
        const bool enabled = matches(row->descriptor.enabled_if);
        row->editor->setVisible(visible);
        row->editor->setEnabled(enabled);
        if (row->label) {
            row->label->setVisible(visible);
            row->label->setEnabled(enabled);
        }
        if (row->reset) {
            row->reset->setVisible(visible);
            row->reset->setEnabled(enabled);
        }
    }
}

void ParameterPanel::show_opaque_parameters(const std::vector<std::pair<std::string, std::string>>& fields) {
    for (const auto& [name, json] : fields) {
        auto row = std::make_unique<ControlRow>();
        row->read_only_structure = true;
        row->kind = ParameterKind::string;
        row->id = name;
        row->label = std::make_unique<juce::Label>();
        row->label->setComponentID("opaque-label." + to_juce_string(name));
        row->label->setText(to_juce_string(name) + " (unsupported structure, read-only)", juce::dontSendNotification);
        row->label->setColour(juce::Label::textColourId, juce::Colour(muted));
        row->label->setTooltip(row->label->getText());
        auto editor = std::make_unique<juce::TextEditor>();
        editor->setComponentID("opaque." + to_juce_string(name));
        editor->setMultiLine(true);
        editor->setReadOnly(true);
        editor->setText(to_juce_string(json), false);
        editor->setTooltip("This parameter structure is preserved but has no supported editor.");
        row->editor = std::move(editor);
        addAndMakeVisible(*row->label);
        addAndMakeVisible(*row->editor);
        controls_.push_back(std::move(row));
    }
    resized();
}

void ParameterPanel::show_parameter_error(const std::string& parameter_id, const std::string& message) {
    error_.setText(to_juce_string(parameter_id + ": " + message), juce::dontSendNotification);
    error_.setTooltip(error_.getText());
    resized();
}

void ParameterPanel::clear_parameter_error() {
    error_.setText({}, juce::dontSendNotification);
    error_.setTooltip({});
    resized();
}

void ParameterPanel::remove_controls() {
    controls_.clear();
    schema_.clear();
}

void ParameterPanel::emit_change(const std::string& parameter_id, ParameterValue value) {
    if (on_change_)
        on_change_(parameter_id, std::move(value));
}

} // namespace audioalgo::editor
