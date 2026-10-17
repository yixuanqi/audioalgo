#include "audio_project/ProjectModel.h"
#include "audio_project/ProjectFileIO.h"
#include "../../apps/editor/EditorNodeDragDrop.h"
#include "../../apps/editor/EditorNodeCreation.h"
#include "../../apps/editor/EditorTopologyHistory.h"
#include "../../apps/editor/EditorTopologyCommands.h"
#include "../../apps/editor/EditorKeyboardShortcuts.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace {
audioalgo::ProjectModel disconnected_file_gain_output() {
    audioalgo::ProjectModel project;
    project.project_id = "edit-incomplete";
    project.name = "Incomplete graph remains editable";
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("signal.wav")}}, 10.0, 10.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 10.0},
        {"out", "builtin.output", {}, 470.0, 10.0},
    };
    project.selected_output_node_id = "out";
    return project;
}

bool has_input_required_for(const std::vector<audioalgo::GraphDiagnostic>& diagnostics, const std::string& node_id) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == "input_required" && diagnostic.node_id == node_id;
    });
}

void require_diagnostic(const std::vector<audioalgo::GraphDiagnostic>& diagnostics, const std::string& code,
                        const std::string& node_id, const std::string& port_id) {
    const auto found = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code && diagnostic.node_id == node_id && diagnostic.port_id == port_id;
    });
    REQUIRE(found != diagnostics.end());
}

bool same_project_state(const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
    if (left.schema_version != right.schema_version || left.project_id != right.project_id || left.name != right.name ||
        left.revision != right.revision || left.selected_output_node_id != right.selected_output_node_id ||
        left.nodes.size() != right.nodes.size() || left.connections.size() != right.connections.size())
        return false;
    for (std::size_t index = 0; index < left.nodes.size(); ++index) {
        const auto& a = left.nodes[index];
        const auto& b = right.nodes[index];
        if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters || a.x != b.x || a.y != b.y)
            return false;
    }
    for (std::size_t index = 0; index < left.connections.size(); ++index) {
        const auto& a = left.connections[index];
        const auto& b = right.connections[index];
        if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node || a.to_port != b.to_port)
            return false;
    }
    return true;
}

std::filesystem::path temporary_project_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("audioalgo-ned03-" + std::to_string(stamp));
    std::filesystem::create_directories(directory);
    return directory;
}
} // namespace

TEST_CASE("NED-03.a", "[node_interaction][NED-03]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();

    const auto edit_diagnostics = audioalgo::try_add_connection(
        project, {"file", "audio", "gain", "audio_in"}, registry);
    REQUIRE(edit_diagnostics.empty());
    REQUIRE(project.connections.size() == 1);
    REQUIRE(project.connections.front().from_node == "file");
    REQUIRE(project.connections.front().to_node == "gain");

    const auto run_diagnostics = audioalgo::validate_project(project, registry);
    REQUIRE(has_input_required_for(run_diagnostics, "out"));

    const auto final_edge_diagnostics = audioalgo::try_add_connection(
        project, {"gain", "audio_out", "out", "audio"}, registry);
    REQUIRE(final_edge_diagnostics.empty());
    REQUIRE(project.connections.size() == 2);
    REQUIRE(audioalgo::validate_project(project, registry).empty());
}

TEST_CASE("NED-03.b", "[node_interaction][NED-03]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();

    const auto first_edge_diagnostics = audioalgo::try_add_connection(
        project, {"gain", "audio_out", "out", "audio"}, registry);
    REQUIRE(first_edge_diagnostics.empty());
    REQUIRE(project.connections.size() == 1);
    REQUIRE(has_input_required_for(audioalgo::validate_project(project, registry), "gain"));

    const auto second_edge_diagnostics = audioalgo::try_add_connection(
        project, {"file", "audio", "gain", "audio_in"}, registry);
    REQUIRE(second_edge_diagnostics.empty());
    REQUIRE(audioalgo::validate_project(project, registry).empty());
}

TEST_CASE("NED-03.c", "[node_interaction][NED-03]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.push_back({"unused-gain-1", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 180.0});
    project.nodes.push_back({"unused-gain-2", "builtin.gain", {{"gain_db", 0.0}}, 470.0, 180.0});

    const auto edit_diagnostics = audioalgo::try_add_connection(
        project, {"file", "audio", "gain", "audio_in"}, registry);
    REQUIRE(edit_diagnostics.empty());
    REQUIRE(project.connections.size() == 1);
    REQUIRE(project.connections.front().to_node == "gain");
    REQUIRE(has_input_required_for(audioalgo::validate_project(project, registry), "out"));
    REQUIRE(has_input_required_for(audioalgo::validate_project(project, registry), "unused-gain-1"));
    REQUIRE(has_input_required_for(audioalgo::validate_project(project, registry), "unused-gain-2"));
}

TEST_CASE("NED-03.d", "[node_interaction][NED-03]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 23;

    REQUIRE(audioalgo::try_add_connection(project, {"file", "audio", "gain", "audio_in"}, registry).empty());
    REQUIRE(project.revision == 24);
    REQUIRE(audioalgo::try_add_connection(project, {"gain", "audio_out", "out", "audio"}, registry).empty());
    REQUIRE(project.revision == 25);
}

TEST_CASE("NED-03.e", "[node_interaction][NED-03]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    const auto directory = temporary_project_directory();
    const auto path = directory / "incomplete.aaproj";
    auto project = disconnected_file_gain_output();
    project.revision = 9;

    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(path, project)));
    const auto loaded_result = audioalgo::load_project_file(path);
    const auto* loaded = std::get_if<audioalgo::ProjectModel>(&loaded_result);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->connections.empty());
    REQUIRE(loaded->revision == 9);

    auto reopened = *loaded;
    const auto edit_diagnostics = audioalgo::try_add_connection(
        reopened, {"file", "audio", "gain", "audio_in"}, registry);
    REQUIRE(edit_diagnostics.empty());
    REQUIRE(reopened.connections.size() == 1);
    REQUIRE(has_input_required_for(audioalgo::validate_project(reopened, registry), "out"));
    std::filesystem::remove_all(directory);
}

