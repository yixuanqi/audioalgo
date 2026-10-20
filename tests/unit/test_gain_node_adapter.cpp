#include "plugins/builtin/GainNodeAdapter.h"
#include "audio_juce_backend/JuceGraph.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <utility>

TEST_CASE("AR-05.b Gain node adapter matches an independent block oracle", "[AR-05.b][G1]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    constexpr double gain_db = 6.020599913279624;
    const double scale = std::pow(10.0, gain_db / 20.0);
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.21 * std::sin(frame * 0.071));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.34 * std::cos(frame * 0.043));
    }
    const auto original = input;

    audioalgo::NodeParameters parameters;
    parameters.emplace("gain_db", gain_db);
    audioalgo::AudioPorts inputs;
    inputs.emplace("audio_in", input);
    const audioalgo::NodeExecutionContext context{};
    audioalgo::GainNodeAdapter adapter;
    const auto outcome = adapter.process({parameters, inputs, context});

    REQUIRE(std::holds_alternative<audioalgo::AudioPorts>(outcome));
    const auto& output = std::get<audioalgo::AudioPorts>(outcome).at("audio_out");
    REQUIRE(output.channel_ids() == input.channel_ids());
    REQUIRE(output.frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        REQUIRE(std::equal(input.channel(channel).begin(), input.channel(channel).end(),
                           original.channel(channel).begin(), original.channel(channel).end()));
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const double expected = static_cast<double>(original.channel(channel)[frame]) * scale;
            const double tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
            REQUIRE(std::abs(static_cast<double>(output.channel(channel)[frame]) - expected) <= tolerance);
        }
    }

    audioalgo::NodeRegistry descriptors;
    audioalgo::register_builtin_nodes(descriptors);
    audioalgo::ProjectModel project;
    project.project_id = "ar05b-wrapper-oracle";
    project.name = "Adapter and JUCE wrapper share the Gain kernel";
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
        {"gain", "builtin.gain", {{"gain_db", gain_db}}, 100.0, 0.0},
        {"output", "builtin.output", {}, 200.0, 0.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";
    const auto rendered = audioalgo::render_audio_graph(project, descriptors, input, 127);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& juce_output = std::get<audioalgo::AudioBuffer>(rendered);
    REQUIRE(juce_output.valid_interval() == input.valid_interval());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const double expected = static_cast<double>(original.channel(channel)[frame]) * scale;
            const double tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
            REQUIRE(std::abs(static_cast<double>(juce_output.channel(channel)[frame]) - expected) <= tolerance);
        }
    }
}

TEST_CASE("AR-05.c Gain rejects invalid values consistently across adapter and runtime", "[AR-05.c][G1]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::AudioBuffer input(48000, {"mono"}, 3);
    input.mutable_channel(0)[0] = 0.25f;
    input.mutable_channel(0)[1] = 0.5f;
    input.mutable_channel(0)[2] = -0.125f;

    audioalgo::GainNodeAdapter adapter;
    audioalgo::NodeParameters parameters;
    audioalgo::AudioPorts inputs;
    inputs.emplace("audio_in", input);
    const audioalgo::NodeExecutionContext context{};
    for (const double gain_db : {-60.0, 12.0}) {
        parameters["gain_db"] = gain_db;
        const auto result = adapter.process({parameters, inputs, context});
        REQUIRE(std::holds_alternative<audioalgo::AudioPorts>(result));
    }
    parameters["gain_db"] = std::numeric_limits<double>::quiet_NaN();
    const auto nan_parameter = adapter.process({parameters, inputs, context});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(nan_parameter));
    REQUIRE(std::get<audioalgo::Diagnostic>(nan_parameter).code == "invalid_parameter");
    parameters["gain_db"] = std::numeric_limits<double>::infinity();
    const auto infinite_parameter = adapter.process({parameters, inputs, context});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(infinite_parameter));
    REQUIRE(std::get<audioalgo::Diagnostic>(infinite_parameter).code == "invalid_parameter");
    parameters["gain_db"] = std::string("6");
    const auto wrong_type = adapter.process({parameters, inputs, context});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(wrong_type));
    REQUIRE(std::get<audioalgo::Diagnostic>(wrong_type).code == "parameter_type");
    parameters["gain_db"] = 12.01;
    const auto outside_range = adapter.process({parameters, inputs, context});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(outside_range));
    REQUIRE(std::get<audioalgo::Diagnostic>(outside_range).code == "parameter_out_of_range");

    audioalgo::NodeRegistry descriptors;
    audioalgo::register_builtin_nodes(descriptors);
    audioalgo::ProjectModel project;
    project.project_id = "ar05c-invalid-input";
    project.name = "Reject non-finite input";
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 100.0, 0.0},
        {"output", "builtin.output", {}, 200.0, 0.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";
    auto non_finite_input = input;
    non_finite_input.mutable_channel(0)[1] = std::numeric_limits<float>::quiet_NaN();
    const auto runtime_result = audioalgo::render_audio_graph(project, descriptors, non_finite_input, 2);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(runtime_result));
    REQUIRE(std::get<audioalgo::Diagnostic>(runtime_result).code == "non_finite_audio");
    REQUIRE(std::get<audioalgo::Diagnostic>(runtime_result).message.find("gain") != std::string::npos);
}
