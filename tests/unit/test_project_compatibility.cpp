#include "audio_io/WavFile.h"
#include "audio_editor_application/EditorController.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_project/ProjectResourceService.h"
#include "audio_run_service/RunService.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        std::filesystem::u8path("AR-13.a legacy " + juce::Uuid().toString().toStdString());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

bool same_project_data(const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
    if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
        left.name != right.name || left.revision != right.revision ||
        left.selected_output_node_id != right.selected_output_node_id || left.nodes.size() != right.nodes.size() ||
        left.connections.size() != right.connections.size() ||
        left.serialized_extensions != right.serialized_extensions)
        return false;
    for (std::size_t index = 0; index < left.nodes.size(); ++index) {
        const auto& a = left.nodes[index];
        const auto& b = right.nodes[index];
        if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters || a.x != b.x || a.y != b.y ||
            a.opaque_parameters != b.opaque_parameters || a.serialized_extensions != b.serialized_extensions)
            return false;
    }
    for (std::size_t index = 0; index < left.connections.size(); ++index) {
        const auto& a = left.connections[index];
        const auto& b = right.connections[index];
        if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node ||
            a.to_port != b.to_port || a.serialized_extensions != b.serialized_extensions)
            return false;
    }
    return true;
}

std::string compact_json(const juce::var& value) {
    const auto text = juce::JSON::toString(value, false);
    const auto json = text.toUTF8();
    return std::string(json.getAddress(), static_cast<std::size_t>(json.sizeInBytes() - 1));
}

} // namespace

TEST_CASE("AR-13.a Frozen schema1 Gain project loads saves reopens and runs against the legacy oracle",
          "[R2][AR-13.a]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto fixture_directory = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build" / "reports" / "loops" / "RF0000" / "legacy_fixture";
    const auto fixture_project = fixture_directory / "gain_demo.aaproj";
    const auto fixture_input = fixture_directory / "input.wav";
    REQUIRE(std::filesystem::is_regular_file(fixture_project));
    REQUIRE(std::filesystem::is_regular_file(fixture_input));
    const auto frozen_project_bytes = read_bytes(fixture_project);
    const auto frozen_input_bytes = read_bytes(fixture_input);
    REQUIRE_FALSE(frozen_project_bytes.empty());
    REQUIRE_FALSE(frozen_input_bytes.empty());

    const auto project_path = temporary.path / "legacy copy.aaproj";
    const auto input_path = temporary.path / "input.wav";
    REQUIRE(std::filesystem::copy_file(fixture_project, project_path));
    REQUIRE(std::filesystem::copy_file(fixture_input, input_path));
    std::filesystem::permissions(project_path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);

    const auto loaded = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    const auto& before_save = std::get<audioalgo::ProjectModel>(loaded);
    REQUIRE(before_save.schema_version == 1);
    REQUIRE(before_save.project_id == "audioalgo-demo-gain-001");
    REQUIRE(before_save.name == "Stereo Gain Demo");
    REQUIRE(before_save.revision == 1);
    REQUIRE(before_save.nodes.size() == 3);
    REQUIRE(before_save.connections.size() == 2);
    REQUIRE(before_save.nodes[0].id == "file");
    REQUIRE(before_save.nodes[1].id == "gain");
    REQUIRE(before_save.nodes[2].id == "output");
    REQUIRE(before_save.nodes[0].x == 60.0);
    REQUIRE(before_save.nodes[0].y == 150.0);
    REQUIRE(before_save.nodes[1].x == 340.0);
    REQUIRE(before_save.nodes[1].y == 150.0);
    REQUIRE(before_save.nodes[2].x == 620.0);
    REQUIRE(before_save.nodes[2].y == 150.0);
    REQUIRE(std::get<std::string>(before_save.nodes[0].parameters.at("path")) == "input.wav");
    REQUIRE(std::get<double>(before_save.nodes[1].parameters.at("gain_db")) == 6.0);
    REQUIRE(before_save.connections[0].from_node == "file");
    REQUIRE(before_save.connections[0].to_node == "gain");
    REQUIRE(before_save.connections[1].from_node == "gain");
    REQUIRE(before_save.connections[1].to_node == "output");
    REQUIRE(before_save.selected_output_node_id == "output");

    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, before_save)));
    const auto reopened_result = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(reopened_result));
    const auto reopened = std::get<audioalgo::ProjectModel>(reopened_result);
    CHECK(same_project_data(reopened, before_save));

    const auto input_result = audioalgo::read_wav_file(input_path);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(input_result));
    const auto& input = std::get<audioalgo::AudioBuffer>(input_result);
    audioalgo::NodeRegistry descriptors;
    REQUIRE(audioalgo::register_builtin_nodes(descriptors));
    audioalgo::RunService service(descriptors);
    const auto run = service.run({reopened, project_path, temporary.path / "legacy run", 127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(run));
    const auto& success = std::get<audioalgo::RunSuccess>(run);
    CHECK(success.frames == input.frame_count());
    CHECK(success.identity.project_id == reopened.project_id);
    CHECK(success.identity.revision == reopened.revision);
    CHECK(success.input_sha256.size() == 64);
    CHECK(std::filesystem::is_regular_file(success.output_file));
    CHECK(std::filesystem::is_regular_file(success.manifest_file));
    CHECK(std::filesystem::is_regular_file(success.complete_file));

    const auto output_result = audioalgo::read_wav_file(success.output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(output_result));
    const auto& output = std::get<audioalgo::AudioBuffer>(output_result);
    REQUIRE(output.frame_count() == input.frame_count());
    REQUIRE(output.sample_rate_hz() == input.sample_rate_hz());
    REQUIRE(output.channel_count() == input.channel_count());
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        CHECK(output.channel_ids()[channel] == input.channel_ids()[channel]);
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto expected = static_cast<double>(input.channel(channel)[frame]) * gain;
            CHECK(std::abs(static_cast<double>(output.channel(channel)[frame]) - expected) <=
                  2.0e-6 + 1.0e-5 * std::abs(expected));
        }
    }
    CHECK(read_bytes(fixture_project) == frozen_project_bytes);
    CHECK(read_bytes(fixture_input) == frozen_input_bytes);
    CHECK(read_bytes(input_path) == frozen_input_bytes);

    if (const auto* artifact = std::getenv("AUDIOALGO_AR13A_ARTIFACT_OUTPUT"); artifact != nullptr && artifact[0] != '\0') {
        const auto artifact_directory = std::filesystem::u8path(artifact);
        REQUIRE_FALSE(std::filesystem::exists(artifact_directory));
        REQUIRE(std::filesystem::create_directories(artifact_directory));
        CHECK(std::filesystem::copy_file(project_path, artifact_directory / "legacy-reopened.aaproj"));
        CHECK(std::filesystem::copy_file(input_path, artifact_directory / "input.wav"));
        CHECK(std::filesystem::copy_file(success.output_file, artifact_directory / "output.wav"));
        CHECK(std::filesystem::copy_file(success.manifest_file, artifact_directory / "run.json"));
        CHECK(std::filesystem::copy_file(success.complete_file, artifact_directory / "complete.json"));
    }
}