TEST_CASE("NED-04.a", "[node_interaction][NED-04]") {
    SECTION("missing endpoints and ports identify the requested endpoint") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        auto project = disconnected_file_gain_output();
        require_diagnostic(audioalgo::try_add_connection(project, {"ghost", "audio", "gain", "audio_in"}, registry),
                           "connection_source_missing", "ghost", "audio");
        require_diagnostic(audioalgo::try_add_connection(project, {"file", "audio", "ghost", "audio_in"}, registry),
                           "connection_target_missing", "ghost", "audio_in");
        require_diagnostic(audioalgo::try_add_connection(project, {"file", "missing", "gain", "audio_in"}, registry),
                           "connection_source_port_missing", "file", "missing");
        require_diagnostic(audioalgo::try_add_connection(project, {"gain", "audio_in", "out", "audio"}, registry),
                           "connection_source_port_missing", "gain", "audio_in");
    }

    SECTION("type mismatch and self-connections are rejected") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        REQUIRE(registry.register_descriptor({
            "test.control_source", "1.0.0", "Control source",
            {{"control", "control.value@1", audioalgo::PortDirection::output, false}}, {},
            audioalgo::NodeExecutionKind::source}));
        auto project = disconnected_file_gain_output();
        project.nodes.push_back({"control", "test.control_source", {}, 0.0, 180.0});
        require_diagnostic(audioalgo::try_add_connection(project, {"control", "control", "gain", "audio_in"}, registry),
                           "connection_type_mismatch", "gain", "audio_in");
        require_diagnostic(audioalgo::try_add_connection(project, {"gain", "audio_out", "gain", "audio_in"}, registry),
                           "connection_self", "gain", "audio_in");
    }

    SECTION("cycles, duplicate edges and duplicate inputs are rejected") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        audioalgo::ProjectModel cyclic;
        cyclic.nodes = {
            {"gain-a", "builtin.gain", {{"gain_db", 0.0}}, 0.0, 0.0},
            {"gain-b", "builtin.gain", {{"gain_db", 0.0}}, 200.0, 0.0},
        };
        REQUIRE(audioalgo::try_add_connection(cyclic, {"gain-a", "audio_out", "gain-b", "audio_in"}, registry).empty());
        require_diagnostic(audioalgo::try_add_connection(cyclic, {"gain-b", "audio_out", "gain-a", "audio_in"}, registry),
                           "graph_cycle", {}, {});

        auto project = disconnected_file_gain_output();
        const audioalgo::Connection first{"file", "audio", "gain", "audio_in"};
        REQUIRE(audioalgo::try_add_connection(project, first, registry).empty());
        require_diagnostic(audioalgo::try_add_connection(project, first, registry),
                           "connection_duplicate", "gain", "audio_in");
        project.nodes.push_back({"file-2", "builtin.file_source", {{"path", std::string("other.wav")}}, 0.0, 180.0});
        require_diagnostic(audioalgo::try_add_connection(project, {"file-2", "audio", "gain", "audio_in"}, registry),
                           "input_connection_duplicate", "gain", "audio_in");
    }
}

TEST_CASE("NED-04.b", "[node_interaction][NED-04]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 52;
    project.name = "Atomic rejection";
    project.nodes.push_back({"file-2", "builtin.file_source", {{"path", std::string("preserved.wav")}}, 73.5, 211.25});
    REQUIRE(audioalgo::try_add_connection(project, {"file", "audio", "gain", "audio_in"}, registry).empty());

    const auto before = project;
    const auto diagnostics = audioalgo::try_add_connection(
        project, {"file-2", "audio", "gain", "audio_in"}, registry);

    require_diagnostic(diagnostics, "input_connection_duplicate", "gain", "audio_in");
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-09.a", "[node_interaction][NED-09]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    audioalgo::ProjectModel project;
    project.project_id = "delete-output-project";
    project.revision = 8;
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 30.0},
        {"gain", "builtin.gain", {{"gain_db", 3.0}}, 220.0, 30.0},
        {"output", "builtin.output", {}, 430.0, 30.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";

    REQUIRE(audioalgo::editor::delete_node(project, registry, "output"));
    REQUIRE(project.nodes.size() == 2);
    REQUIRE(project.connections.size() == 1);
    REQUIRE(project.connections.front().from_node == "file");
    REQUIRE(project.connections.front().to_node == "gain");
    REQUIRE(project.selected_output_node_id.empty());
    REQUIRE(project.revision == 9);

    const auto after_delete = project;
    REQUIRE_FALSE(audioalgo::editor::delete_node(project, registry, "missing"));
    REQUIRE(same_project_state(project, after_delete));
}

TEST_CASE("NED-09.e", "[node_interaction][NED-09]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.insert(project.nodes.begin() + 2,
                         {"gain-b", "builtin.gain", {{"gain_db", -6.0}}, 350.0, 30.0});
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "gain-b", "audio_in"},
        {"gain-b", "audio_out", "out", "audio"},
    };
    project.revision = 18;
    const auto before = project;
    audioalgo::editor::EditorTopologyHistory history;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::editor::delete_node(candidate, registry, "gain");
    }));
    REQUIRE(project.nodes.size() == 3);
    REQUIRE(project.connections.size() == 2);
    REQUIRE(project.connections[0].from_node == "file");
    REQUIRE(project.connections[0].to_node == "gain-b");
    REQUIRE(project.connections[0].to_port == "audio_in");
    REQUIRE(project.connections[1].from_node == "gain-b");
    REQUIRE(project.connections[1].to_node == "out");
    REQUIRE(project.selected_output_node_id == "out");
    REQUIRE(project.revision == 19);

    REQUIRE(history.undo(project));
    auto expected_undo = before;
    expected_undo.revision = 20;
    REQUIRE(same_project_state(project, expected_undo));
    REQUIRE(history.redo(project));
    REQUIRE(project.connections.size() == 2);
    REQUIRE(project.connections.front().from_node == "file");
    REQUIRE(project.connections.front().to_node == "gain-b");
    REQUIRE(project.revision == 21);

    SECTION("A branch through the removed node is not collapsed into one edge") {
        auto branched = disconnected_file_gain_output();
        branched.nodes.insert(branched.nodes.begin() + 2,
                              {"gain-b", "builtin.gain", {{"gain_db", -6.0}}, 350.0, 30.0});
        branched.connections = {
            {"file", "audio", "gain", "audio_in"},
            {"gain", "audio_out", "out", "audio"},
            {"gain", "audio_out", "gain-b", "audio_in"},
        };
        REQUIRE(audioalgo::editor::delete_node(branched, registry, "gain"));
        REQUIRE(branched.connections.empty());
    }

    SECTION("A type-changing processor is deleted without an incompatible bypass") {
        REQUIRE(registry.register_descriptor({
            "test.audio_to_midi", "1", "Audio to MIDI",
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"midi_out", "midi", audioalgo::PortDirection::output, true}},
            {}, audioalgo::NodeExecutionKind::block}));
        REQUIRE(registry.register_descriptor({
            "test.midi_output", "1", "MIDI output",
            {{"midi", "midi", audioalgo::PortDirection::input, true}},
            {}, audioalgo::NodeExecutionKind::sink}));
        audioalgo::ProjectModel converted;
        converted.project_id = "delete-type-changing-node";
        converted.nodes = {
            {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 30.0},
            {"convert", "test.audio_to_midi", {}, 220.0, 30.0},
            {"midi-output", "test.midi_output", {}, 430.0, 30.0},
        };
        converted.connections = {
            {"file", "audio", "convert", "audio_in"},
            {"convert", "midi_out", "midi-output", "midi"},
        };
        converted.selected_output_node_id = "midi-output";
        const auto diagnostics = audioalgo::validate_project_structure(converted, registry);
        for (const auto& diagnostic : diagnostics)
            INFO(diagnostic.code << ": " << diagnostic.message);
        REQUIRE(diagnostics.empty());
        REQUIRE(audioalgo::editor::delete_node(converted, registry, "convert"));
        REQUIRE(converted.nodes.size() == 2);
        REQUIRE(converted.connections.empty());
    }
}

TEST_CASE("NED-11.g", "[node_interaction][NED-11]") {
    using audioalgo::editor::EditorShortcutAction;
    using audioalgo::editor::editor_shortcut_action;
    using juce::KeyPress;
    using juce::ModifierKeys;

    REQUIRE(editor_shortcut_action(KeyPress('z', ModifierKeys(ModifierKeys::ctrlModifier), 'z')) ==
            EditorShortcutAction::undo);
    REQUIRE(editor_shortcut_action(KeyPress('Z', ModifierKeys(ModifierKeys::ctrlModifier |
                                                              ModifierKeys::shiftModifier), 'Z')) ==
            EditorShortcutAction::redo);
    REQUIRE(editor_shortcut_action(KeyPress('y', ModifierKeys(ModifierKeys::ctrlModifier), 'y')) ==
            EditorShortcutAction::redo);
    REQUIRE(editor_shortcut_action(KeyPress('z', ModifierKeys{}, 'z')) == EditorShortcutAction::none);
    REQUIRE(editor_shortcut_action(KeyPress('z', ModifierKeys(ModifierKeys::ctrlModifier |
                                                              ModifierKeys::altModifier), 'z')) ==
            EditorShortcutAction::none);
}

