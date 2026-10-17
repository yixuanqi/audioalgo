#include "audio_project/ProjectFileIO.h"
#include "../../apps/editor/EditorText.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using audioalgo::Connection;
using audioalgo::NodeInstance;
using audioalgo::ParameterValue;
using audioalgo::ProjectModel;
using audioalgo::load_project_file;
using audioalgo::save_project_file;

namespace {
std::filesystem::path test_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / std::filesystem::path(L"AudioAlgo 工程保存") /
                           ("case-" + std::to_string(stamp));
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

#if defined(_WIN32)
struct NoReplaceShareLock {
    explicit NoReplaceShareLock(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~NoReplaceShareLock() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    NoReplaceShareLock(const NoReplaceShareLock&) = delete;
    NoReplaceShareLock& operator=(const NoReplaceShareLock&) = delete;

    HANDLE handle = INVALID_HANDLE_VALUE;
};
#endif

ProjectModel sample_project() {
    ProjectModel project;
    project.project_id = "project-contract-1";
    project.name = "Gain demo 中文";
    project.revision = 7;
    project.nodes = {
        {"source-1", "builtin.file_source", {{"path", std::string("音频 samples.wav")}}, 15.5, 20.0},
        {"gain-1", "builtin.gain", {{"gain_db", -6.0}}, 240.0, 20.0},
        {"output-1", "builtin.output", {}, 480.0, 20.0},
    };
    project.connections = {
        {"source-1", "audio", "gain-1", "audio_in"},
        {"gain-1", "audio_out", "output-1", "audio"},
    };
    project.selected_output_node_id = "output-1";
    return project;
}
} // namespace

TEST_CASE("PRJ-01 saving and reopening preserves graph, parameters, resources and layout", "[G1][PRJ-01]") {
    const auto directory = test_directory();
    const auto path = directory / "project.json";
    auto expected = sample_project();

    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(path, expected)));
    const auto loaded_result = load_project_file(path);
    const auto* loaded = std::get_if<ProjectModel>(&loaded_result);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->schema_version == expected.schema_version);
    REQUIRE(loaded->project_id == expected.project_id);
    REQUIRE(loaded->name == expected.name);
    REQUIRE(loaded->revision == expected.revision);
    REQUIRE(loaded->selected_output_node_id == expected.selected_output_node_id);
    REQUIRE(loaded->nodes.size() == expected.nodes.size());
    REQUIRE(loaded->connections.size() == expected.connections.size());
    for (std::size_t index = 0; index < expected.nodes.size(); ++index) {
        REQUIRE(loaded->nodes[index].id == expected.nodes[index].id);
        REQUIRE(loaded->nodes[index].type_id == expected.nodes[index].type_id);
        REQUIRE(loaded->nodes[index].parameters == expected.nodes[index].parameters);
        REQUIRE(loaded->nodes[index].x == expected.nodes[index].x);
        REQUIRE(loaded->nodes[index].y == expected.nodes[index].y);
    }
    REQUIRE(loaded->connections[0].from_node == expected.connections[0].from_node);
    REQUIRE(loaded->connections[0].from_port == expected.connections[0].from_port);
    REQUIRE(loaded->connections[1].to_node == expected.connections[1].to_node);
    REQUIRE(loaded->connections[1].to_port == expected.connections[1].to_port);

    std::get<double>(expected.nodes[1].parameters.at("gain_db")) = 3.0;
    ++expected.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(path, expected)));
    const auto replaced_result = load_project_file(path);
    const auto* replaced = std::get_if<ProjectModel>(&replaced_result);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->revision == 8);
    REQUIRE(std::get<double>(replaced->nodes[1].parameters.at("gain_db")) == 3.0);
    std::filesystem::remove_all(directory);
}

TEST_CASE("PRJ-01 invalid project JSON returns a diagnostic", "[G1][PRJ-01]") {
    const auto directory = test_directory();
    const auto path = directory / "invalid.json";
    std::ofstream(path, std::ios::binary) << "{invalid";
    const auto result = load_project_file(path);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
    std::filesystem::remove_all(directory);
}

#if defined(_WIN32)
TEST_CASE("NED-13.d", "[node_interaction][NED-13]") {
    const auto directory = test_directory();
    const auto path = directory / "existing-project.aaproj";
    const auto original_project = sample_project();
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(path, original_project)));
    const auto original_bytes = read_file_bytes(path);
    REQUIRE_FALSE(original_bytes.empty());

    {
        NoReplaceShareLock locked_target(path);
        REQUIRE(locked_target.handle != INVALID_HANDLE_VALUE);
        auto changed_project = original_project;
        changed_project.name = "Changed project must not replace locked target";
        ++changed_project.revision;

        const auto failed_save = save_project_file(path, changed_project);
        const auto* diagnostic = std::get_if<audioalgo::Diagnostic>(&failed_save);
        REQUIRE(diagnostic != nullptr);
        REQUIRE(diagnostic->code == "project_save_failed");
        REQUIRE(read_file_bytes(path) == original_bytes);
        const auto reopened = load_project_file(path);
        REQUIRE(std::holds_alternative<ProjectModel>(reopened));
        REQUIRE(std::get<ProjectModel>(reopened).name == original_project.name);
        REQUIRE(std::get<ProjectModel>(reopened).revision == original_project.revision);
    }
    std::filesystem::remove_all(directory);
}
#endif

TEST_CASE("NED-01.a", "[node_interaction][NED-01]") {
    const auto directory = test_directory();
    const auto path = directory / std::filesystem::u8path("工程 名称 · − … .aaproj");
    auto project = sample_project();
    project.name = "夜曲 · −6 dB …";

    const std::string display_name = "增益节点 · −6 dB …";
    REQUIRE(audioalgo::editor::to_utf8(audioalgo::editor::utf8_string(display_name)) == display_name);
    REQUIRE(audioalgo::editor::to_utf8(audioalgo::editor::utf8_literal("Gain (−60 to +12 dB)")) ==
            "Gain (−60 to +12 dB)");
    REQUIRE(audioalgo::editor::to_utf8(audioalgo::editor::utf8_literal("Rendered waveform · overview…")) ==
            "Rendered waveform · overview…");
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(path, project)));

    const auto loaded = load_project_file(path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    REQUIRE(std::get<ProjectModel>(loaded).name == "夜曲 · −6 dB …");
    REQUIRE(std::filesystem::exists(path));
    std::filesystem::remove_all(directory);
}

TEST_CASE("NED-01.c", "[node_interaction][NED-01]") {
    std::string display_name;
    std::string summary;
    for (int index = 0; index < 24; ++index) {
        display_name += "多通道音频处理节点 · ";
        summary += "输入文件：现场录音 take-" + std::to_string(index) + " −6 dB … ";
    }
    const std::string hovered_port = "control.sidechain.input_with_a_long_name";
    const auto expected = display_name + "\n" + summary + "\n" + hovered_port;
    const auto tooltip = audioalgo::editor::build_node_tooltip_text(display_name, summary, hovered_port);

    REQUIRE(tooltip.size() > 512);
    REQUIRE(tooltip == expected);
    REQUIRE(tooltip.find("\xef\xbf\xbd") == std::string::npos);
    REQUIRE(audioalgo::editor::to_utf8(audioalgo::editor::utf8_string(tooltip)) == expected);
}