TEST_CASE("AR-13.b Unknown plugin fields and nested parameters survive roundtrip and remain unavailable",
          "[R2][AR-13.b]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "unknown plugin.aaproj";
    const auto input_path = temporary.path / "input.wav";
    const std::string fixture = R"json({
  "schema_version": 1,
  "project_id": "unknown-plugin-project",
  "name": "Unknown plugin compatibility",
  "revision": 5,
  "selected_output_node_id": "out",
  "vendor_project": {"format_revision": 17, "flags": [true, 4.5, null]},
  "nodes": [
    {"id": "file", "type_id": "builtin.file_source", "x": 10.0, "y": 25.0,
     "parameters": {"path": "input.wav"}},
    {"id": "mystery", "type_id": "vendor.equalizer", "x": 220.0, "y": 25.0,
     "plugin_version": "3.4.5", "plugin_data": {"vendor": "Aural", "bands": [{"freq": 100, "gain": 1.25}]},
     "parameters": {"mode": "stereo", "curve": {"kind": "bezier", "points": [[0, 0.5], [1, 0.75]]}}},
    {"id": "out", "type_id": "builtin.output", "x": 430.0, "y": 25.0, "parameters": {}}
  ],
  "connections": [
    {"from_node": "file", "from_port": "audio", "to_node": "mystery", "to_port": "audio_in",
     "route_label": "legacy input", "route_metadata": {"color": "violet", "priority": 2}},
    {"from_node": "mystery", "from_port": "audio_out", "to_node": "out", "to_port": "audio"}
  ]
})json";
    {
        std::ofstream stream(project_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << fixture;
    }
    const auto input_write = audioalgo::write_wav_file(input_path,
        audioalgo::AudioBuffer(48000, {"left", "right"}, 8), audioalgo::WavEncoding::float32);
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(input_write));

    const auto expected_parsed = juce::JSON::parse(juce::String::fromUTF8(fixture.c_str()));
    REQUIRE(expected_parsed.getDynamicObject() != nullptr);
    const auto loaded = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    const auto model = std::get<audioalgo::ProjectModel>(loaded);
    REQUIRE(model.nodes.size() == 3);
    REQUIRE(model.nodes[1].type_id == "vendor.equalizer");
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, model)));

    const auto reopened = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(reopened));
    const auto actual_parsed = juce::JSON::parse(juce::File(project_path.string()).loadFileAsString());
    const auto* expected_root = expected_parsed.getDynamicObject();
    const auto* actual_root = actual_parsed.getDynamicObject();
    REQUIRE(expected_root != nullptr);
    REQUIRE(actual_root != nullptr);
    CHECK(compact_json(actual_root->getProperty("vendor_project")) ==
          compact_json(expected_root->getProperty("vendor_project")));

    const auto expected_nodes = expected_root->getProperty("nodes").getArray();
    const auto actual_nodes = actual_root->getProperty("nodes").getArray();
    REQUIRE(expected_nodes != nullptr);
    REQUIRE(actual_nodes != nullptr);
    REQUIRE(expected_nodes->size() == actual_nodes->size());
    const auto* expected_unknown = (*expected_nodes)[1].getDynamicObject();
    const auto* actual_unknown = (*actual_nodes)[1].getDynamicObject();
    REQUIRE(expected_unknown != nullptr);
    REQUIRE(actual_unknown != nullptr);
    CHECK(actual_unknown->getProperty("plugin_version") == expected_unknown->getProperty("plugin_version"));
    CHECK(compact_json(actual_unknown->getProperty("plugin_data")) ==
          compact_json(expected_unknown->getProperty("plugin_data")));
    CHECK(compact_json(actual_unknown->getProperty("parameters")) ==
          compact_json(expected_unknown->getProperty("parameters")));

    const auto expected_connections = expected_root->getProperty("connections").getArray();
    const auto actual_connections = actual_root->getProperty("connections").getArray();
    REQUIRE(expected_connections != nullptr);
    REQUIRE(actual_connections != nullptr);
    const auto* expected_route = (*expected_connections)[0].getDynamicObject();
    const auto* actual_route = (*actual_connections)[0].getDynamicObject();
    REQUIRE(expected_route != nullptr);
    REQUIRE(actual_route != nullptr);
    CHECK(actual_route->getProperty("route_label") == expected_route->getProperty("route_label"));
    CHECK(compact_json(actual_route->getProperty("route_metadata")) ==
          compact_json(expected_route->getProperty("route_metadata")));

    audioalgo::NodeRegistry descriptors;
    REQUIRE(audioalgo::register_builtin_nodes(descriptors));
    const auto diagnostics = audioalgo::validate_project(std::get<audioalgo::ProjectModel>(reopened), descriptors);
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics.front().code == "node_type_missing");
    audioalgo::RunService service(descriptors);
    const auto run = service.run({std::get<audioalgo::ProjectModel>(reopened), project_path,
                                  temporary.path / "must not render", 127});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(run));
    CHECK(std::get<audioalgo::Diagnostic>(run).code == "node_type_missing");
    CHECK_FALSE(std::filesystem::exists(temporary.path / "must not render"));
}