TEST_CASE("NED-09.c", "[node_interaction][NED-09]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 17;
    const auto before = project;

    REQUIRE_FALSE(audioalgo::editor::can_add_node_of_execution_kind(
        project, registry, audioalgo::NodeExecutionKind::source));
    REQUIRE_FALSE(audioalgo::editor::can_add_node_of_execution_kind(
        project, registry, audioalgo::NodeExecutionKind::sink));
    REQUIRE_FALSE(audioalgo::editor::try_add_node_from_descriptor(
        project, registry, "builtin.output", "second-output", 700.0, 40.0));
    REQUIRE(same_project_state(project, before));

    auto without_source = project;
    std::erase_if(without_source.nodes, [](const auto& node) { return node.type_id == "builtin.file_source"; });
    REQUIRE(audioalgo::editor::can_add_node_of_execution_kind(
        without_source, registry, audioalgo::NodeExecutionKind::source));
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-07.a", "[node_interaction][NED-07]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 31;
    const auto original_count = project.nodes.size();

    const auto dropped = audioalgo::editor::try_drop_library_node(
        project, registry, "builtin.gain", "library-gain-1", {770.0f, 340.0f},
        1000.0f, 600.0f);

    REQUIRE(dropped.has_value());
    REQUIRE(dropped->node_id == "library-gain-1");
    REQUIRE(dropped->position.x == 682.0f);
    REQUIRE(dropped->position.y == 290.0f);
    REQUIRE(project.nodes.size() == original_count + 1);
    REQUIRE(project.nodes.back().id == "library-gain-1");
    REQUIRE(project.nodes.back().x == dropped->position.x);
    REQUIRE(project.nodes.back().y == dropped->position.y);
    REQUIRE(project.revision == 32);
}

TEST_CASE("NED-07.b", "[node_interaction][NED-07]") {
    audioalgo::editor::LibraryNodeDragGesture gesture;
    gesture.begin({40.0f, 20.0f});

    REQUIRE_FALSE(gesture.move({46.0f, 20.0f}));
    REQUIRE(gesture.finish(true) == audioalgo::editor::LibraryNodeDragGesture::Release::click);
    REQUIRE(gesture.finish(true) == audioalgo::editor::LibraryNodeDragGesture::Release::none);
}

TEST_CASE("NED-07.c", "[node_interaction][NED-07]") {
    audioalgo::editor::LibraryNodeDragGesture gesture;
    gesture.begin({40.0f, 20.0f});

    REQUIRE(gesture.move({46.01f, 20.0f}));
    REQUIRE_FALSE(gesture.move({80.0f, 55.0f}));
    REQUIRE(gesture.finish(true) == audioalgo::editor::LibraryNodeDragGesture::Release::drag);
    REQUIRE(gesture.finish(true) == audioalgo::editor::LibraryNodeDragGesture::Release::none);
}

TEST_CASE("NED-07.d", "[node_interaction][NED-07]") {
    auto project = disconnected_file_gain_output();
    const auto before = project;
    audioalgo::editor::LibraryDropSession session;

    REQUIRE(session.begin("builtin.gain", 12, project.revision));
    REQUIRE(session.update({200.0f, 180.0f}, audioalgo::editor::EditorPoint{112.0f, 130.0f}));
    REQUIRE(session.can_deliver_drop(12, project.revision));
    session.leave_canvas();
    REQUIRE_FALSE(session.can_deliver_drop(12, project.revision));
    REQUIRE_FALSE(session.preview_position().has_value());
    session.finish_drag(12);
    REQUIRE_FALSE(session.is_active());
    REQUIRE(same_project_state(project, before));

    REQUIRE(session.begin("builtin.gain", 12, project.revision));
    REQUIRE(session.update({240.0f, 200.0f}, audioalgo::editor::EditorPoint{152.0f, 150.0f}));
    REQUIRE_FALSE(session.can_deliver_drop(13, project.revision));
    session.cancel();
    REQUIRE_FALSE(session.is_active());
    REQUIRE_FALSE(session.preview_position().has_value());
    REQUIRE(same_project_state(project, before));

    REQUIRE(session.begin("builtin.gain", 12, project.revision));
    REQUIRE(session.update({280.0f, 220.0f}, audioalgo::editor::EditorPoint{192.0f, 170.0f}));
    session.cancel();
    REQUIRE_FALSE(session.can_deliver_drop(12, project.revision));
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-07.e", "[node_interaction][NED-07]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    std::erase_if(project.nodes, [](const auto& node) { return node.type_id == "builtin.file_source"; });
    project.revision = 45;
    const auto before = project;

    REQUIRE_FALSE(audioalgo::editor::try_add_source_node_from_drop(
        project, registry, "builtin.file_source", "file-cancelled", "", {770.0f, 340.0f},
        1000.0f, 600.0f, 5, 5));
    REQUIRE(same_project_state(project, before));
    REQUIRE_FALSE(audioalgo::editor::try_add_source_node_from_drop(
        project, registry, "builtin.file_source", "file-stale", "signal.wav", {770.0f, 340.0f},
        1000.0f, 600.0f, 5, 6));
    REQUIRE(same_project_state(project, before));

    const auto dropped = audioalgo::editor::try_add_source_node_from_drop(
        project, registry, "builtin.file_source", "file-dropped", "signal.wav", {770.0f, 340.0f},
        1000.0f, 600.0f, 5, 5);
    REQUIRE(dropped.has_value());
    REQUIRE(dropped->position.x == 682.0f);
    REQUIRE(dropped->position.y == 290.0f);
    REQUIRE(project.nodes.back().parameters.at("path") == audioalgo::ParameterValue(std::string("signal.wav")));
    REQUIRE(project.revision == 46);
}

TEST_CASE("NED-07.f", "[node_interaction][NED-07]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    const auto source_project = disconnected_file_gain_output();
    auto current_project = source_project;
    std::erase_if(current_project.nodes, [](const auto& node) { return node.type_id == "builtin.file_source"; });
    current_project.revision = 81;
    const auto current_before = current_project;

    // A resource chooser opened in the source project must not attach its result
    // to a different project, even when that project currently has source capacity.
    REQUIRE_FALSE(audioalgo::editor::try_add_source_node_from_drop(
        current_project, registry, "builtin.file_source", "stale-file-result", "signal.wav",
        {770.0f, 340.0f}, 1000.0f, 600.0f, 12, 13));
    REQUIRE(same_project_state(current_project, current_before));
    REQUIRE(std::any_of(source_project.nodes.begin(), source_project.nodes.end(), [](const auto& node) {
        return node.type_id == "builtin.file_source";
    }));
}

