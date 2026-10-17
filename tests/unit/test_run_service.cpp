#include "audio_io/WavFile.h"
#include "../../apps/editor/EditorGeometry.h"
#include "../../apps/editor/EditorNodeCreation.h"
#include "../../apps/editor/EditorTopologyCommands.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_run_service/RunService.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using audioalgo::AudioBuffer;
using audioalgo::NodeRegistry;
using audioalgo::ProjectModel;
using audioalgo::RunRequest;
using audioalgo::RunService;
using audioalgo::RunSuccess;
using audioalgo::WavEncoding;
using audioalgo::load_project_file;
using audioalgo::read_wav_file;
using audioalgo::register_builtin_juce_processors;
using audioalgo::register_builtin_node_descriptors;
using audioalgo::save_project_file;
using audioalgo::write_wav_file;

namespace {
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("AudioAlgo 音频 E2E " + juce::Uuid().toString().toStdString());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct RunMetadata {
    std::string run_id;
    std::string project_id;
    std::uint64_t project_revision = 0;
    std::string input_file;
    std::string input_sha256;
    std::string output_sha256;
};

std::optional<RunMetadata> read_run_metadata(const std::filesystem::path& path) {
    const auto path_utf8 = path_to_utf8(path);
    const auto json = juce::JSON::parse(
        juce::File(juce::String::fromUTF8(path_utf8.c_str())).loadFileAsString());
    const auto* object = json.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;
    const auto run_id = object->getProperty("run_id");
    const auto project_id = object->getProperty("project_id");
    const auto revision = object->getProperty("project_revision");
    const auto input_file = object->getProperty("input_file");
    const auto input_hash = object->getProperty("input_sha256");
    const auto output_hash = object->getProperty("output_sha256");
    if (!run_id.isString() || !project_id.isString() || (!revision.isInt64() && !revision.isInt()) ||
        !input_file.isString() || !input_hash.isString() || !output_hash.isString())
        return std::nullopt;
    return RunMetadata{
        run_id.toString().toStdString(), project_id.toString().toStdString(),
        static_cast<std::uint64_t>(static_cast<juce::int64>(revision)),
        input_file.toString().toStdString(),
        input_hash.toString().toStdString(), output_hash.toString().toStdString()};
}

struct CliRunOutcome {
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    std::string output;
};

CliRunOutcome run_cli(const std::filesystem::path& project_path, const std::filesystem::path& output_directory) {
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getSiblingFile("audio_cli.exe");
    const auto project_path_utf8 = path_to_utf8(project_path);
    const auto output_path_utf8 = path_to_utf8(output_directory);
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(project_path_utf8.c_str()));
    arguments.add(juce::String::fromUTF8(output_path_utf8.c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    const auto started = child.start(arguments);
    if (!started)
        return {false, false, -1, {}};
    const auto finished = child.waitForProcessToFinish(30000);
    return {true, finished, static_cast<int>(child.getExitCode()), child.readAllProcessOutput().toStdString()};
}

ProjectModel make_project() {
    ProjectModel project;
    project.project_id = "e2e-gain-project";
    project.name = "E2E Gain run";
    project.revision = 4;
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 30.0},
        {"gain", "builtin.gain", {{"gain_db", 6.0}}, 220.0, 30.0},
        {"output", "builtin.output", {}, 430.0, 30.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";
    return project;
}
} // namespace

TEST_CASE("E2E-01 RunService and CLI share a project snapshot and gain oracle", "[G1][E2E-01]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "gain project.aaproj";
    const auto input_path = temporary.path / "input.wav";

    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.4 * std::sin(frame * 0.031));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.2 * std::cos(frame * 0.047));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, make_project())));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);

    const auto service_output_dir = temporary.path / "service-run";
    const auto service_run = service.run({std::get<ProjectModel>(loaded), project_path, service_output_dir, 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_run));
    const auto& service_result = std::get<RunSuccess>(service_run);
    REQUIRE(std::filesystem::exists(service_result.complete_file));
    REQUIRE(std::filesystem::exists(service_result.manifest_file));
    REQUIRE(service_result.frames == input.frame_count());
    REQUIRE(service_result.input_sha256.size() == 64);
    REQUIRE(service_result.output_sha256.size() == 64);

    const auto service_audio = read_wav_file(service_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio));
    const auto& rendered = std::get<AudioBuffer>(service_audio);
    const auto expected_gain = std::pow(10.0, 6.0 / 20.0);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            REQUIRE(rendered.channel(channel)[frame] == Catch::Approx(input.channel(channel)[frame] * expected_gain).margin(2.0e-6));
    }

    const auto cli_output_dir = temporary.path / "cli-run";
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    const auto project_path_arg = path_to_utf8(project_path);
    const auto output_path_arg = path_to_utf8(cli_output_dir);
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(project_path_arg.c_str()));
    arguments.add(juce::String::fromUTF8(output_path_arg.c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    REQUIRE(child.start(arguments));
    REQUIRE(child.waitForProcessToFinish(30000));
    const auto cli_stdout = child.readAllProcessOutput();
    INFO(cli_stdout.toStdString());
    REQUIRE(child.getExitCode() == 0);
    REQUIRE(std::filesystem::exists(cli_output_dir / "complete.json"));

    const auto cli_audio = read_wav_file(cli_output_dir / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio));
    const auto& cli_rendered = std::get<AudioBuffer>(cli_audio);
    REQUIRE(cli_rendered.frame_count() == rendered.frame_count());
    for (std::size_t channel = 0; channel < rendered.channel_count(); ++channel)
        REQUIRE(std::equal(rendered.channel(channel).begin(), rendered.channel(channel).end(), cli_rendered.channel(channel).begin()));
}