TEST_CASE("AR-13.c Unsupported project schema returns a migration diagnostic without rewriting the file",
          "[R2][AR-13.c]") {
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "future schema.aaproj";
    const std::string fixture = R"json({"schema_version": 7, "project_id": "future-project", "vendor": {"keep": true}})json";
    {
        std::ofstream stream(project_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << fixture;
    }
    const auto original_bytes = read_bytes(project_path);
    REQUIRE_FALSE(original_bytes.empty());

    const auto loaded = audioalgo::load_project_file(project_path);

    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(loaded));
    const auto& diagnostic = std::get<audioalgo::Diagnostic>(loaded);
    CHECK(diagnostic.code == "project_schema_unsupported");
    CHECK(diagnostic.message.find("schema version 1") != std::string::npos);
    CHECK(read_bytes(project_path) == original_bytes);
    auto backup_path = project_path;
    backup_path += ".bak";
    CHECK_FALSE(std::filesystem::exists(backup_path));
    auto version_backup_path = project_path;
    version_backup_path += ".schema-7.bak";
    CHECK_FALSE(std::filesystem::exists(version_backup_path));
}

TEST_CASE("AR-13.d Missing source resources report a relocatable project resource diagnostic",
          "[R2][AR-13.d]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto fixture_directory = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build" / "reports" / "loops" / "RF0000" / "legacy_fixture";
    const auto project_path = temporary.path / "资源 项目.aaproj";
    REQUIRE(std::filesystem::copy_file(fixture_directory / "gain_demo.aaproj", project_path));
    std::filesystem::permissions(project_path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    const auto loaded = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    auto project = std::get<audioalgo::ProjectModel>(loaded);
    const std::string missing_reference = "缺失 资源/原始录音.wav";
    project.nodes[0].parameters["path"] = missing_reference;
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, project)));
    const auto missing_project = project;
    CHECK_FALSE(std::filesystem::exists(temporary.path / std::filesystem::u8path(missing_reference)));

    auto resources = std::make_shared<audioalgo::ProjectResourceService>();
    const auto missing_resolution = resources->resolve(missing_reference, project_path);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(missing_resolution));
    CHECK(std::get<audioalgo::Diagnostic>(missing_resolution).code == "source_resource_missing");

    const auto relative_resource = temporary.path / std::filesystem::u8path("资源池") /
        std::filesystem::u8path("新录音 take.wav");
    std::filesystem::create_directories(relative_resource.parent_path());
    audioalgo::AudioBuffer relative_audio(48000, {"left", "right"}, 5);
    const std::vector<float> left_samples{0.1f, -0.25f, 0.5f, -0.8f, 0.03f};
    const std::vector<float> right_samples{-0.2f, 0.4f, -0.6f, 0.15f, 0.9f};
    std::copy(left_samples.begin(), left_samples.end(), relative_audio.mutable_channel(0).begin());
    std::copy(right_samples.begin(), right_samples.end(), relative_audio.mutable_channel(1).begin());
    const auto relative_write = audioalgo::write_wav_file(
        relative_resource, relative_audio, audioalgo::WavEncoding::float32);
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(relative_write));
    const auto relative_bytes_before = read_bytes(relative_resource);

    const auto relocated_reference = resources->relocate(missing_reference, project_path, relative_resource);
    REQUIRE(std::holds_alternative<std::string>(relocated_reference));
    CHECK(std::filesystem::u8path(std::get<std::string>(relocated_reference)) ==
          std::filesystem::u8path("资源池") / std::filesystem::u8path("新录音 take.wav"));

    audioalgo::NodeRegistry descriptors;
    REQUIRE(audioalgo::register_builtin_nodes(descriptors));
    audioalgo::editor::EditorController controller(descriptors);
    controller.adopt_project(missing_project);
    const auto relative_edit = controller.set_parameter(
        controller.session_identity(), "file", "path", std::get<std::string>(relocated_reference));
    REQUIRE(relative_edit.status == audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::save_project_file(project_path, controller.project())));
    const auto reopened_relative = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(reopened_relative));
    auto reopened_project = std::get<audioalgo::ProjectModel>(reopened_relative);
    CHECK(std::get<std::string>(reopened_project.nodes[0].parameters.at("path")) ==
          std::get<std::string>(relocated_reference));

    const auto resolved_relative = resources->resolve(std::get<std::string>(relocated_reference), project_path);
    REQUIRE(std::holds_alternative<std::filesystem::path>(resolved_relative));
    CHECK(std::get<std::filesystem::path>(resolved_relative).lexically_normal() ==
          relative_resource.lexically_normal());

    audioalgo::RunService service(descriptors, resources);
    const auto output_directory = temporary.path / "missing resource run";
    const auto missing_run = service.run({missing_project, project_path, output_directory, 127});

    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(missing_run));
    CHECK(std::get<audioalgo::Diagnostic>(missing_run).code == "source_resource_missing");
    CHECK(std::get<audioalgo::Diagnostic>(missing_run).node_id == "file");
    CHECK(std::get<audioalgo::Diagnostic>(missing_run).port_id == "path");
    CHECK_FALSE(std::filesystem::exists(output_directory));

    const auto relative_run_directory = temporary.path / "relative resource run";
    const auto relative_run = service.run({reopened_project, project_path, relative_run_directory, 3});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(relative_run));
    const auto& relative_success = std::get<audioalgo::RunSuccess>(relative_run);
    CHECK(relative_success.frames == relative_audio.frame_count());
    CHECK(relative_success.input_sha256.size() == 64);
    const auto relative_output_result = audioalgo::read_wav_file(relative_success.output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(relative_output_result));
    const auto& relative_output = std::get<audioalgo::AudioBuffer>(relative_output_result);
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (std::size_t frame = 0; frame < relative_audio.frame_count(); ++frame) {
        CHECK(relative_output.channel(0)[frame] == Catch::Approx(relative_audio.channel(0)[frame] * gain).margin(2e-6));
        CHECK(relative_output.channel(1)[frame] == Catch::Approx(relative_audio.channel(1)[frame] * gain).margin(2e-6));
    }
    CHECK(read_bytes(relative_resource) == relative_bytes_before);

    TemporaryDirectory external;
    const auto external_resource = external.path / std::filesystem::u8path("外部 素材.wav");
    const auto external_write = audioalgo::write_wav_file(
        external_resource, relative_audio, audioalgo::WavEncoding::float32);
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(external_write));
    const auto external_bytes_before = read_bytes(external_resource);
    const auto external_reference = resources->make_reference(external_resource, project_path);
    REQUIRE(std::holds_alternative<std::string>(external_reference));
    CHECK(std::filesystem::u8path(std::get<std::string>(external_reference)).is_absolute());
    const auto resolved_external = resources->resolve(std::get<std::string>(external_reference), project_path);
    REQUIRE(std::holds_alternative<std::filesystem::path>(resolved_external));
    CHECK(std::get<std::filesystem::path>(resolved_external).lexically_normal() ==
          external_resource.lexically_normal());
    const auto external_edit = controller.set_parameter(
        controller.session_identity(), "file", "path", std::get<std::string>(external_reference));
    REQUIRE(external_edit.status == audioalgo::editor::ParameterEditStatus::applied);
    const auto external_project = controller.project();
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, external_project)));
    const auto external_run = service.run({external_project, project_path,
                                          temporary.path / "external resource run", 127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(external_run));
    CHECK(std::get<audioalgo::RunSuccess>(external_run).frames == relative_audio.frame_count());
    CHECK(read_bytes(external_resource) == external_bytes_before);

    const auto reopened = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(reopened));
    CHECK(std::get<std::string>(std::get<audioalgo::ProjectModel>(reopened).nodes[0].parameters.at("path")) ==
          std::get<std::string>(external_reference));
}