TEST_CASE("NED-08.a", "[node_interaction][NED-08]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto base = disconnected_file_gain_output();
    base.revision = 10;

    audioalgo::editor::PortConnectionGesture click_gesture;
    REQUIRE(click_gesture.begin("file", "audio"));
    const auto click_connection = click_gesture.finish("gain", "audio_in");
    REQUIRE(click_connection.has_value());

    audioalgo::editor::PortConnectionGesture drag_gesture;
    REQUIRE(drag_gesture.begin("file", "audio"));
    drag_gesture.mark_drag_started();
    REQUIRE(drag_gesture.is_dragging());
    const auto drag_connection = drag_gesture.finish("gain", "audio_in");
    REQUIRE(drag_connection.has_value());

    REQUIRE(click_connection->from_node == drag_connection->from_node);
    REQUIRE(click_connection->from_port == drag_connection->from_port);
    REQUIRE(click_connection->to_node == drag_connection->to_node);
    REQUIRE(click_connection->to_port == drag_connection->to_port);

    auto click_project = base;
    auto drag_project = base;
    REQUIRE(audioalgo::try_add_connection(click_project, *click_connection, registry).empty());
    REQUIRE(audioalgo::try_add_connection(drag_project, *drag_connection, registry).empty());
    REQUIRE(same_project_state(click_project, drag_project));
    REQUIRE(click_project.connections.size() == 1);
    REQUIRE(click_project.revision == 11);
}

TEST_CASE("NED-08.b", "[node_interaction][NED-08]") {
    audioalgo::editor::CanvasCoordinateTransform coordinates;
    audioalgo::editor::PortConnectionGesture gesture;
    const auto start = coordinates.event_to_canvas({44.0f, 58.0f});
    REQUIRE(gesture.begin("file", "audio", start));
    REQUIRE(gesture.pointer_position().x == start.x);
    REQUIRE(gesture.pointer_position().y == start.y);

    auto pointer = coordinates.event_to_canvas({186.0f, 132.0f});
    REQUIRE(gesture.update_pointer(pointer));
    REQUIRE(gesture.pointer_position().x == pointer.x);
    REQUIRE(gesture.pointer_position().y == pointer.y);
    gesture.mark_drag_started();
    pointer = coordinates.event_to_canvas({231.0f, 177.0f});
    REQUIRE(gesture.update_pointer(pointer));
    REQUIRE(gesture.is_dragging());
    REQUIRE(gesture.pointer_position().x == pointer.x);
    REQUIRE(gesture.pointer_position().y == pointer.y);
}

TEST_CASE("NED-08.c", "[node_interaction][NED-08]") {
    auto project = disconnected_file_gain_output();
    project.revision = 27;
    const auto before = project;
    audioalgo::editor::PortConnectionGesture gesture;

    REQUIRE(gesture.begin("file", "audio", {52.0f, 63.0f}));
    gesture.mark_drag_started();
    gesture.cancel(); // Invalid release has no target port and uses this cleanup path.
    REQUIRE_FALSE(gesture.is_active());
    REQUIRE(same_project_state(project, before));

    REQUIRE(gesture.begin("file", "audio", {52.0f, 63.0f}));
    gesture.mark_drag_started();
    REQUIRE_FALSE(gesture.cancel_on_escape(false));
    REQUIRE(gesture.is_dragging());
    REQUIRE(gesture.cancel_on_escape(true));
    REQUIRE_FALSE(gesture.is_active());
    REQUIRE(gesture.pointer_position().x == 0.0f);
    REQUIRE(gesture.pointer_position().y == 0.0f);
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-08.d", "[node_interaction][NED-08]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 74;
    project.nodes.push_back({"file-2", "builtin.file_source", {{"path", std::string("other.wav")}}, 12.0, 210.0});
    REQUIRE(audioalgo::try_add_connection(project, {"file", "audio", "gain", "audio_in"}, registry).empty());
    const auto before = project;

    audioalgo::editor::PortConnectionGesture gesture;
    REQUIRE(gesture.begin("file-2", "audio", {600.0f, 160.0f}));
    gesture.mark_drag_started();
    const auto attempted_connection = gesture.finish("gain", "audio_in");
    REQUIRE(attempted_connection.has_value());
    const auto diagnostics = audioalgo::try_add_connection(project, *attempted_connection, registry);
    require_diagnostic(diagnostics, "input_connection_duplicate", "gain", "audio_in");
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-08.e", "[node_interaction][NED-08]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 91;
    const audioalgo::Connection selected{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection remaining{"gain", "audio_out", "out", "audio"};
    project.connections = {selected, remaining};
    const auto before = project;

    REQUIRE(audioalgo::editor::try_disconnect_connection(project, selected));
    REQUIRE(project.connections.size() == 1);
    REQUIRE(project.connections.front().from_node == remaining.from_node);
    REQUIRE(project.connections.front().from_port == remaining.from_port);
    REQUIRE(project.connections.front().to_node == remaining.to_node);
    REQUIRE(project.connections.front().to_port == remaining.to_port);
    auto expected = before;
    expected.connections.erase(expected.connections.begin());
    expected.revision = 92;
    REQUIRE(same_project_state(project, expected));

    REQUIRE_FALSE(audioalgo::editor::try_disconnect_connection(project, selected));
    REQUIRE(same_project_state(project, expected));
    REQUIRE(audioalgo::try_add_connection(project, selected, registry).empty());
    REQUIRE(project.connections.size() == 2);
    REQUIRE(project.revision == 93);
}

TEST_CASE("NED-10.a", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 100;
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection other{"gain", "audio_out", "out", "audio"};
    project.connections = {target, other};
    const auto before = project;

    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "builtin.gain", "inserted-gain", target, 252.0, 145.0);
    REQUIRE(inserted.has_value());
    REQUIRE(inserted->node_id == "inserted-gain");
    REQUIRE(inserted->input_port_id == "audio_in");
    REQUIRE(inserted->output_port_id == "audio_out");
    REQUIRE(inserted->position.x == 252.0);
    REQUIRE(inserted->position.y == 145.0);

    auto expected = before;
    expected.nodes.push_back({"inserted-gain", "builtin.gain", {{"gain_db", 0.0}}, 252.0, 145.0});
    expected.connections.erase(expected.connections.begin());
    expected.connections.push_back({"file", "audio", "inserted-gain", "audio_in"});
    expected.connections.push_back({"inserted-gain", "audio_out", "gain", "audio_in"});
    expected.revision = 101;
    REQUIRE(same_project_state(project, expected));
    REQUIRE(project.nodes.size() == before.nodes.size() + 1);
    REQUIRE(project.connections.size() == before.connections.size() + 1);
    REQUIRE(project.revision == before.revision + 1);

    SECTION("library drop preview retains and clears the exact target edge") {
        audioalgo::editor::LibraryDropSession drop;
        REQUIRE(drop.begin("builtin.gain", 17, project.revision));
        REQUIRE(drop.update({320.0f, 88.0f}, audioalgo::editor::EditorPoint{232.0f, 38.0f}, target));
        REQUIRE(drop.target_connection().has_value());
        REQUIRE(drop.target_connection()->from_node == target.from_node);
        REQUIRE(drop.target_connection()->from_port == target.from_port);
        REQUIRE(drop.target_connection()->to_node == target.to_node);
        REQUIRE(drop.target_connection()->to_port == target.to_port);
        const auto* gain_descriptor = registry.find("builtin.gain");
        REQUIRE(gain_descriptor != nullptr);
        const auto preview_origin = audioalgo::editor::library_node_origin_for_centered_drop(
            *gain_descriptor, {320.0f, 88.0f});
        REQUIRE(preview_origin.x == 232.0f);
        REQUIRE(preview_origin.y == 38.0f);
        drop.leave_canvas();
        REQUIRE_FALSE(drop.target_connection().has_value());
    }

    SECTION("target delivery is independent of source mouse-up callback ordering") {
        audioalgo::editor::LibraryNodeDragGesture source_gesture;
        source_gesture.begin({10.0f, 10.0f});
        REQUIRE(source_gesture.move({20.0f, 10.0f}));

        audioalgo::editor::LibraryDropSession drop;
        REQUIRE(drop.begin("builtin.gain", 17, project.revision));
        REQUIRE(drop.update({320.0f, 88.0f}, audioalgo::editor::EditorPoint{232.0f, 38.0f}, target));

        // JUCE's drag image calls itemDropped from its own mouseUp. Delivery must
        // not depend on whether the original button also receives mouseUp first.
        REQUIRE(source_gesture.finish(true) == audioalgo::editor::LibraryNodeDragGesture::Release::drag);
        REQUIRE(drop.is_active());
        REQUIRE(drop.can_deliver_drop(17, project.revision));
        REQUIRE(drop.target_connection().has_value());
        REQUIRE(drop.target_connection()->from_node == target.from_node);
        REQUIRE(drop.target_connection()->from_port == target.from_port);
        REQUIRE(drop.target_connection()->to_node == target.to_node);
        REQUIRE(drop.target_connection()->to_port == target.to_port);

        // itemDropped consumes the target; dragOperationEnded then performs cleanup.
        drop.cancel();
        drop.finish_drag(17);
        REQUIRE_FALSE(drop.is_active());
        REQUIRE_FALSE(drop.target_connection().has_value());
    }

}

