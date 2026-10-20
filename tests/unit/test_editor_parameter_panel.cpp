#include "EditorParameterPanel.h"
#include "EditorNodeSummary.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

audioalgo::NodeDescriptor described_parameter_node() {
    audioalgo::ParameterDescriptor enabled{"enabled", audioalgo::ParameterKind::boolean, true, {}, {}, {}};
    enabled.display_name = "Enabled";
    enabled.help = "Apply this test node.";

    audioalgo::ParameterDescriptor repeat{"repeat", audioalgo::ParameterKind::integer,
                                           std::int64_t{3}, 1.0, 8.0, "times"};
    repeat.display_name = "Repeat";
    repeat.step = 1.0;
    repeat.help = "Number of repetitions.";

    audioalgo::ParameterDescriptor scale{"scale", audioalgo::ParameterKind::floating,
                                          0.5, 0.25, 2.0, "×"};
    scale.display_name = "Scale";
    scale.step = 0.25;
    scale.help = "Output scale.";

    audioalgo::ParameterDescriptor polarity{"polarity", audioalgo::ParameterKind::enumeration,
                                             std::string("negative"), {}, {}, {}};
    polarity.display_name = "Polarity";
    polarity.choices = {{"negative", "Negative"}, {"positive", "Positive"}};
    polarity.help = "Select the stable polarity value.";

    audioalgo::ParameterDescriptor tag{"tag", audioalgo::ParameterKind::string, std::string("warm"), {}, {}, {}};
    tag.display_name = "Tag";
    tag.help = "A report-only label.";

    audioalgo::ParameterDescriptor coefficient{"coefficient", audioalgo::ParameterKind::path,
                                                std::string("coefficients/default.wav"), {}, {}, {}};
    coefficient.display_name = "Coefficient file";
    coefficient.help = "Path to the coefficient resource.";

    return {"test.parameter_panel", "1.0.0", "Parameter panel test", {},
            {std::move(enabled), std::move(repeat), std::move(scale), std::move(polarity),
             std::move(tag), std::move(coefficient)},
            audioalgo::NodeExecutionKind::block};
}

template <typename ComponentType>
ComponentType* find_control(juce::Component& parent, const juce::String& component_id) {
    for (int index = 0; index < parent.getNumChildComponents(); ++index) {
        auto* child = parent.getChildComponent(index);
        if (child != nullptr && child->getComponentID() == component_id)
            return dynamic_cast<ComponentType*>(child);
    }
    return nullptr;
}

} // namespace