TEST_CASE("NED-04.c", "[node_interaction][NED-04]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    auto project = make_project();
    project.connections.clear();
    std::get<std::string>(project.nodes.front().parameters.at("path")) = "missing.wav";
    const auto project_path = temporary.path / "incomplete.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);

    const auto service_output = temporary.path / "service-run";
    const auto service_result = service.run({project, project_path, service_output, 127});
    const auto* service_error = std::get_if<audioalgo::Diagnostic>(&service_result);
    REQUIRE(service_error != nullptr);
    REQUIRE(service_error->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(service_output));

    const auto cli_output = temporary.path / "cli-run";
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(path_to_utf8(project_path).c_str()));
    arguments.add(juce::String::fromUTF8(path_to_utf8(cli_output).c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    REQUIRE(child.start(arguments));
    REQUIRE(child.waitForProcessToFinish(30000));
    const auto cli_output_text = child.readAllProcessOutput();
    INFO(cli_output_text.toStdString());
    REQUIRE(child.getExitCode() == 1);
    REQUIRE(cli_output_text.contains("input_required"));
    REQUIRE_FALSE(std::filesystem::exists(cli_output / "complete.json"));
}

TEST_CASE("NED-10.g", "[node_interaction][NED-10]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;

    auto project = make_project();
    project.revision = 40;
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    REQUIRE(descriptors.register_descriptor({
        "test.required_side_input", "1.0.0", "Required side input block",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"control_in", "control.value@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}}, {},
        audioalgo::NodeExecutionKind::block}));

    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "test.required_side_input", "sidechain", target, 310.0, 30.0);
    REQUIRE(inserted.has_value());
    REQUIRE(inserted->input_port_id == "audio_in");
    REQUIRE(inserted->output_port_id == "audio_out");
    REQUIRE(project.nodes.size() == 4);
    REQUIRE(project.connections.size() == 3);
    const auto has_connection = [&](const audioalgo::Connection& expected) {
        return std::count_if(project.connections.begin(), project.connections.end(), [&](const auto& actual) {
            return actual.from_node == expected.from_node && actual.from_port == expected.from_port &&
                   actual.to_node == expected.to_node && actual.to_port == expected.to_port;
        }) == 1;
    };
    REQUIRE(has_connection({"file", "audio", "sidechain", "audio_in"}));
    REQUIRE(has_connection({"sidechain", "audio_out", "gain", "audio_in"}));
    REQUIRE(has_connection({"gain", "audio_out", "output", "audio"}));
    REQUIRE(project.revision == 41);

    const auto edit_diagnostics = audioalgo::validate_project_structure(project, descriptors);
    REQUIRE(edit_diagnostics.empty());
    const auto run_diagnostics = audioalgo::validate_project(project, descriptors);
    const auto missing_side_input = std::find_if(run_diagnostics.begin(), run_diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "input_required" && diagnostic.node_id == "sidechain" &&
               diagnostic.port_id == "control_in";
    });
    REQUIRE(missing_side_input != run_diagnostics.end());

    const auto project_path = temporary.path / "inserted-incomplete.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto output_directory = temporary.path / "incomplete-run";
    const auto run_result = service.run({project, project_path, output_directory, 127});
    const auto* run_error = std::get_if<audioalgo::Diagnostic>(&run_result);
    REQUIRE(run_error != nullptr);
    REQUIRE(run_error->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(output_directory));
}