TEST_CASE("NED-10.b", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.push_back({"branch-gain", "builtin.gain", {{"gain_db", 0.0}}, 470.0, 180.0});
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection output_branch{"gain", "audio_out", "out", "audio"};
    const audioalgo::Connection side_branch{"gain", "audio_out", "branch-gain", "audio_in"};
    project.connections = {target, output_branch, side_branch};
    project.revision = 202;
    const auto before = project;

    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "builtin.gain", "inserted-gain", target, 252.0, 145.0);
    REQUIRE(inserted.has_value());

    auto expected = before;
    expected.nodes.push_back({"inserted-gain", "builtin.gain", {{"gain_db", 0.0}}, 252.0, 145.0});
    expected.connections.erase(expected.connections.begin());
    expected.connections.push_back({"file", "audio", "inserted-gain", "audio_in"});
    expected.connections.push_back({"inserted-gain", "audio_out", "gain", "audio_in"});
    expected.revision = 203;
    REQUIRE(same_project_state(project, expected));
    REQUIRE(project.connections.size() == before.connections.size() + 1);
    REQUIRE(project.connections[0].from_node == output_branch.from_node);
    REQUIRE(project.connections[0].to_node == output_branch.to_node);
    REQUIRE(project.connections[1].from_node == side_branch.from_node);
    REQUIRE(project.connections[1].to_node == side_branch.to_node);
    REQUIRE(project.revision == before.revision + 1);
}

TEST_CASE("NED-10.c", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.push_back({"existing-gain", "builtin.gain", {{"gain_db", -6.0}}, 190.0, 24.0});
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection retained{"gain", "audio_out", "out", "audio"};
    project.connections = {target, retained};
    project.revision = 300;
    const auto before = project;

    REQUIRE_FALSE(audioalgo::editor::find_visible_connection_at(
        project, registry, {213.0f, 74.0f}, 8.0f));
    const auto connection_under_dragged_node = audioalgo::editor::find_visible_connection_at(
        project, registry, {213.0f, 74.0f}, 8.0f, audioalgo::editor::CanvasCoordinateTransform{}, "existing-gain");
    REQUIRE(connection_under_dragged_node.has_value());
    REQUIRE(*connection_under_dragged_node == 0);

    const auto inserted = audioalgo::editor::try_insert_existing_node_on_connection(
        project, registry, "existing-gain", target, 348.0, 142.0);
    REQUIRE(inserted.has_value());

    auto expected = before;
    auto& existing = expected.nodes.back();
    existing.x = 348.0;
    existing.y = 142.0;
    expected.connections.erase(expected.connections.begin());
    expected.connections.push_back({"file", "audio", "existing-gain", "audio_in"});
    expected.connections.push_back({"existing-gain", "audio_out", "gain", "audio_in"});
    expected.revision = 301;
    REQUIRE(inserted->node_id == "existing-gain");
    REQUIRE(inserted->input_port_id == "audio_in");
    REQUIRE(inserted->output_port_id == "audio_out");
    REQUIRE(inserted->position.x == 348.0f);
    REQUIRE(inserted->position.y == 142.0f);
    REQUIRE(same_project_state(project, expected));
    REQUIRE(project.nodes.size() == before.nodes.size());
    REQUIRE(std::count_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "existing-gain";
    }) == 1);
    REQUIRE(project.revision == before.revision + 1);
}

TEST_CASE("NED-10.d", "[node_interaction][NED-10]") {
    SECTION("connected existing nodes are rejected without mutation") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        auto project = disconnected_file_gain_output();
        project.nodes.push_back({"connected-gain", "builtin.gain", {{"gain_db", -3.0}}, 520.0, 180.0});
        const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
        project.connections = {target, {"gain", "audio_out", "connected-gain", "audio_in"}};
        const auto before = project;

        REQUIRE_FALSE(audioalgo::editor::try_insert_existing_node_on_connection(
            project, registry, "connected-gain", target, 340.0, 140.0));
        REQUIRE(same_project_state(project, before));
    }

    SECTION("Source and Sink library entries are rejected on a connection") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        auto project = disconnected_file_gain_output();
        const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
        const auto before = project;
        const auto* source = registry.find("builtin.file_source");
        const auto* sink = registry.find("builtin.output");
        REQUIRE(source != nullptr);
        REQUIRE(sink != nullptr);
        REQUIRE(audioalgo::editor::decide_library_drop_route(source, true) ==
                audioalgo::editor::LibraryDropRoute::reject);
        REQUIRE(audioalgo::editor::decide_library_drop_route(sink, true) ==
                audioalgo::editor::LibraryDropRoute::reject);
        REQUIRE(audioalgo::editor::decide_library_drop_route(source, false) ==
                audioalgo::editor::LibraryDropRoute::source_chooser);
        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, source->type_id, "bad-source", target, 340.0, 140.0));
        REQUIRE(same_project_state(project, before));
        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, sink->type_id, "bad-sink", target, 340.0, 140.0));
        REQUIRE(same_project_state(project, before));
    }

    SECTION("a block node with incompatible port types is rejected atomically") {
        audioalgo::NodeRegistry registry;
        audioalgo::register_builtin_node_descriptors(registry);
        REQUIRE(registry.register_descriptor({
            "test.control_block", "1.0.0", "Control block",
            {{"control_in", "control.value@1", audioalgo::PortDirection::input, true},
             {"control_out", "control.value@1", audioalgo::PortDirection::output, false}}, {},
            audioalgo::NodeExecutionKind::block}));
        auto project = disconnected_file_gain_output();
        const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
        const auto before = project;

        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, "test.control_block", "bad-control", target, 340.0, 140.0));
        REQUIRE(same_project_state(project, before));
    }
}