TEST_CASE("AR-10.a descriptor creates typed parameter controls and preserves stable enum values", "[AR-10.a][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto descriptor = described_parameter_node();
    std::vector<std::pair<std::string, audioalgo::ParameterValue>> edits;
    audioalgo::editor::ParameterPanel panel;
    panel.show_parameters(descriptor, {}, [&](std::string id, audioalgo::ParameterValue value) {
        edits.emplace_back(std::move(id), std::move(value));
    });

    auto* enabled = find_control<juce::ToggleButton>(panel, "parameter.enabled");
    REQUIRE(enabled != nullptr);
    CHECK(enabled->getButtonText().toStdString() == "Enabled");
    CHECK(enabled->getToggleState());
    CHECK(enabled->getTooltip().toStdString() == "Apply this test node.");

    auto* repeat_label = find_control<juce::Label>(panel, "label.repeat");
    auto* repeat = find_control<juce::Slider>(panel, "parameter.repeat");
    REQUIRE(repeat_label != nullptr);
    REQUIRE(repeat != nullptr);
    CHECK(repeat_label->getText().toStdString() == "Repeat");
    CHECK(repeat->getValue() == 3.0);
    CHECK(repeat->getMinimum() == 1.0);
    CHECK(repeat->getMaximum() == 8.0);
    CHECK(repeat->getInterval() == 1.0);
    CHECK(repeat->getTextValueSuffix().toStdString() == " times");
    CHECK(repeat->getTooltip().toStdString() == "Number of repetitions.");

    auto* scale_label = find_control<juce::Label>(panel, "label.scale");
    auto* scale = find_control<juce::Slider>(panel, "parameter.scale");
    REQUIRE(scale_label != nullptr);
    REQUIRE(scale != nullptr);
    CHECK(scale_label->getText().toStdString() == "Scale");
    CHECK(scale->getValue() == 0.5);
    CHECK(scale->getMinimum() == 0.25);
    CHECK(scale->getMaximum() == 2.0);
    CHECK(scale->getInterval() == 0.25);
    CHECK(scale->getTextValueSuffix().toStdString() == " ×");
    CHECK(scale->getTooltip().toStdString() == "Output scale.");

    auto* polarity_label = find_control<juce::Label>(panel, "label.polarity");
    auto* polarity = find_control<juce::ComboBox>(panel, "parameter.polarity");
    REQUIRE(polarity_label != nullptr);
    REQUIRE(polarity != nullptr);
    CHECK(polarity_label->getText().toStdString() == "Polarity");
    CHECK(polarity->getNumItems() == 2);
    CHECK(polarity->getText().toStdString() == "Negative");
    CHECK(polarity->getTooltip().toStdString() == "Select the stable polarity value.");
    polarity->setSelectedId(2, juce::sendNotificationSync);
    REQUIRE(edits.size() == 1);
    CHECK(edits.back().first == "polarity");
    REQUIRE(std::holds_alternative<std::string>(edits.back().second));
    CHECK(std::get<std::string>(edits.back().second) == "positive");

    auto* tag_label = find_control<juce::Label>(panel, "label.tag");
    auto* tag = find_control<juce::TextEditor>(panel, "parameter.tag");
    REQUIRE(tag_label != nullptr);
    REQUIRE(tag != nullptr);
    CHECK(tag_label->getText().toStdString() == "Tag");
    CHECK(tag->getText().toStdString() == "warm");
    CHECK(tag->getTooltip().toStdString() == "A report-only label.");

    auto* coefficient_label = find_control<juce::Label>(panel, "label.coefficient");
    auto* coefficient = find_control<juce::TextEditor>(panel, "parameter.coefficient");
    REQUIRE(coefficient_label != nullptr);
    REQUIRE(coefficient != nullptr);
    CHECK(coefficient_label->getText().toStdString() == "Coefficient file");
    CHECK(coefficient->getText().toStdString() == "coefficients/default.wav");
    CHECK(coefficient->getTooltip().toStdString() == "Path to the coefficient resource.");
}

TEST_CASE("AR-10.f node summaries follow descriptor metadata and preserve full tooltip values", "[AR-10.f][R3]") {
    auto descriptor = described_parameter_node();
    audioalgo::NodeInstance node{"summary-one", descriptor.type_id, {}, 0, 0};
    auto summary = audioalgo::editor::to_utf8(audioalgo::editor::node_summary(node, &descriptor));
    CHECK(summary.find("Enabled: true") != std::string::npos);
    CHECK(summary.find("Repeat: 3 times") != std::string::npos);
    CHECK(summary.find("Scale: 0.5 ×") != std::string::npos);
    CHECK(summary.find("Polarity: Negative") != std::string::npos);
    CHECK(summary.find("Tag: warm") != std::string::npos);
    CHECK(summary.find("Coefficient file: coefficients/default.wav") != std::string::npos);

    descriptor.parameters[2].display_name = "新尺度";
    descriptor.parameters[2].unit = "ratio";
    const auto long_path = std::string("中文 路径/") + std::string(320, 'x') + "/系数.txt";
    node.parameters["scale"] = 0.75;
    node.parameters["coefficient"] = long_path;
    summary = audioalgo::editor::to_utf8(audioalgo::editor::node_summary(node, &descriptor));
    CHECK(summary.find("新尺度: 0.75 ratio") != std::string::npos);
    const auto tooltip = audioalgo::editor::build_node_tooltip_text(descriptor.display_name, summary, "audio_in");
    CHECK(tooltip.find(long_path) != std::string::npos);
    CHECK(tooltip.find("audio_in") != std::string::npos);
    CHECK(audioalgo::editor::to_utf8(audioalgo::editor::node_summary(
        {"summary-two", descriptor.type_id, {{"scale", 1.5}}, 0, 0}, &descriptor)).find("新尺度: 1.5 ratio") != std::string::npos);
    CHECK(audioalgo::editor::node_summary(node, nullptr).contains("Missing"));
}