TEST_CASE("NED-06.d", "[node_interaction][NED-06]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "layout-only.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.35 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.18 * std::cos(frame * 0.051));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto original_revision = project.revision;

    NodeRegistry geometry_registry;
    register_builtin_node_descriptors(geometry_registry);
    const auto preview_transform = audioalgo::editor::CanvasCoordinateTransform{};
    auto preview_node = project.nodes[1];
    preview_node.x = 220.0;
    preview_node.y = 180.0;
    const auto preview_geometry = audioalgo::editor::make_node_geometry(
        preview_node, geometry_registry.find(preview_node.type_id), preview_transform);
    const auto preview_input = std::find_if(preview_geometry.ports.begin(), preview_geometry.ports.end(),
        [](const auto& port) { return port.id == "audio_in"; });
    const auto preview_output = std::find_if(preview_geometry.ports.begin(), preview_geometry.ports.end(),
        [](const auto& port) { return port.id == "audio_out"; });
    REQUIRE(preview_input != preview_geometry.ports.end());
    REQUIRE(preview_output != preview_geometry.ports.end());
    const audioalgo::editor::NodePositionPreview position_preview{
        preview_node.id, {static_cast<float>(preview_node.x), static_cast<float>(preview_node.y)}};
    const auto incoming_during_drag = audioalgo::editor::make_connection_geometry(
        project, geometry_registry, project.connections[0], preview_transform, position_preview);
    const auto outgoing_during_drag = audioalgo::editor::make_connection_geometry(
        project, geometry_registry, project.connections[1], preview_transform, position_preview);
    REQUIRE(incoming_during_drag.has_value());
    REQUIRE(outgoing_during_drag.has_value());
    REQUIRE(incoming_during_drag->finish.x == preview_input->anchor.x);
    REQUIRE(incoming_during_drag->finish.y == preview_input->anchor.y);
    REQUIRE(outgoing_during_drag->start.x == preview_output->anchor.x);
    REQUIRE(outgoing_during_drag->start.y == preview_output->anchor.y);
    REQUIRE(project.nodes[1].x == 220.0);
    REQUIRE(project.nodes[1].y == 30.0);
    REQUIRE(project.revision == original_revision);

    audioalgo::editor::ProjectDirtyState dirty_state;
    REQUIRE_FALSE(dirty_state.is_modified());
    REQUIRE(audioalgo::editor::move_node_layout(project, dirty_state, "gain", 310.0, 180.0));
    REQUIRE(dirty_state.is_modified());
    REQUIRE(project.revision == original_revision);
    REQUIRE(project.nodes[1].x == 310.0);
    REQUIRE(project.nodes[1].y == 180.0);
    REQUIRE_FALSE(audioalgo::editor::move_node_layout(project, dirty_state, "missing", 0.0, 0.0));

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto before = service.run({make_project(), project_path, temporary.path / "before-run", 127});
    const auto after = service.run({project, project_path, temporary.path / "after-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(before));
    REQUIRE(std::holds_alternative<RunSuccess>(after));
    const auto& before_result = std::get<RunSuccess>(before);
    const auto& after_result = std::get<RunSuccess>(after);
    REQUIRE(before_result.output_sha256 == after_result.output_sha256);
    REQUIRE(before_result.input_sha256 == after_result.input_sha256);
    REQUIRE(before_result.frames == after_result.frames);

    const auto before_audio = read_wav_file(before_result.output_file);
    const auto after_audio = read_wav_file(after_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(before_audio));
    REQUIRE(std::holds_alternative<AudioBuffer>(after_audio));
    const auto& first = std::get<AudioBuffer>(before_audio);
    const auto& second = std::get<AudioBuffer>(after_audio);
    REQUIRE(first.channel_count() == second.channel_count());
    REQUIRE(first.frame_count() == second.frame_count());
    for (std::size_t channel = 0; channel < first.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < first.frame_count(); ++frame)
            REQUIRE(first.channel(channel)[frame] == second.channel(channel)[frame]);

    dirty_state.mark_saved();
    REQUIRE_FALSE(dirty_state.is_modified());
}

TEST_CASE("NED-09.b", "[node_interaction][NED-09]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "recreated-output.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.3 * std::sin(frame * 0.041));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.16 * std::cos(frame * 0.029));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    auto project = make_project();
    REQUIRE(audioalgo::editor::delete_node(project, descriptors, "output"));
    REQUIRE(project.selected_output_node_id.empty());
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-recreated", 640.0, 60.0));
    REQUIRE(project.selected_output_node_id == "output-recreated");
    const auto revision_with_sink = project.revision;
    const auto node_count_with_sink = project.nodes.size();
    const auto connection_count_with_sink = project.connections.size();
    REQUIRE_FALSE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-second", 840.0, 60.0));
    REQUIRE(project.selected_output_node_id == "output-recreated");
    REQUIRE(project.revision == revision_with_sink);
    REQUIRE(project.nodes.size() == node_count_with_sink);
    REQUIRE(project.connections.size() == connection_count_with_sink);

    REQUIRE(audioalgo::try_add_connection(project, {"gain", "audio_out", "output-recreated", "audio"}, descriptors).empty());
    REQUIRE(project.revision == revision_with_sink + 1);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto outcome = service.run({project, project_path, temporary.path / "recreated-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    REQUIRE(std::filesystem::exists(result.complete_file));
    REQUIRE(result.output_sha256.size() == 64);
}

TEST_CASE("NED-09.d", "[node_interaction][NED-09]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "recreated-output-roundtrip.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 479);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.22 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.11 * std::cos(frame * 0.053));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(temporary.path / "input.wav", input, WavEncoding::float32)));

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    auto project = make_project();
    REQUIRE(audioalgo::editor::delete_node(project, descriptors, "output"));
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-reopened", 650.0, 30.0));
    REQUIRE(audioalgo::try_add_connection(
        project, {"gain", "audio_out", "output-reopened", "audio"}, descriptors).empty());
    REQUIRE(project.selected_output_node_id == "output-reopened");
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.selected_output_node_id == "output-reopened");
    REQUIRE(reopened.revision == project.revision);

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto outcome = service.run({reopened, project_path, temporary.path / "reopened-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    REQUIRE(std::filesystem::exists(result.complete_file));
}

TEST_CASE("NED-13.b", "[node_interaction][NED-13]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-input-preservation.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.31 * std::sin(frame * 0.043));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.17 * std::cos(frame * 0.029));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));
    const auto input_bytes_before_save = read_file_bytes(input_path);
    REQUIRE_FALSE(input_bytes_before_save.empty());

    auto project = make_project();
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "saved-input-preservation-gain",
        {"gain", "audio_out", "output", "audio"}, 324.0, 54.0);
    REQUIRE(inserted.has_value());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(std::get<std::string>(reopened.nodes.front().parameters.at("path")) == "input.wav");
    REQUIRE(read_file_bytes(input_path) == input_bytes_before_save);

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto original_run = service.run({project, project_path, temporary.path / "original-run", 127});
    const auto reopened_run = service.run({reopened, project_path, temporary.path / "reopened-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(original_run));
    REQUIRE(std::holds_alternative<RunSuccess>(reopened_run));
    const auto& original_result = std::get<RunSuccess>(original_run);
    const auto& reopened_result = std::get<RunSuccess>(reopened_run);
    REQUIRE(original_result.input_sha256.size() == 64);
    REQUIRE(reopened_result.input_sha256 == original_result.input_sha256);
    REQUIRE(read_file_bytes(input_path) == input_bytes_before_save);
}

TEST_CASE("NED-13.c", "[node_interaction][NED-13]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-oracle-roundtrip.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.38 * std::sin(frame * 0.031));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.21 * std::cos(frame * 0.047));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "oracle-inserted-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "oracle-inserted-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(std::any_of(reopened.connections.begin(), reopened.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "oracle-inserted-gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(reopened.connections.begin(), reopened.connections.end(), [](const auto& edge) {
        return edge.from_node == "oracle-inserted-gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto outcome = service.run({reopened, project_path, temporary.path / "inserted-oracle-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    const auto output = read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(output));
    const auto& rendered = std::get<AudioBuffer>(output);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());

    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(rendered.channel(channel)[frame]) - oracle) <= 2.0e-6);
        }
    }
}