TEST_CASE("NED-10.f", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    REQUIRE(registry.register_descriptor({
        "test.multi_audio", "1.0.0", "Multi audio block",
        {{"left_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"right_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"left_out", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"right_out", "audio.pcm@1", audioalgo::PortDirection::output, false}}, {},
        audioalgo::NodeExecutionKind::block}));
    auto project = disconnected_file_gain_output();
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    project.connections = {target, {"gain", "audio_out", "out", "audio"}};
    project.revision = 400;
    const auto before = project;

    const auto choices = audioalgo::editor::find_library_insertion_port_choices(
        project, registry, "test.multi_audio", target);
    REQUIRE(choices.size() == 4);
    REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "test.multi_audio", "multi", target, 340.0, 140.0));
    REQUIRE(same_project_state(project, before));
    REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "test.multi_audio", "multi", target, 340.0, 140.0,
        audioalgo::editor::InsertionPortChoice{"missing_in", "left_out"}));
    REQUIRE(same_project_state(project, before));

    const audioalgo::editor::InsertionPortChoice chosen{"right_in", "left_out"};
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "test.multi_audio", "multi", target, 340.0, 140.0, chosen);
    REQUIRE(inserted.has_value());
    REQUIRE(inserted->input_port_id == chosen.input_port_id);
    REQUIRE(inserted->output_port_id == chosen.output_port_id);
    REQUIRE(project.nodes.size() == before.nodes.size() + 1);
    REQUIRE(project.connections[1].to_node == "multi");
    REQUIRE(project.connections[1].to_port == "right_in");
    REQUIRE(project.connections.back().from_port == "left_out");
    REQUIRE(project.revision == before.revision + 1);
}

TEST_CASE("NED-10.h", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};

    SECTION("a project revision change invalidates a library-drop target snapshot") {
        auto project = disconnected_file_gain_output();
        project.connections = {target, {"gain", "audio_out", "out", "audio"}};
        project.revision = 500;
        audioalgo::editor::LibraryDropSession drop;

        REQUIRE(drop.begin("builtin.gain", 17, project.revision));
        REQUIRE(drop.update({320.0f, 88.0f}, audioalgo::editor::EditorPoint{232.0f, 38.0f}, target));
        REQUIRE(drop.can_deliver_drop(17, project.revision));
        REQUIRE_FALSE(drop.can_deliver_drop(17, project.revision + 1));
        REQUIRE_FALSE(drop.can_deliver_drop(18, project.revision));

        project.connections.erase(project.connections.begin());
        ++project.revision;
        const auto before_stale_commit = project;
        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, "builtin.gain", "stale-target-gain", target, 252.0, 145.0));
        REQUIRE(same_project_state(project, before_stale_commit));
    }

    SECTION("duplicate target edges reject without changing the invalid graph") {
        auto project = disconnected_file_gain_output();
        project.connections = {target, target, {"gain", "audio_out", "out", "audio"}};
        const auto before = project;

        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, "builtin.gain", "duplicate-target-gain", target, 252.0, 145.0));
        REQUIRE(same_project_state(project, before));
    }

    SECTION("candidate validation failure leaves the original project byte-for-byte equivalent") {
        auto project = disconnected_file_gain_output();
        project.connections = {target, {"gain", "audio_out", "out", "audio"}};
        std::get<double>(project.nodes[1].parameters.at("gain_db")) = 100.0;
        const auto before = project;
        const auto diagnostics = audioalgo::validate_project_structure(project, registry);
        REQUIRE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "parameter_out_of_range" && diagnostic.node_id == "gain" &&
                   diagnostic.port_id == "gain_db";
        }));

        REQUIRE_FALSE(audioalgo::editor::try_insert_library_node_on_connection(
            project, registry, "builtin.gain", "validation-failure-gain", target, 252.0, 145.0));
        REQUIRE(same_project_state(project, before));
    }
}

TEST_CASE("NED-11.a", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection output{"gain", "audio_out", "out", "audio"};

    SECTION("Add restores the same node identity and defaults") {
        auto project = disconnected_file_gain_output();
        project.revision = 700;
        const auto original_node_count = project.nodes.size();
        audioalgo::editor::EditorTopologyHistory history;

        REQUIRE(history.apply(project, [&](auto& candidate) {
            return audioalgo::editor::try_add_node_from_descriptor(
                candidate, registry, "builtin.gain", "history-gain", 700.0, 220.0);
        }));
        REQUIRE(project.nodes.size() == original_node_count + 1);
        REQUIRE(project.revision == 701);
        REQUIRE(history.can_undo());
        REQUIRE_FALSE(history.can_redo());

        REQUIRE(history.undo(project));
        REQUIRE(project.nodes.size() == original_node_count);
        REQUIRE(project.revision == 702);
        REQUIRE(std::none_of(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
            return node.id == "history-gain";
        }));
        REQUIRE(history.can_redo());

        REQUIRE(history.redo(project));
        REQUIRE(project.nodes.size() == original_node_count + 1);
        REQUIRE(project.revision == 703);
        const auto restored = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
            return node.id == "history-gain";
        });
        REQUIRE(restored != project.nodes.end());
        REQUIRE(restored->type_id == "builtin.gain");
        REQUIRE(restored->parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
        REQUIRE(restored->x == 700.0);
        REQUIRE(restored->y == 220.0);
    }

    SECTION("Connect and disconnect each form one reversible topology operation") {
        auto project = disconnected_file_gain_output();
        project.revision = 710;
        audioalgo::editor::EditorTopologyHistory history;

        REQUIRE(history.apply(project, [&](auto& candidate) {
            return audioalgo::try_add_connection(candidate, target, registry).empty();
        }));
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.revision == 711);
        REQUIRE(history.undo(project));
        REQUIRE(project.connections.empty());
        REQUIRE(project.revision == 712);
        REQUIRE(history.redo(project));
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.connections.front().to_node == "gain");
        REQUIRE(project.revision == 713);

        project.connections = {target, output};
        ++project.revision;
        audioalgo::editor::EditorTopologyHistory disconnect_history;
        REQUIRE(disconnect_history.apply(project, [&](auto& candidate) {
            return audioalgo::editor::try_disconnect_connection(candidate, target);
        }));
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.revision == 715);
        REQUIRE(disconnect_history.undo(project));
        REQUIRE(project.connections.size() == 2);
        REQUIRE(project.revision == 716);
        REQUIRE(disconnect_history.redo(project));
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.revision == 717);
    }

    SECTION("Insert is one reversible command") {
        auto project = disconnected_file_gain_output();
        project.connections = {target, output};
        project.revision = 730;
        audioalgo::editor::EditorTopologyHistory history;
        std::optional<audioalgo::editor::LibraryNodeInsertionResult> inserted;

        REQUIRE(history.apply(project, [&](auto& candidate) {
            inserted = audioalgo::editor::try_insert_library_node_on_connection(
                candidate, registry, "builtin.gain", "history-insert", target, 252.0, 145.0);
            return inserted.has_value();
        }));
        REQUIRE(history.undo_size() == 1);
        REQUIRE(project.nodes.size() == 4);
        REQUIRE(project.connections.size() == 3);
        REQUIRE(history.undo(project));
        REQUIRE(project.nodes.size() == 3);
        REQUIRE(project.connections.size() == 2);
        REQUIRE(project.connections[0].from_node == target.from_node);
        REQUIRE(project.connections[0].to_node == target.to_node);
        REQUIRE(project.revision == 732);
        REQUIRE(history.redo(project));
        REQUIRE(project.nodes.size() == 4);
        REQUIRE(project.connections.size() == 3);
        REQUIRE(project.revision == 733);
    }

    SECTION("Delete restores removed nodes, incident edges, and selected output") {
        auto project = disconnected_file_gain_output();
        project.connections = {target, output};
        project.revision = 740;
        const auto before = project;
        audioalgo::editor::EditorTopologyHistory history;

        REQUIRE(history.apply(project, [&](auto& candidate) {
            return audioalgo::editor::delete_node(candidate, registry, "gain");
        }));
        REQUIRE(project.nodes.size() == 2);
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.connections.front().from_node == "file");
        REQUIRE(project.connections.front().to_node == "out");
        REQUIRE(project.selected_output_node_id == "out");
        REQUIRE(project.revision == 741);
        REQUIRE(history.undo(project));
        auto expected_undo = before;
        expected_undo.revision = 742;
        REQUIRE(same_project_state(project, expected_undo));
        REQUIRE(history.redo(project));
        REQUIRE(project.nodes.size() == 2);
        REQUIRE(project.connections.size() == 1);
        REQUIRE(project.connections.front().from_node == "file");
        REQUIRE(project.connections.front().to_node == "out");
        REQUIRE(project.revision == 743);
    }
}

