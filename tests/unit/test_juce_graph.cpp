#include "audio_juce_backend/JuceGraph.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>

using audioalgo::AudioBuffer;
using audioalgo::JuceProcessorRegistry;
using audioalgo::NodeRegistry;
using audioalgo::ProjectModel;
using audioalgo::register_builtin_juce_processors;
using audioalgo::register_builtin_node_descriptors;
using audioalgo::render_audio_graph;

namespace {
ProjectModel stereo_gain_project() {
    ProjectModel project;
    project.project_id = "graph-03-stereo";
    project.name = "Distinct channel routing";
    project.nodes = {
        {"file", "builtin.file_source", { {"path", std::string("unused.wav")} }, 10.0, 10.0},
        {"gain", "builtin.gain", { {"gain_db", -6.0} }, 250.0, 10.0},
        {"output", "builtin.output", {}, 500.0, 10.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";
    return project;
}
} // namespace

TEST_CASE("GRAPH-03 domain connections preserve distinct JUCE graph channels", "[G1][GRAPH-03]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    JuceProcessorRegistry registry;
    register_builtin_juce_processors(registry);

    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.2 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.3 * std::cos(frame * 0.023));
    }

    const auto project = stereo_gain_project();
    const auto rendered = render_audio_graph(project, descriptors, registry, input, 127);
    REQUIRE(std::holds_alternative<AudioBuffer>(rendered));
    const auto& output = std::get<AudioBuffer>(rendered);
    REQUIRE(output.channel_count() == 2);
    REQUIRE(output.frame_count() == input.frame_count());
    REQUIRE(output.channel_ids() == input.channel_ids());

    const auto gain = std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            REQUIRE(output.channel(channel)[frame] == Catch::Approx(input.channel(channel)[frame] * gain).margin(2.0e-6));
    }

    auto moved = project;
    moved.revision += 1;
    for (auto& node : moved.nodes) {
        node.x += 83.0;
        node.y -= 19.0;
    }
    const auto moved_render = render_audio_graph(moved, descriptors, registry, input, 127);
    REQUIRE(std::holds_alternative<AudioBuffer>(moved_render));
    const auto& moved_output = std::get<AudioBuffer>(moved_render);
    for (std::size_t channel = 0; channel < 2; ++channel)
        REQUIRE(std::equal(output.channel(channel).begin(), output.channel(channel).end(), moved_output.channel(channel).begin()));
}

TEST_CASE("GRAPH-02 descriptor defaults and gain boundaries control real JUCE samples", "[G1][GRAPH-02]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);

    ProjectModel project;
    project.project_id = "graph-02-gain";
    project.name = "Gain descriptor contract";
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
        {"gain", "builtin.gain", {}, 100.0, 0.0},
        {"output", "builtin.output", {}, 200.0, 0.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";

    AudioBuffer input(16000, {"mic"}, 7);
    const std::array<float, 7> samples{-0.75f, -0.25f, -0.01f, 0.0f, 0.125f, 0.5f, 0.9f};
    std::copy(samples.begin(), samples.end(), input.mutable_channel(0).begin());

    const std::array<double, 5> gains{-60.0, -6.0, 0.0, 6.0, 12.0};
    for (const auto gain_db : gains) {
        project.nodes[1].parameters["gain_db"] = gain_db;
        const auto outcome = render_audio_graph(project, descriptors, processors, input, 3);
        REQUIRE(std::holds_alternative<AudioBuffer>(outcome));
        const auto& output = std::get<AudioBuffer>(outcome);
        REQUIRE(output.frame_count() == input.frame_count());
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto expected = static_cast<double>(input.channel(0)[frame]) * std::pow(10.0, gain_db / 20.0);
            const auto tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
            REQUIRE(std::abs(static_cast<double>(output.channel(0)[frame]) - expected) <= tolerance);
        }
    }

    project.nodes[1].parameters.erase("gain_db");
    const auto default_outcome = render_audio_graph(project, descriptors, processors, input, 3);
    REQUIRE(std::holds_alternative<AudioBuffer>(default_outcome));
    REQUIRE(std::equal(input.channel(0).begin(), input.channel(0).end(),
                       std::get<AudioBuffer>(default_outcome).channel(0).begin()));

    project.nodes[1].parameters["gain_db"] = 12.1;
    const auto outside_range = render_audio_graph(project, descriptors, processors, input, 3);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(outside_range));
    REQUIRE(std::get<audioalgo::Diagnostic>(outside_range).code == "parameter_out_of_range");

    project.nodes[1].parameters["gain_db"] = std::string("6");
    const auto wrong_type = render_audio_graph(project, descriptors, processors, input, 3);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(wrong_type));
    REQUIRE(std::get<audioalgo::Diagnostic>(wrong_type).code == "parameter_type");
}