TEST_CASE("NED-14.a", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "new-graph.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.27 * std::sin(frame * 0.039));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.13 * std::cos(frame * 0.057));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    ProjectModel project;
    project.project_id = "ned14-a-new-graph-project";
    project.name = "New graph workflow";
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    auto source = audioalgo::editor::make_node_instance(
        descriptors, project.nodes, "builtin.file_source", "new-file", 40.0, 60.0);
    REQUIRE(source.has_value());
    source->parameters["path"] = std::string("input.wav");
    project.nodes.push_back(std::move(*source));
    ++project.revision;
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.gain", "new-gain", 270.0, 60.0));
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "new-output", 500.0, 60.0));
    REQUIRE(project.selected_output_node_id == "new-output");
    REQUIRE(audioalgo::try_add_connection(
        project, {"new-file", "audio", "new-gain", "audio_in"}, descriptors).empty());
    REQUIRE(audioalgo::try_add_connection(
        project, {"new-gain", "audio_out", "new-output", "audio"}, descriptors).empty());
    REQUIRE(project.nodes.size() == 3);
    REQUIRE(project.connections.size() == 2);
    REQUIRE(audioalgo::validate_project(project, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto outcome = service.run({project, project_path, temporary.path / "new-graph-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    const auto metadata = read_run_metadata(result.manifest_file);
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->project_id == project.project_id);
    REQUIRE(metadata->project_revision == project.revision);
    REQUIRE(metadata->run_id == result.run_id);
    REQUIRE(metadata->input_sha256 == result.input_sha256);
    REQUIRE(metadata->output_sha256 == result.output_sha256);
    const auto rendered_result = read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(rendered_result));
    const auto& rendered = std::get<AudioBuffer>(rendered_result);
    const auto gain = std::pow(10.0, 0.0 / 20.0);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain;
            REQUIRE(std::abs(static_cast<double>(rendered.channel(channel)[frame]) - oracle) <= 2.0e-6);
        }
}