TEST_CASE("NI0010.a", "[node_interaction][NI0010]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes[1].x = 210.0;
    project.nodes[1].y = 90.0;
    project.nodes[1].parameters["gain_db"] = 3.0;
    project.nodes.push_back({"gain-b", "builtin.gain", {{"gain_db", -6.0}}, 430.0, 180.0});
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "gain-b", "audio_in"},
        {"gain-b", "audio_out", "out", "audio"},
    };
    project.revision = 90;
    const auto before = project;
    audioalgo::editor::EditorTopologyHistory history;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::editor::try_swap_adjacent_processing_nodes(
            candidate, registry, "gain", "gain-b");
    }));
    const auto has_connection = [&](const std::string& from, const std::string& from_port,
                                    const std::string& to, const std::string& to_port) {
        return std::any_of(project.connections.begin(), project.connections.end(), [&](const auto& edge) {
            return edge.from_node == from && edge.from_port == from_port &&
                   edge.to_node == to && edge.to_port == to_port;
        });
    };
    REQUIRE(has_connection("file", "audio", "gain-b", "audio_in"));
    REQUIRE(has_connection("gain-b", "audio_out", "gain", "audio_in"));
    REQUIRE(has_connection("gain", "audio_out", "out", "audio"));
    const auto swapped_gain = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "gain";
    });
    const auto swapped_gain_b = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "gain-b";
    });
    REQUIRE(swapped_gain != project.nodes.end());
    REQUIRE(swapped_gain_b != project.nodes.end());
    REQUIRE(swapped_gain->x == before.nodes[3].x);
    REQUIRE(swapped_gain->y == before.nodes[3].y);
    REQUIRE(swapped_gain_b->x == before.nodes[1].x);
    REQUIRE(swapped_gain_b->y == before.nodes[1].y);
    REQUIRE(swapped_gain->parameters.at("gain_db") == audioalgo::ParameterValue{3.0});
    REQUIRE(swapped_gain_b->parameters.at("gain_db") == audioalgo::ParameterValue{-6.0});
    REQUIRE(project.revision == 91);
    REQUIRE(audioalgo::validate_project(project, registry).empty());
    std::vector<audioalgo::GraphDiagnostic> order_diagnostics;
    const auto order = audioalgo::topological_order(project, registry, order_diagnostics);
    REQUIRE(order_diagnostics.empty());
    REQUIRE(std::find(order.begin(), order.end(), "gain-b") < std::find(order.begin(), order.end(), "gain"));
    const auto after = project;

    REQUIRE(history.undo(project));
    auto expected_undo = before;
    expected_undo.revision = 92;
    REQUIRE(same_project_state(project, expected_undo));
    REQUIRE(history.redo(project));
    auto expected_redo = after;
    expected_redo.revision = 93;
    REQUIRE(same_project_state(project, expected_redo));
}

TEST_CASE("NI0010.b", "[node_interaction][NI0010]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.push_back({"gain-b", "builtin.gain", {{"gain_db", 0.0}}, 430.0, 90.0});
    project.nodes.push_back({"gain-side", "builtin.gain", {{"gain_db", 0.0}}, 430.0, 220.0});
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "gain-b", "audio_in"},
        {"gain", "audio_out", "gain-side", "audio_in"},
        {"gain-b", "audio_out", "out", "audio"},
    };
    const auto before = project;
    REQUIRE_FALSE(audioalgo::editor::try_swap_adjacent_processing_nodes(
        project, registry, "gain", "gain-b"));
    REQUIRE(same_project_state(project, before));
}

TEST_CASE("NED-11.b", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection retained{"gain", "audio_out", "out", "audio"};
    project.connections = {target, retained};
    project.revision = 800;
    const auto before = project;
    audioalgo::editor::EditorTopologyHistory history;
    std::optional<audioalgo::editor::LibraryNodeInsertionResult> inserted;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        inserted = audioalgo::editor::try_insert_library_node_on_connection(
            candidate, registry, "builtin.gain", "single-history-insert", target, 252.0, 145.0);
        return inserted.has_value();
    }));
    REQUIRE(inserted.has_value());
    REQUIRE(history.undo_size() == 1);
    REQUIRE(history.redo_size() == 0);
    REQUIRE(project.nodes.size() == before.nodes.size() + 1);
    REQUIRE(project.connections.size() == before.connections.size() + 1);
    REQUIRE(project.revision == 801);
    auto expected_redo = project;

    REQUIRE(history.undo(project));
    auto expected_undo = before;
    expected_undo.revision = 802;
    REQUIRE(same_project_state(project, expected_undo));
    REQUIRE_FALSE(std::any_of(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "single-history-insert";
    }));

    REQUIRE(history.redo(project));
    expected_redo.revision = 803;
    REQUIRE(same_project_state(project, expected_redo));
}

TEST_CASE("NED-11.c", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.nodes.push_back({"existing-gain", "builtin.gain", {{"gain_db", -6.0}}, 120.0, 260.0});
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection retained{"gain", "audio_out", "out", "audio"};
    project.connections = {target, retained};
    project.revision = 900;
    const auto before = project;
    audioalgo::editor::EditorTopologyHistory history;
    std::optional<audioalgo::editor::LibraryNodeInsertionResult> inserted;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        inserted = audioalgo::editor::try_insert_existing_node_on_connection(
            candidate, registry, "existing-gain", target, 350.0, 140.0);
        return inserted.has_value();
    }));
    REQUIRE(inserted.has_value());
    REQUIRE(project.nodes.size() == before.nodes.size());
    REQUIRE(history.undo_size() == 1);
    const auto moved = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "existing-gain";
    });
    REQUIRE(moved != project.nodes.end());
    REQUIRE(moved->x == 350.0);
    REQUIRE(moved->y == 140.0);
    REQUIRE(moved->parameters.at("gain_db") == audioalgo::ParameterValue{-6.0});

    REQUIRE(history.undo(project));
    auto expected_undo = before;
    expected_undo.revision = 902;
    REQUIRE(same_project_state(project, expected_undo));
    const auto restored = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "existing-gain";
    });
    REQUIRE(restored != project.nodes.end());
    REQUIRE(restored->x == 120.0);
    REQUIRE(restored->y == 260.0);
    REQUIRE(restored->parameters.at("gain_db") == audioalgo::ParameterValue{-6.0});

    REQUIRE(history.redo(project));
    auto expected_redo = before;
    expected_redo.nodes.back().x = 350.0;
    expected_redo.nodes.back().y = 140.0;
    expected_redo.connections.erase(expected_redo.connections.begin());
    expected_redo.connections.push_back({"file", "audio", "existing-gain", "audio_in"});
    expected_redo.connections.push_back({"existing-gain", "audio_out", "gain", "audio_in"});
    expected_redo.revision = 903;
    REQUIRE(same_project_state(project, expected_redo));
}