TEST_CASE("AR-13.e Schema v1 layout saves without changing compute revision or creating an implicit backup",
          "[R2][AR-13.e]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto fixture_directory = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build" / "reports" / "loops" / "RF0000" / "legacy_fixture";
    const auto fixture_project = fixture_directory / "gain_demo.aaproj";
    const auto fixture_input = fixture_directory / "input.wav";
    const auto frozen_project_bytes = read_bytes(fixture_project);
    const auto frozen_input_bytes = read_bytes(fixture_input);
    const auto project_path = temporary.path / "v1 layout policy.aaproj";
    REQUIRE(std::filesystem::copy_file(fixture_project, project_path));
    std::filesystem::permissions(project_path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    const auto loaded = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    const auto original_project = std::get<audioalgo::ProjectModel>(loaded);

    audioalgo::NodeRegistry descriptors;
    REQUIRE(audioalgo::register_builtin_nodes(descriptors));
    audioalgo::editor::EditorController controller(descriptors);
    controller.adopt_project(original_project);
    controller.mark_result_current();
    const auto original_revision = controller.revision();
    const auto original_x = controller.project().nodes[1].x;
    const auto original_y = controller.project().nodes[1].y;
    const auto moved = controller.move_node_layout("gain", original_x + 27.5, original_y - 12.25);

    REQUIRE(moved.status == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(controller.revision() == original_revision);
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.dirty());
    CHECK(controller.project().nodes[1].x == original_x + 27.5);
    CHECK(controller.project().nodes[1].y == original_y - 12.25);

    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::save_project_file(project_path, controller.project())));
    auto backup_path = project_path;
    backup_path += ".bak";
    CHECK_FALSE(std::filesystem::exists(backup_path));
    auto version_backup_path = project_path;
    version_backup_path += ".schema-1.bak";
    CHECK_FALSE(std::filesystem::exists(version_backup_path));

    const auto reopened = audioalgo::load_project_file(project_path);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(reopened));
    const auto& reopened_project = std::get<audioalgo::ProjectModel>(reopened);
    CHECK(reopened_project.schema_version == 1);
    CHECK(reopened_project.revision == original_revision);
    CHECK(reopened_project.nodes[1].x == original_x + 27.5);
    CHECK(reopened_project.nodes[1].y == original_y - 12.25);
    CHECK(read_bytes(fixture_project) == frozen_project_bytes);
    CHECK(read_bytes(fixture_input) == frozen_input_bytes);
}