TEST_CASE("NED-14.b", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-unity.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.41 * std::sin(frame * 0.033));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.19 * std::cos(frame * 0.051));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-b-inserted-unity-project";
    project.name = "Inserted gain unity acceptance";
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "inserted-minus-six-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "inserted-minus-six-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto service_outcome = service.run(
        {reopened, project_path, temporary.path / "inserted-unity-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    REQUIRE(service_metadata.has_value());

    const auto cli = run_cli(project_path, temporary.path / "inserted-unity-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto cli_metadata = read_run_metadata(temporary.path / "inserted-unity-cli" / "run.json");
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == project.project_id);
    REQUIRE(cli_metadata->project_id == project.project_id);
    REQUIRE(service_metadata->project_revision == project.revision);
    REQUIRE(cli_metadata->project_revision == project.revision);
    REQUIRE(service_metadata->run_id == service_result.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "inserted-unity-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.channel_count() == input.channel_count());
    REQUIRE(cli_audio.channel_count() == input.channel_count());
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
    }
}

TEST_CASE("NED-14.c", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-zero-gain.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.36 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.23 * std::cos(frame * 0.049));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-c-inserted-zero-gain-project";
    project.name = "Inserted zero gain participation acceptance";
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "inserted-zero-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "inserted-zero-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    REQUIRE(std::get<double>(inserted_node->parameters.at("gain_db")) == 0.0);
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto service_outcome = service.run(
        {reopened, project_path, temporary.path / "inserted-zero-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    REQUIRE(service_metadata.has_value());

    const auto cli = run_cli(project_path, temporary.path / "inserted-zero-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto cli_metadata = read_run_metadata(temporary.path / "inserted-zero-cli" / "run.json");
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == project.project_id);
    REQUIRE(cli_metadata->project_id == project.project_id);
    REQUIRE(service_metadata->project_revision == project.revision);
    REQUIRE(cli_metadata->project_revision == project.revision);
    REQUIRE(service_metadata->run_id == service_result.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "inserted-zero-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.channel_count() == input.channel_count());
    REQUIRE(cli_audio.channel_count() == input.channel_count());
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
    }
}

TEST_CASE("NED-14.d", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-reconnect.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 385);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.34 * std::sin(frame * 0.035));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.2 * std::cos(frame * 0.061));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-d-reconnect-project";
    project.name = "Disconnect reconnect persistence";
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const audioalgo::Connection original_output_edge{
        "gain", "audio_out", "output", "audio"};
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "reconnect-inserted-gain",
        original_output_edge, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "reconnect-inserted-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const audioalgo::Connection inserted_output_edge{
        "reconnect-inserted-gain", "audio_out", "output", "audio"};
    REQUIRE(audioalgo::editor::try_disconnect_connection(project, inserted_output_edge));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto disconnected_output = temporary.path / "disconnected-service";
    const auto disconnected_service = service.run({
        project, project_path, disconnected_output, 127});
    const auto* disconnected_diagnostic = std::get_if<audioalgo::Diagnostic>(&disconnected_service);
    REQUIRE(disconnected_diagnostic != nullptr);
    REQUIRE(disconnected_diagnostic->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(disconnected_output));
    const auto disconnected_cli_dir = temporary.path / "disconnected-cli";
    const auto disconnected_cli = run_cli(project_path, disconnected_cli_dir);
    REQUIRE(disconnected_cli.started);
    REQUIRE(disconnected_cli.finished);
    INFO(disconnected_cli.output);
    REQUIRE(disconnected_cli.exit_code == 1);
    REQUIRE(disconnected_cli.output.find("input_required") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(disconnected_cli_dir / "complete.json"));

    REQUIRE(audioalgo::try_add_connection(project, inserted_output_edge, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto reopened_result = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(reopened_result));
    const auto& reopened = std::get<ProjectModel>(reopened_result);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);
    REQUIRE(std::count_if(reopened.connections.begin(), reopened.connections.end(), [&](const auto& edge) {
        return edge.from_node == inserted_output_edge.from_node && edge.from_port == inserted_output_edge.from_port &&
               edge.to_node == inserted_output_edge.to_node && edge.to_port == inserted_output_edge.to_port;
    }) == 1);

    const auto service_success = service.run({
        reopened, project_path, temporary.path / "reconnected-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_success));
    const auto& service_result = std::get<RunSuccess>(service_success);
    const auto cli_success = run_cli(project_path, temporary.path / "reconnected-cli");
    REQUIRE(cli_success.started);
    REQUIRE(cli_success.finished);
    INFO(cli_success.output);
    REQUIRE(cli_success.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "reconnected-cli" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == reopened.project_id);
    REQUIRE(cli_metadata->project_id == reopened.project_id);
    REQUIRE(service_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);
    REQUIRE(std::filesystem::exists(service_result.complete_file));
    REQUIRE(std::filesystem::exists(temporary.path / "reconnected-cli" / "complete.json"));

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "reconnected-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
}

TEST_CASE("NED-14.e", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "correlated-inserted-project.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 129);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.29 * std::sin(frame * 0.071));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.14 * std::cos(frame * 0.053));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-e-correlated-project";
    project.name = "Correlated inserted graph";
    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "correlated-inserted-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    REQUIRE(project.nodes.size() == 4);
    REQUIRE(project.connections.size() == 3);
    REQUIRE(std::none_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "file" && edge.from_port == "audio" &&
               edge.to_node == "gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "correlated-inserted-gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "correlated-inserted-gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));
    for (const auto& node : project.nodes)
        REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [&](const auto& edge) {
            return edge.from_node == node.id || edge.to_node == node.id;
        }));
    REQUIRE(audioalgo::validate_project(project, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto service_result = service.run({
        reopened, project_path, temporary.path / "correlated-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_result));
    const auto& service_success = std::get<RunSuccess>(service_result);
    const auto cli = run_cli(project_path, temporary.path / "correlated-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_success.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "correlated-cli" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == reopened.project_id);
    REQUIRE(cli_metadata->project_id == reopened.project_id);
    REQUIRE(service_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->project_revision == reopened.revision);
    REQUIRE(service_metadata->run_id == service_success.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(service_metadata->input_sha256 == cli_metadata->input_sha256);
    REQUIRE(service_metadata->output_sha256 == service_success.output_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_success.output_sha256);
    REQUIRE(std::filesystem::exists(service_success.complete_file));
    REQUIRE(std::filesystem::exists(temporary.path / "correlated-cli" / "complete.json"));
    REQUIRE(std::filesystem::exists(service_success.output_file));
    REQUIRE(std::filesystem::exists(temporary.path / "correlated-cli" / "output.wav"));
}

TEST_CASE("NED-01.b", "[node_interaction][NED-01]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_filename = std::filesystem::u8path("输入 tone final.wav");
    const auto input_path = temporary.path / input_filename;
    const auto project_path = temporary.path / std::filesystem::u8path("中文 工程 file.aaproj");
    AudioBuffer input(48000, {"left", "right"}, 321);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.28 * std::sin(frame * 0.047));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.15 * std::cos(frame * 0.039));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));
    const auto original_input_bytes = read_file_bytes(input_path);
    REQUIRE_FALSE(original_input_bytes.empty());

    auto project = make_project();
    project.project_id = "ned01-b-chinese-path-project";
    project.name = "中文工程 · −6 dB …";
    std::get<std::string>(project.nodes.front().parameters.at("path")) = path_to_utf8(input_filename);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.name == "中文工程 · −6 dB …");
    REQUIRE(std::get<std::string>(reopened.nodes.front().parameters.at("path")) == path_to_utf8(input_filename));
    REQUIRE(read_file_bytes(input_path) == original_input_bytes);

    NodeRegistry descriptors;
    register_builtin_node_descriptors(descriptors);
    audioalgo::JuceProcessorRegistry processors;
    register_builtin_juce_processors(processors);
    RunService service(descriptors, processors);
    const auto service_outcome = service.run({
        reopened, project_path, temporary.path / "中文 service run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto cli = run_cli(project_path, temporary.path / "中文 cli run");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "中文 cli run" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->input_file == path_to_utf8(input_filename));
    REQUIRE(cli_metadata->input_file == path_to_utf8(input_filename));
    REQUIRE(service_metadata->input_sha256 == cli_metadata->input_sha256);
    REQUIRE(service_metadata->input_sha256 == service_result.input_sha256);
    REQUIRE(read_file_bytes(input_path) == original_input_bytes);
}