TEST_CASE("NED-11.d", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 1000;
    audioalgo::editor::EditorTopologyHistory history;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::editor::try_add_node_from_descriptor(
            candidate, registry, "builtin.gain", "revision-gain", 700.0, 220.0);
    }));
    const auto after_command = project.revision;
    REQUIRE(after_command == 1001);

    auto& existing_gain = project.nodes[1];
    existing_gain.parameters["gain_db"] = 4.0;
    existing_gain.x = 333.0;
    existing_gain.y = 177.0;
    project.name = "Saved graph name";
    ++project.revision;
    const auto after_parameter_and_save = project.revision;

    REQUIRE(history.undo(project));
    const auto after_undo = project.revision;
    REQUIRE(after_undo == after_parameter_and_save + 1);
    REQUIRE(after_undo > after_command);
    REQUIRE(project.name == "Saved graph name");
    REQUIRE(project.nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{4.0});
    REQUIRE(project.nodes[1].x == 333.0);
    REQUIRE(project.nodes[1].y == 177.0);
    REQUIRE_FALSE(std::any_of(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "revision-gain";
    }));

    REQUIRE(history.redo(project));
    const auto after_redo = project.revision;
    REQUIRE(after_redo == after_undo + 1);
    REQUIRE(after_redo > after_undo);
    REQUIRE(project.name == "Saved graph name");
    REQUIRE(project.nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{4.0});
    REQUIRE(project.nodes[1].x == 333.0);
    REQUIRE(project.nodes[1].y == 177.0);
    REQUIRE(project.nodes.back().id == "revision-gain");

    REQUIRE(history.undo(project));
    REQUIRE(project.revision == after_redo + 1);
    REQUIRE(project.revision > after_redo);
}

TEST_CASE("NED-11.e", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.revision = 1100;
    audioalgo::editor::EditorTopologyHistory history;

    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::editor::try_add_node_from_descriptor(
            candidate, registry, "builtin.gain", "redo-branch-gain", 720.0, 220.0);
    }));
    REQUIRE(history.undo(project));
    REQUIRE_FALSE(history.can_undo());
    REQUIRE(history.can_redo());
    const auto before_failed_command = project;

    REQUIRE_FALSE(history.apply(project, [](auto& candidate) {
        candidate.nodes.push_back({"partial-node", "builtin.gain", {{"gain_db", 9.0}}, 800.0, 300.0});
        candidate.connections.push_back({"missing", "audio", "partial-node", "audio_in"});
        ++candidate.revision;
        return false;
    }));
    REQUIRE(same_project_state(project, before_failed_command));
    REQUIRE_FALSE(history.can_undo());
    REQUIRE(history.can_redo());
    REQUIRE(history.redo_size() == 1);

    const audioalgo::Connection first_edge{"file", "audio", "gain", "audio_in"};
    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::try_add_connection(candidate, first_edge, registry).empty();
    }));
    REQUIRE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.undo_size() == 1);
    REQUIRE(history.redo_size() == 0);
    REQUIRE_FALSE(history.redo(project));
    REQUIRE_FALSE(std::any_of(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "partial-node" || node.id == "redo-branch-gain";
    }));
}

TEST_CASE("NED-11.f", "[node_interaction][NED-11]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    const audioalgo::Connection edge{"file", "audio", "gain", "audio_in"};
    audioalgo::editor::EditorTopologyHistory history;
    REQUIRE(history.apply(project, [&](auto& candidate) {
        return audioalgo::try_add_connection(candidate, edge, registry).empty();
    }));
    REQUIRE(history.can_undo());

    audioalgo::editor::LibraryDropSession library_drop;
    REQUIRE(library_drop.begin("builtin.gain", 9, project.revision));
    REQUIRE(library_drop.update({280.0f, 160.0f}, audioalgo::editor::EditorPoint{192.0f, 110.0f}, edge));
    audioalgo::editor::PortConnectionGesture connection_gesture;
    REQUIRE(connection_gesture.begin("gain", "audio_out", {420.0f, 80.0f}));
    connection_gesture.mark_drag_started();
    REQUIRE(connection_gesture.is_dragging());

    const auto clear_old_project_context = [&] {
        history.clear();
        library_drop.cancel();
        connection_gesture.cancel();
    };
    clear_old_project_context();
    project = audioalgo::ProjectModel{};
    project.project_id = "new-project-context";
    project.name = "New project";
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE_FALSE(library_drop.is_active());
    REQUIRE_FALSE(library_drop.target_connection().has_value());
    REQUIRE_FALSE(connection_gesture.is_active());
    REQUIRE(project.connections.empty());

    REQUIRE(history.apply(project, [&](auto& candidate) {
        candidate.project_id = "opened-project-context";
        candidate.name = "Opened project";
        candidate.nodes = {{"opened-source", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 30.0}};
        ++candidate.revision;
        return true;
    }));
    REQUIRE(history.can_undo());
    clear_old_project_context();
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE_FALSE(library_drop.is_active());
    REQUIRE_FALSE(connection_gesture.is_active());
    REQUIRE(project.project_id == "opened-project-context");
    REQUIRE(project.nodes.size() == 1);
}

TEST_CASE("NED-13.a", "[node_interaction][NED-13]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_node_descriptors(registry);
    auto project = disconnected_file_gain_output();
    project.project_id = "ned13-save-inserted-project";
    project.name = "Inserted node save round trip";
    project.revision = 31;
    project.nodes[0].parameters["path"] = std::string("audio fixtures/final take.wav");
    std::get<double>(project.nodes[1].parameters.at("gain_db")) = 6.0;
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "out", "audio"},
    };

    const audioalgo::Connection inserted_edge{"gain", "audio_out", "out", "audio"};
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, registry, "builtin.gain", "ned13-inserted-gain", inserted_edge, 351.25, 88.5);
    REQUIRE(inserted.has_value());
    REQUIRE(project.revision == 32);
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "ned13-inserted-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;

    const auto directory = temporary_project_directory();
    const auto path = directory / "inserted-project.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(path, project)));
    const auto loaded_result = audioalgo::load_project_file(path);
    const auto* reopened = std::get_if<audioalgo::ProjectModel>(&loaded_result);
    REQUIRE(reopened != nullptr);
    REQUIRE(same_project_state(project, *reopened));
    REQUIRE(reopened->project_id == "ned13-save-inserted-project");
    REQUIRE(reopened->revision == 33);
    REQUIRE(reopened->selected_output_node_id == "out");
    REQUIRE(reopened->nodes[0].parameters.at("path") == audioalgo::ParameterValue{std::string("audio fixtures/final take.wav")});

    const auto persisted_inserted = std::find_if(reopened->nodes.begin(), reopened->nodes.end(), [](const auto& node) {
        return node.id == "ned13-inserted-gain";
    });
    REQUIRE(persisted_inserted != reopened->nodes.end());
    REQUIRE(persisted_inserted->type_id == "builtin.gain");
    REQUIRE(persisted_inserted->parameters.at("gain_db") == audioalgo::ParameterValue{-6.0});
    REQUIRE(persisted_inserted->x == 351.25);
    REQUIRE(persisted_inserted->y == 88.5);
    REQUIRE(std::any_of(reopened->connections.begin(), reopened->connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "ned13-inserted-gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(reopened->connections.begin(), reopened->connections.end(), [](const auto& edge) {
        return edge.from_node == "ned13-inserted-gain" && edge.from_port == "audio_out" &&
               edge.to_node == "out" && edge.to_port == "audio";
    }));
    std::filesystem::remove_all(directory);
}
