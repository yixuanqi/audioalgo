#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/RunReception.h"
#include "audio_editor_application/AsyncCompletionSlot.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace {

class ControllerProbeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::AudioPorts{};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        return audioalgo::AudioBlockResult{block.frame_count};
    }
};

audioalgo::NodeDescriptor probe_descriptor() {
    audioalgo::ParameterDescriptor mode{"mode", audioalgo::ParameterKind::enumeration,
                                        std::string("normal"), {}, {}, {}};
    mode.choices = {{"normal", "Normal"}, {"invert", "Invert"}};
    audioalgo::ParameterDescriptor enabled{"enabled", audioalgo::ParameterKind::boolean, true, {}, {}, {}};
    audioalgo::ParameterDescriptor repeat{"repeat", audioalgo::ParameterKind::integer, std::int64_t{2}, 1.0, 8.0,
                                          {}};
    audioalgo::ParameterDescriptor tag{"tag", audioalgo::ParameterKind::string, std::string("initial"), {}, {}, {}};
    audioalgo::ParameterDescriptor coefficient{"coefficient", audioalgo::ParameterKind::path,
                                               std::string("coefficients/default.wav"), {}, {}, {}};
    return {"test.controller_node", "1.0.0", "Controller node",
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {{"gain_db", audioalgo::ParameterKind::floating, 0.0, -60.0, 12.0, "dB"}, std::move(mode),
             std::move(enabled), std::move(repeat), std::move(tag), std::move(coefficient)},
            audioalgo::NodeExecutionKind::block};
}

audioalgo::ProjectModel probe_project() {
    audioalgo::ProjectModel project;
    project.project_id = "controller-project";
    project.name = "Controller project";
    project.revision = 3;
    project.nodes = {
        {"file", "test.controller_source", {{"path", std::string("input.wav")}}, 0.0, 0.0},
        {"gain", "test.controller_node",
         {{"gain_db", 0.0}, {"mode", std::string("normal")}, {"enabled", true}, {"repeat", std::int64_t{2}},
          {"tag", std::string("initial")}, {"coefficient", std::string("coefficients/default.wav")}},
         100.0, 0.0},
        {"output", "test.controller_sink", {}, 200.0, 0.0},
    };
    project.connections = {{"file", "audio", "gain", "audio_in"}, {"gain", "audio_out", "output", "audio"}};
    project.selected_output_node_id = "output";
    return project;
}

audioalgo::NodeRegistry probe_catalog() {
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor({"test.controller_source", "1.0.0", "Source",
                                          {{"audio", "audio.pcm@1", audioalgo::PortDirection::output, false}},
                                          {{"path", audioalgo::ParameterKind::path, std::string{}, {}, {}, {}}},
                                          audioalgo::NodeExecutionKind::source}));
    REQUIRE(registry.register_node(probe_descriptor(), [] {
        return std::make_unique<ControllerProbeProcessor>();
    }).registered);
    REQUIRE(registry.register_descriptor({"test.controller_sink", "1.0.0", "Sink",
                                          {{"audio", "audio.pcm@1", audioalgo::PortDirection::input, true}},
                                          {}, audioalgo::NodeExecutionKind::sink}));
    return registry;
}

bool same_project_state(const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
    if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
        left.name != right.name || left.revision != right.revision ||
        left.selected_output_node_id != right.selected_output_node_id ||
        left.selected_output_port_id != right.selected_output_port_id || left.nodes.size() != right.nodes.size() ||
        left.connections.size() != right.connections.size())
        return false;
    for (std::size_t i = 0; i < left.nodes.size(); ++i) {
        const auto& a = left.nodes[i];
        const auto& b = right.nodes[i];
        if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters || a.x != b.x || a.y != b.y)
            return false;
    }
    for (std::size_t i = 0; i < left.connections.size(); ++i) {
        const auto& a = left.connections[i];
        const auto& b = right.connections[i];
        if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node || a.to_port != b.to_port)
            return false;
    }
    return true;
}

} // namespace

static_assert(std::is_const_v<std::remove_reference_t<
              decltype(std::declval<audioalgo::editor::EditorController&>().history())>>,
              "EditorController must not expose mutable history");

TEST_CASE("AR-08.a AddNode is controller-owned, undoable, and preserves redo on rejection",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto added = controller.add_node("test.controller_node", "extra", 360.0, 72.0);
    REQUIRE(added.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(added.diagnostics.empty());

    auto expected_added = initial;
    expected_added.nodes.push_back({"extra", "test.controller_node",
                                    {{"gain_db", 0.0}, {"mode", std::string("normal")}, {"enabled", true},
                                     {"repeat", std::int64_t{2}}, {"tag", std::string("initial")},
                                     {"coefficient", std::string("coefficients/default.wav")}},
                                    360.0, 72.0});
    expected_added.revision = 4;
    CHECK(same_project_state(controller.project(), expected_added));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);

    REQUIRE(controller.undo());
    auto expected_undone = initial;
    expected_undone.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undone));
    CHECK(controller.history().can_redo());

    const auto reject_without_change = [&](const std::string& type_id, const std::string& node_id) {
        const auto before = controller.project();
        const auto undo_size = controller.history().undo_size();
        const auto redo_size = controller.history().redo_size();
        const auto outcome = controller.add_node(type_id, node_id, 1.0, 2.0);
        CHECK(outcome.status == audioalgo::editor::ProjectEditStatus::rejected);
        CHECK_FALSE(outcome.diagnostics.empty());
        CHECK(same_project_state(controller.project(), before));
        CHECK(controller.history().undo_size() == undo_size);
        CHECK(controller.history().redo_size() == redo_size);
        CHECK(controller.dirty());
        CHECK(controller.results_stale());
    };
    reject_without_change("missing.type", "new-node");
    reject_without_change("test.controller_node", "gain");
    reject_without_change("test.controller_source", "second-source");
    reject_without_change("test.controller_sink", "second-output");

    REQUIRE(controller.redo());
    expected_added.revision = 6;
    CHECK(same_project_state(controller.project(), expected_added));
    CHECK(controller.history().undo_size() == 1);
    CHECK_FALSE(controller.history().can_redo());
}

TEST_CASE("AR-08.a SwapProcessingNodes reports revision exhaustion without changing state",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.nodes.push_back({"peer", "test.controller_node",
                             {{"gain_db", -6.0}, {"mode", std::string("normal")}, {"enabled", true},
                              {"repeat", std::int64_t{2}}, {"tag", std::string("initial")},
                              {"coefficient", std::string("coefficients/default.wav")}},
                             150.0, 40.0});
    initial.connections = {{"file", "audio", "gain", "audio_in"},
                           {"gain", "audio_out", "peer", "audio_in"},
                           {"peer", "audio_out", "output", "audio"}};
    initial.revision = std::numeric_limits<std::uint64_t>::max();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto outcome = controller.swap_processing_nodes("gain", "peer");

    REQUIRE(outcome.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(outcome.diagnostics.empty());
    CHECK(outcome.diagnostics.front().code == "revision_exhausted");
    CHECK(same_project_state(controller.project(), initial));
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
}

TEST_CASE("AR-08.a AddSourceNode records the chosen resource and supports undo and redo",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.nodes.erase(initial.nodes.begin());
    initial.connections.clear();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto added = controller.add_source_node("test.controller_source", "chosen-file",
                                                   "E:/audio/input.wav", 24.5, 73.25);

    REQUIRE(added.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected = initial;
    expected.nodes.push_back({"chosen-file", "test.controller_source",
                              {{"path", std::string("E:/audio/input.wav")}}, 24.5, 73.25});
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    CHECK_FALSE(controller.history().can_redo());

    const auto duplicate_source = controller.add_source_node("test.controller_source", "second-file",
                                                               "E:/audio/other.wav", 40.0, 80.0);
    REQUIRE(duplicate_source.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(duplicate_source.diagnostics.empty());
    CHECK(duplicate_source.diagnostics.front().code == "node_kind_duplicate");
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);

    REQUIRE(controller.undo());
    auto expected_undo = initial;
    expected_undo.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undo));
    REQUIRE(controller.redo());
    expected.revision = 6;
    CHECK(same_project_state(controller.project(), expected));
}

TEST_CASE("AR-08.a SwapProcessingNodes preserves node data and is undoable",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.nodes.push_back({"peer", "test.controller_node",
                             {{"gain_db", -6.0}, {"mode", std::string("invert")}, {"enabled", false},
                              {"repeat", std::int64_t{4}}, {"tag", std::string("peer")},
                              {"coefficient", std::string("peer.wav")}},
                             150.0, 40.0});
    initial.connections = {{"file", "audio", "gain", "audio_in"},
                           {"gain", "audio_out", "peer", "audio_in"},
                           {"peer", "audio_out", "output", "audio"}};
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto swapped = controller.swap_processing_nodes("gain", "peer");

    REQUIRE(swapped.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected = initial;
    expected.nodes[1].x = initial.nodes[3].x;
    expected.nodes[1].y = initial.nodes[3].y;
    expected.nodes[3].x = initial.nodes[1].x;
    expected.nodes[3].y = initial.nodes[1].y;
    expected.connections = {{"file", "audio", "peer", "audio_in"},
                            {"peer", "audio_out", "gain", "audio_in"},
                            {"gain", "audio_out", "output", "audio"}};
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.undo());
    auto expected_undo = initial;
    expected_undo.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undo));
    REQUIRE(controller.redo());
    expected.revision = 6;
    CHECK(same_project_state(controller.project(), expected));
}

TEST_CASE("AR-08.a Connect and Disconnect validate atomically and share undo history",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.connections.clear();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const audioalgo::Connection source_to_gain{"file", "audio", "gain", "audio_in"};
    const audioalgo::Connection gain_to_output{"gain", "audio_out", "output", "audio"};
    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    const auto during_gesture = controller.project();
    const auto blocked_connection = controller.connect(source_to_gain);
    REQUIRE(blocked_connection.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(blocked_connection.diagnostics.empty());
    CHECK(blocked_connection.diagnostics.front().code == "parameter_gesture_active");
    CHECK(same_project_state(controller.project(), during_gesture));
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    REQUIRE(controller.cancel_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::cancelled);

    const auto first = controller.connect(source_to_gain);
    REQUIRE(first.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(first.diagnostics.empty());
    auto expected = initial;
    expected.connections.push_back(source_to_gain);
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);

    const auto second = controller.connect(gain_to_output);
    REQUIRE(second.status == audioalgo::editor::ProjectEditStatus::applied);
    expected.connections.push_back(gain_to_output);
    expected.revision = 5;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.history().undo_size() == 2);

    controller.mark_saved();
    controller.mark_result_current();
    const auto invalid_connection = controller.connect({"file", "audio", "gain", "missing_input"});
    REQUIRE(invalid_connection.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(invalid_connection.diagnostics.empty());
    CHECK(invalid_connection.diagnostics.front().code == "connection_target_port_missing");
    CHECK(invalid_connection.diagnostics.front().node_id == "gain");
    CHECK(invalid_connection.diagnostics.front().port_id == "missing_input");
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.history().undo_size() == 2);
    CHECK(controller.history().redo_size() == 0);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());

    const auto removed = controller.disconnect(source_to_gain);
    REQUIRE(removed.status == audioalgo::editor::ProjectEditStatus::applied);
    expected.connections.erase(expected.connections.begin());
    expected.revision = 6;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 3);
    REQUIRE(controller.undo());
    expected.connections.insert(expected.connections.begin(), source_to_gain);
    expected.revision = 7;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.history().can_redo());

    const auto before_rejected_duplicate = controller.project();
    const auto undo_before_rejected_duplicate = controller.history().undo_size();
    const auto redo_before_rejected_duplicate = controller.history().redo_size();
    const auto duplicate = controller.connect(source_to_gain);
    REQUIRE(duplicate.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(duplicate.diagnostics.empty());
    CHECK(duplicate.diagnostics.front().code == "connection_duplicate");
    CHECK(duplicate.diagnostics.front().node_id == "gain");
    CHECK(duplicate.diagnostics.front().port_id == "audio_in");
    CHECK(same_project_state(controller.project(), before_rejected_duplicate));
    CHECK(controller.history().undo_size() == undo_before_rejected_duplicate);
    CHECK(controller.history().redo_size() == redo_before_rejected_duplicate);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());

    REQUIRE(controller.redo());
    expected.connections.erase(expected.connections.begin());
    expected.revision = 8;
    CHECK(same_project_state(controller.project(), expected));
    CHECK_FALSE(controller.history().can_redo());

    const auto before_missing_disconnect = controller.project();
    const auto undo_before_missing_disconnect = controller.history().undo_size();
    const auto missing = controller.disconnect(source_to_gain);
    REQUIRE(missing.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(missing.diagnostics.empty());
    CHECK(missing.diagnostics.front().code == "connection_missing");
    CHECK(same_project_state(controller.project(), before_missing_disconnect));
    CHECK(controller.history().undo_size() == undo_before_missing_disconnect);
    CHECK_FALSE(controller.history().can_redo());
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
}

TEST_CASE("AR-08.a InsertNode replaces one edge, preserves branches, and is atomic",
          "[R2][AR-08.a][NED-07]") {
    auto registry = probe_catalog();
    REQUIRE(registry.register_descriptor({
        "test.multi_port_block", "1.0.0", "Multi-port block",
        {{"input_a", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"input_b", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"output_a", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"output_b", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block}));

    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.nodes.push_back({"branch", "test.controller_node",
                             {{"gain_db", 0.0}, {"mode", std::string("normal")}, {"enabled", true},
                              {"repeat", std::int64_t{2}}, {"tag", std::string("initial")},
                              {"coefficient", std::string("coefficients/default.wav")}},
                             120.0, 120.0});
    initial.connections.push_back({"file", "audio", "branch", "audio_in"});
    controller.adopt_project(initial);
    controller.mark_result_current();

    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const auto reject_without_change = [&](const audioalgo::Connection& edge,
                                           std::optional<audioalgo::editor::InsertionPortChoice> choice,
                                           const std::string& expected_code) {
        const auto before = controller.project();
        const auto undo_size = controller.history().undo_size();
        const auto redo_size = controller.history().redo_size();
        const auto dirty = controller.dirty();
        const auto stale = controller.results_stale();
        const auto outcome = controller.insert_node("test.multi_port_block", "inserted", edge, 50.0, 70.0,
                                                    std::move(choice));
        CHECK(outcome.status == audioalgo::editor::ProjectEditStatus::rejected);
        REQUIRE_FALSE(outcome.diagnostics.empty());
        CHECK(outcome.diagnostics.front().code == expected_code);
        CHECK(same_project_state(controller.project(), before));
        CHECK(controller.history().undo_size() == undo_size);
        CHECK(controller.history().redo_size() == redo_size);
        CHECK(controller.dirty() == dirty);
        CHECK(controller.results_stale() == stale);
    };
    reject_without_change(target, std::nullopt, "connection_insertion_choice_required");
    reject_without_change(target, audioalgo::editor::InsertionPortChoice{"missing_input", "output_a"},
                          "connection_insertion_choice_invalid");
    reject_without_change({"file", "audio", "missing", "audio"},
                          audioalgo::editor::InsertionPortChoice{"input_b", "output_a"},
                          "connection_target_missing");

    const auto inserted = controller.insert_node(
        "test.multi_port_block", "inserted", target, 50.0, 70.0,
        audioalgo::editor::InsertionPortChoice{"input_b", "output_a"});
    REQUIRE(inserted.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(inserted.insertion.has_value());
    CHECK(inserted.insertion->node_id == "inserted");
    CHECK(inserted.insertion->input_port_id == "input_b");
    CHECK(inserted.insertion->output_port_id == "output_a");
    CHECK(inserted.insertion->x == 50.0);
    CHECK(inserted.insertion->y == 70.0);

    auto expected_inserted = initial;
    expected_inserted.connections.erase(expected_inserted.connections.begin());
    expected_inserted.nodes.push_back({"inserted", "test.multi_port_block", {}, 50.0, 70.0});
    expected_inserted.connections.push_back({"file", "audio", "inserted", "input_b"});
    expected_inserted.connections.push_back({"inserted", "output_a", "gain", "audio_in"});
    expected_inserted.revision = 4;
    CHECK(same_project_state(controller.project(), expected_inserted));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.undo());
    auto expected_undone = initial;
    expected_undone.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undone));
    CHECK(controller.history().can_redo());
    reject_without_change({"file", "audio", "missing", "audio"},
                          audioalgo::editor::InsertionPortChoice{"input_b", "output_a"},
                          "connection_target_missing");

    REQUIRE(controller.redo());
    expected_inserted.revision = 6;
    CHECK(same_project_state(controller.project(), expected_inserted));
    CHECK_FALSE(controller.history().can_redo());
}

TEST_CASE("AR-08.a Existing unconnected node insertion preserves identity and parameters",
          "[R2][AR-08.a][NED-07]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    const audioalgo::NodeParameters spare_parameters{
        {"gain_db", -9.5}, {"mode", std::string("invert")}, {"enabled", false},
        {"repeat", std::int64_t{7}}, {"tag", std::string("keep this state")},
        {"coefficient", std::string("coefficients/custom.wav")}};
    initial.nodes.push_back({"spare", "test.controller_node", spare_parameters, 350.0, 210.0});
    initial.nodes.push_back({"branch", "test.controller_node",
                             {{"gain_db", 0.0}, {"mode", std::string("normal")}, {"enabled", true},
                              {"repeat", std::int64_t{2}}, {"tag", std::string("initial")},
                              {"coefficient", std::string("coefficients/default.wav")}},
                             120.0, 120.0});
    initial.connections.push_back({"file", "audio", "branch", "audio_in"});
    controller.adopt_project(initial);
    controller.mark_result_current();

    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};
    const auto reject_without_change = [&](const std::string& node_id, const audioalgo::Connection& edge,
                                           std::optional<audioalgo::editor::InsertionPortChoice> choice,
                                           const std::string& expected_code) {
        const auto before = controller.project();
        const auto undo_size = controller.history().undo_size();
        const auto redo_size = controller.history().redo_size();
        const auto dirty = controller.dirty();
        const auto stale = controller.results_stale();
        const auto outcome = controller.insert_existing_node(node_id, edge, 50.0, 70.0, std::move(choice));
        CHECK(outcome.status == audioalgo::editor::ProjectEditStatus::rejected);
        REQUIRE_FALSE(outcome.diagnostics.empty());
        CHECK(outcome.diagnostics.front().code == expected_code);
        CHECK(same_project_state(controller.project(), before));
        CHECK(controller.history().undo_size() == undo_size);
        CHECK(controller.history().redo_size() == redo_size);
        CHECK(controller.dirty() == dirty);
        CHECK(controller.results_stale() == stale);
    };
    reject_without_change("gain", target, std::nullopt, "insertion_node_connected");
    reject_without_change("spare", target, audioalgo::editor::InsertionPortChoice{"wrong", "audio_out"},
                          "connection_insertion_choice_invalid");
    reject_without_change("spare", {"file", "audio", "missing", "audio"}, std::nullopt,
                          "connection_target_missing");

    const auto inserted = controller.insert_existing_node("spare", target, 50.0, 70.0);
    REQUIRE(inserted.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(inserted.insertion.has_value());
    CHECK(inserted.insertion->node_id == "spare");
    CHECK(inserted.insertion->input_port_id == "audio_in");
    CHECK(inserted.insertion->output_port_id == "audio_out");

    auto expected_inserted = initial;
    expected_inserted.nodes[3].x = 50.0;
    expected_inserted.nodes[3].y = 70.0;
    expected_inserted.connections.erase(expected_inserted.connections.begin());
    expected_inserted.connections.push_back({"file", "audio", "spare", "audio_in"});
    expected_inserted.connections.push_back({"spare", "audio_out", "gain", "audio_in"});
    expected_inserted.revision = 4;
    CHECK(same_project_state(controller.project(), expected_inserted));
    CHECK(controller.project().nodes[3].id == "spare");
    CHECK(controller.project().nodes[3].parameters == spare_parameters);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.undo());
    auto expected_undone = initial;
    expected_undone.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undone));
    CHECK(controller.history().can_redo());
    reject_without_change("gain", target, std::nullopt, "insertion_node_connected");

    REQUIRE(controller.redo());
    expected_inserted.revision = 6;
    CHECK(same_project_state(controller.project(), expected_inserted));
    CHECK_FALSE(controller.history().can_redo());
}

TEST_CASE("AR-08.a DeleteNode uses a legal linear bypass and preserves redo on rejection",
          "[R2][AR-08.a][NED-09]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto deleted = controller.delete_node("gain");
    REQUIRE(deleted.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected_deleted = initial;
    expected_deleted.nodes.erase(expected_deleted.nodes.begin() + 1);
    expected_deleted.connections = {{"file", "audio", "output", "audio"}};
    expected_deleted.revision = 4;
    CHECK(same_project_state(controller.project(), expected_deleted));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.undo());
    auto expected_undone = initial;
    expected_undone.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undone));
    CHECK(controller.history().can_redo());

    const auto before_rejected_delete = controller.project();
    const auto undo_size = controller.history().undo_size();
    const auto redo_size = controller.history().redo_size();
    const auto rejected = controller.delete_node("missing");
    REQUIRE(rejected.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(rejected.diagnostics.empty());
    CHECK(rejected.diagnostics.front().code == "node_missing");
    CHECK(same_project_state(controller.project(), before_rejected_delete));
    CHECK(controller.history().undo_size() == undo_size);
    CHECK(controller.history().redo_size() == redo_size);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());

    REQUIRE(controller.redo());
    expected_deleted.revision = 6;
    CHECK(same_project_state(controller.project(), expected_deleted));
    CHECK_FALSE(controller.history().can_redo());
}

TEST_CASE("AR-09.c late service notification cannot reference deleted nodes or stale View state",
          "[R2][AR-09.c]") {
    struct ViewSnapshot {
        std::string selected_node_id = "gain";
        std::string result_path = "results/current.wav";
        std::string waveform_identity = "waveform-before-delete";
        std::string status = "Completed";
        bool playback_ready = true;
        bool operator==(const ViewSnapshot&) const = default;
    };

    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());
    controller.mark_result_current();

    audioalgo::RunReception reception;
    const audioalgo::RunIdentity in_flight{"controller-project", 1, "run-before-delete", controller.revision()};
    reception.begin(in_flight);
    const ViewSnapshot view_before_delete;

    const auto deleted = controller.delete_node("gain");
    REQUIRE(deleted.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.revision() == 4);
    REQUIRE(controller.results_stale());
    REQUIRE(std::none_of(controller.project().nodes.begin(), controller.project().nodes.end(),
                         [](const auto& node) { return node.id == "gain"; }));

    auto view_before_late_notification = view_before_delete;
    view_before_late_notification.selected_node_id.clear();
    view_before_late_notification.status = "Controller project · modified";
    // This is the production RunReception gate used by EditorComponent::run_finished.
    // A delayed completion from the prior revision must not touch any presentation state.
    auto view_after_late_notification = view_before_late_notification;
    if (reception.accepts(in_flight, controller.project().project_id, 1, controller.revision()))
        view_after_late_notification = {"gain", "results/late.wav", "late-waveform", "Completed late", true};

    CHECK(view_after_late_notification == view_before_late_notification);
    CHECK(reception.active() == std::optional<audioalgo::RunIdentity>{in_flight});
    CHECK(controller.history().undo_size() == 1);
}

TEST_CASE("AR-09.c immutable snapshot intent is rejected after same-ID project replacement",
          "[R2][AR-09.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());

    auto stale_view = controller.node_snapshot("gain");
    REQUIRE(stale_view.has_value());
    CHECK(stale_view->node.id == "gain");
    CHECK(stale_view->node.type_id == "test.controller_node");

    // A consumer may keep or alter this value copy; it cannot alias the controller's model.
    stale_view->node.parameters["gain_db"] = -24.0;
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 0.0);

    auto reopened = probe_project();
    reopened.nodes[1].parameters["gain_db"] = 3.0;
    controller.adopt_project(std::move(reopened)); // same project ID, revision, and node IDs
    controller.mark_result_current();

    const auto before_stale_intent = controller.project();
    const auto stale_outcome = controller.set_parameter(stale_view->session, "gain", "gain_db", 9.0);
    REQUIRE(stale_outcome.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(stale_outcome.diagnostic.has_value());
    CHECK(stale_outcome.diagnostic->code == "project_session_stale");
    CHECK(same_project_state(controller.project(), before_stale_intent));
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 3.0);
    CHECK(controller.revision() == 3);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);

    const auto current_view = controller.node_snapshot("gain");
    REQUIRE(current_view.has_value());
    const auto current_outcome = controller.set_parameter(current_view->session, "gain", "gain_db", 6.0);
    CHECK(current_outcome.status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(controller.revision() == 4);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 6.0);
}

TEST_CASE("AR-08.e Project replacement clears history and rejects late chooser results",
          "[R2][AR-08.e]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto project_a = probe_project();
    controller.adopt_project(project_a);
    const auto old_view = controller.node_snapshot("gain");
    REQUIRE(old_view.has_value());

    REQUIRE(controller.set_parameter(old_view->session, "gain", "gain_db", 3.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(controller.undo());
    CHECK(controller.history().can_redo());
    REQUIRE(controller.begin_parameter_gesture(old_view->session, "gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(old_view->session, 8.0).status ==
            audioalgo::editor::ParameterEditStatus::staged);

    audioalgo::AsyncCompletionSlot<std::string> chooser;
    const auto old_chooser = chooser.begin([&](std::string) {
        (void)controller.set_parameter(old_view->session, "gain", "gain_db", 9.0);
    });

    auto project_b = project_a;
    project_b.project_id = "replacement-project";
    project_b.name = "Replacement project";
    project_b.nodes[1].parameters["gain_db"] = 2.0;
    chooser.invalidate();
    controller.adopt_project(project_b);

    CHECK(controller.session_identity() != old_view->session);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 2.0);
    CHECK_FALSE(chooser.take(old_chooser));
    CHECK(controller.begin_parameter_gesture(controller.session_identity(), "gain", "gain_db").status ==
          audioalgo::editor::ParameterEditStatus::started);
    CHECK(controller.cancel_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::cancelled);

    const auto current_session = controller.session_identity();
    const auto current_chooser = chooser.begin([&](std::string) {
        (void)controller.set_parameter(current_session, "gain", "gain_db", 4.0);
    });
    auto completion = chooser.take(current_chooser);
    REQUIRE(completion);
    completion("replacement-input.wav");
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 4.0);
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
}

TEST_CASE("AR-08.a DeleteNode keeps a branch instead of bypassing it",
          "[R2][AR-08.a][NED-09]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    initial.nodes.push_back({"branch", "test.controller_node",
                             {{"gain_db", 0.0}, {"mode", std::string("normal")}, {"enabled", true},
                              {"repeat", std::int64_t{2}}, {"tag", std::string("initial")},
                              {"coefficient", std::string("coefficients/default.wav")}},
                             120.0, 100.0});
    initial.connections.push_back({"gain", "audio_out", "branch", "audio_in"});
    controller.adopt_project(initial);

    const auto deleted = controller.delete_node("gain");
    REQUIRE(deleted.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected = initial;
    expected.nodes.erase(expected.nodes.begin() + 1);
    expected.connections.clear();
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.project().selected_output_node_id == "output");

    REQUIRE(controller.undo());
    auto expected_undo = initial;
    expected_undo.revision = 5;
    CHECK(same_project_state(controller.project(), expected_undo));
}

TEST_CASE("AR-08.a Deleting the selected output clears it and undo restores it",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);

    const auto deleted = controller.delete_node("output");
    REQUIRE(deleted.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected = initial;
    expected.nodes.pop_back();
    expected.connections.pop_back();
    expected.selected_output_node_id.clear();
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    REQUIRE(controller.undo());
    expected = initial;
    expected.revision = 5;
    CHECK(same_project_state(controller.project(), expected));
}

TEST_CASE("AR-14.c SetSelectedOutputPort applies one undoable revision and validates the port",
          "[R5][AR-14.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    REQUIRE(controller.revision() == 3);

    SECTION("a legal port selection is one undoable revision") {
        const auto applied = controller.set_selected_output_port("gain", "audio_out");
        REQUIRE(applied.status == audioalgo::editor::ProjectEditStatus::applied);
        CHECK(applied.diagnostics.empty());
        auto expected = initial;
        expected.selected_output_node_id = "gain";
        expected.selected_output_port_id = "audio_out";
        expected.revision = 4;
        CHECK(same_project_state(controller.project(), expected));
        CHECK(controller.dirty());

        REQUIRE(controller.undo());
        expected = initial;
        expected.revision = 5;
        CHECK(same_project_state(controller.project(), expected));
        REQUIRE(controller.redo());
        expected.selected_output_node_id = "gain";
        expected.selected_output_port_id = "audio_out";
        expected.revision = 6;
        CHECK(same_project_state(controller.project(), expected));

        // Re-selecting the same port is a no-op.
        const auto unchanged = controller.set_selected_output_port("gain", "audio_out");
        CHECK(unchanged.status == audioalgo::editor::ProjectEditStatus::unchanged);
        CHECK(controller.revision() == 6);
    }

    SECTION("invalid selections are rejected with located diagnostics") {
        const auto revision_before = controller.revision();
        const auto undo_before = controller.history().undo_size();
        const std::tuple<std::string, std::string, std::string> rejects[] = {
            {"nope", "audio_out", "selected_output_missing"},
            {"gain", "nope", "selected_output_port_missing"},
            {"gain", "audio_in", "selected_output_port_direction"},
        };
        for (const auto& [node_id, port_id, code] : rejects) {
            const auto outcome = controller.set_selected_output_port(node_id, port_id);
            CHECK(outcome.status == audioalgo::editor::ProjectEditStatus::rejected);
            REQUIRE(outcome.diagnostics.size() == 1);
            CHECK(outcome.diagnostics.front().code == code);
        }
        CHECK(controller.revision() == revision_before);
        CHECK(controller.history().undo_size() == undo_before);
        CHECK(same_project_state(controller.project(), initial));
    }
}

TEST_CASE("AR-08.b SetParameter applies one validated revision and never bypasses schema",
          "[R2][AR-08.b]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());
    REQUIRE(controller.revision() == 3);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());

    // A legal edit produces exactly one new computing revision and marks the current result stale.
    const auto applied = controller.set_parameter("gain", "gain_db", 6.0);
    CHECK(applied.status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK_FALSE(applied.diagnostic.has_value());
    CHECK(controller.revision() == 4);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 6.0);

    // Re-applying the same value is a no-op: no revision churn, nothing to undo.
    const auto unchanged = controller.set_parameter("gain", "gain_db", 6.0);
    CHECK(unchanged.status == audioalgo::editor::ParameterEditStatus::unchanged);
    CHECK_FALSE(unchanged.diagnostic.has_value());
    CHECK(controller.revision() == 4);

    // Every illegal edit is rejected with a located diagnostic and leaves model/history untouched.
    const auto revision_before = controller.revision();
    const auto undo_before = controller.history().undo_size();
    const auto rejects = [&](const std::string& parameter, audioalgo::ParameterValue value,
                             const std::string& expected_code) {
        const auto outcome = controller.set_parameter("gain", parameter, std::move(value));
        CHECK(outcome.status == audioalgo::editor::ParameterEditStatus::rejected);
        REQUIRE(outcome.diagnostic.has_value());
        CHECK(outcome.diagnostic->code == expected_code);
        CHECK(outcome.diagnostic->node_id == "gain");
        CHECK(outcome.diagnostic->port_id == parameter);
        CHECK(controller.revision() == revision_before);
        CHECK(controller.history().undo_size() == undo_before);
        CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 6.0);
    };
    rejects("gain_db", 12.01, "parameter_out_of_range");
    rejects("gain_db", -60.5, "parameter_out_of_range");
    rejects("gain_db", std::string("6"), "parameter_type");
    rejects("mode", std::string("INVALID"), "parameter_enum_unknown");
    rejects("mode", std::int64_t{0}, "parameter_type");
    rejects("missing_parameter", 1.0, "parameter_unknown");

    // Unknown nodes are rejected without touching anything.
    const auto unknown_node = controller.set_parameter("ghost", "gain_db", 1.0);
    CHECK(unknown_node.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(unknown_node.diagnostic.has_value());
    CHECK(unknown_node.diagnostic->code == "node_missing");
    CHECK(controller.revision() == revision_before);

    // A legal enum edit goes through the same schema path.
    const auto enum_edit = controller.set_parameter("gain", "mode", std::string("invert"));
    CHECK(enum_edit.status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(controller.revision() == revision_before + 1);

    // Saving clears the dirty flag but a stale result stays stale until the project is re-run.
    controller.mark_saved();
    CHECK_FALSE(controller.dirty());
    CHECK(controller.results_stale());

    // Adopting a new project resets history, dirty and stale state.
    controller.adopt_project(probe_project());
    CHECK(controller.revision() == 3);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);
}

TEST_CASE("AR-08.c Parameter undo restores the previous value and redo restores the edited value",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());

    REQUIRE(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 0.0);
    REQUIRE(controller.set_parameter("gain", "gain_db", 6.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 6.0);
    REQUIRE(controller.revision() == 4);

    REQUIRE(controller.undo());
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 0.0);
    CHECK(controller.revision() == 5);
    CHECK(controller.history().can_redo());

    REQUIRE(controller.redo());
    CHECK(std::get<double>(controller.project().nodes[1].parameters.at("gain_db")) == 6.0);
    CHECK(controller.revision() == 6);
    CHECK(controller.history().can_undo());
}

TEST_CASE("AR-08.c Parameter history restores every declared value type",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());

    const std::vector<std::pair<std::string, audioalgo::ParameterValue>> edits{
        {"enabled", false},
        {"repeat", std::int64_t{5}},
        {"gain_db", 6.0},
        {"mode", std::string("invert")},
        {"tag", std::string("edited label")},
        {"coefficient", std::string("coefficients/edited.wav")},
    };
    for (const auto& [parameter_id, edited_value] : edits) {
        const auto old_value = controller.project().nodes[1].parameters.at(parameter_id);
        const auto before_revision = controller.revision();
        REQUIRE(controller.set_parameter("gain", parameter_id, edited_value).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        CHECK(controller.project().nodes[1].parameters.at(parameter_id) == edited_value);
        CHECK(controller.revision() == before_revision + 1);

        REQUIRE(controller.undo());
        CHECK(controller.project().nodes[1].parameters.at(parameter_id) == old_value);
        CHECK(controller.revision() == before_revision + 2);

        REQUIRE(controller.redo());
        CHECK(controller.project().nodes[1].parameters.at(parameter_id) == edited_value);
        CHECK(controller.revision() == before_revision + 3);
    }

    REQUIRE(controller.undo());
    const auto undo_revision = controller.revision();
    const auto invalid = controller.set_parameter("gain", "gain_db", 12.01);
    CHECK(invalid.status == audioalgo::editor::ParameterEditStatus::rejected);
    CHECK(controller.revision() == undo_revision);
    CHECK(controller.history().can_redo());

    const auto unchanged = controller.set_parameter("gain", "gain_db", 6.0);
    CHECK(unchanged.status == audioalgo::editor::ParameterEditStatus::unchanged);
    CHECK(controller.revision() == undo_revision);
    CHECK(controller.history().can_redo());

    REQUIRE(controller.set_parameter("gain", "gain_db", 3.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    CHECK_FALSE(controller.history().can_redo());
}

TEST_CASE("AR-08.c Undo restores an absent parameter key and Redo restores its explicit value",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto project = probe_project();
    project.nodes[1].parameters.erase("gain_db");
    controller.adopt_project(std::move(project));

    REQUIRE_FALSE(controller.project().nodes[1].parameters.contains("gain_db"));
    REQUIRE(controller.set_parameter("gain", "gain_db", 0.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(controller.project().nodes[1].parameters.contains("gain_db"));

    REQUIRE(controller.undo());
    CHECK_FALSE(controller.project().nodes[1].parameters.contains("gain_db"));
    REQUIRE(controller.redo());
    CHECK(controller.project().nodes[1].parameters.contains("gain_db"));
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
}

TEST_CASE("AR-08.c Parameter history is scoped to the edited node instance",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto project = probe_project();
    project.nodes.push_back({"gain_peer", "test.controller_node", {{"gain_db", 3.0}}, 140.0, 40.0});
    controller.adopt_project(std::move(project));

    REQUIRE(controller.set_parameter("gain", "gain_db", 6.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(controller.set_parameter("gain_peer", "gain_db", -6.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(controller.undo());
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{6.0});
    CHECK(controller.project().nodes[3].parameters.at("gain_db") == audioalgo::ParameterValue{3.0});

    REQUIRE(controller.undo());
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
    CHECK(controller.project().nodes[3].parameters.at("gain_db") == audioalgo::ParameterValue{3.0});
}

TEST_CASE("AR-08.c Fixed parameter event table matches an independent project reducer",
          "[R2][AR-08.c]") {
    enum class EventKind { set, undo, redo };
    struct Event {
        EventKind kind;
        std::string node_id;
        std::string parameter_id;
        std::optional<audioalgo::ParameterValue> value;
    };
    struct Change {
        std::string node_id;
        std::string parameter_id;
        std::optional<audioalgo::ParameterValue> before;
        std::optional<audioalgo::ParameterValue> after;
    };

    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto expected = probe_project();
    controller.adopt_project(expected);
    std::vector<Change> expected_undo;
    std::vector<Change> expected_redo;
    bool expected_dirty = false;
    bool expected_stale = false;
    const std::vector<Event> events{
        {EventKind::set, "gain", "gain_db", audioalgo::ParameterValue{6.0}},
        {EventKind::set, "gain", "mode", audioalgo::ParameterValue{std::string("invert")}},
        {EventKind::undo, {}, {}, std::nullopt},
        {EventKind::undo, {}, {}, std::nullopt},
        {EventKind::redo, {}, {}, std::nullopt},
        {EventKind::set, "gain", "gain_db", audioalgo::ParameterValue{3.0}},
        {EventKind::set, "gain", "mode", audioalgo::ParameterValue{std::string("invalid")}},
        {EventKind::undo, {}, {}, std::nullopt},
    };

    const auto find_expected_node = [&](const std::string& id) -> audioalgo::NodeInstance* {
        const auto found = std::find_if(expected.nodes.begin(), expected.nodes.end(), [&](const auto& node) {
            return node.id == id;
        });
        return found == expected.nodes.end() ? nullptr : &*found;
    };
    const auto set_expected_value = [&](const Change& change, bool use_after) {
        auto* node = find_expected_node(change.node_id);
        REQUIRE(node != nullptr);
        const auto& value = use_after ? change.after : change.before;
        if (value.has_value())
            node->parameters[change.parameter_id] = *value;
        else
            node->parameters.erase(change.parameter_id);
    };
    const auto same_project = [](const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.revision != right.revision ||
            left.selected_output_node_id != right.selected_output_node_id || left.nodes.size() != right.nodes.size() ||
            left.connections.size() != right.connections.size())
            return false;
        for (std::size_t i = 0; i < left.nodes.size(); ++i) {
            const auto& a = left.nodes[i];
            const auto& b = right.nodes[i];
            if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters || a.x != b.x || a.y != b.y)
                return false;
        }
        for (std::size_t i = 0; i < left.connections.size(); ++i) {
            const auto& a = left.connections[i];
            const auto& b = right.connections[i];
            if (a.from_node != b.from_node || a.from_port != b.from_port || a.to_node != b.to_node ||
                a.to_port != b.to_port)
                return false;
        }
        return true;
    };

    for (const auto& event : events) {
        if (event.kind == EventKind::set) {
            auto* node = find_expected_node(event.node_id);
            REQUIRE(node != nullptr);
            const auto old = node->parameters.find(event.parameter_id);
            const std::optional<audioalgo::ParameterValue> before =
                old == node->parameters.end() ? std::nullopt : std::optional<audioalgo::ParameterValue>{old->second};
            const bool invalid = event.parameter_id == "mode" &&
                                 std::get<std::string>(*event.value) == "invalid";
            const auto outcome = controller.set_parameter(event.node_id, event.parameter_id, *event.value);
            if (invalid) {
                CHECK(outcome.status == audioalgo::editor::ParameterEditStatus::rejected);
                CHECK(controller.revision() == expected.revision);
            } else if (before.has_value() && *before == *event.value) {
                CHECK(outcome.status == audioalgo::editor::ParameterEditStatus::unchanged);
            } else {
                CHECK(outcome.status == audioalgo::editor::ParameterEditStatus::applied);
                Change change{event.node_id, event.parameter_id, before, event.value};
                expected_undo.push_back(change);
                expected_redo.clear();
                set_expected_value(change, true);
                ++expected.revision;
                expected_dirty = true;
                expected_stale = true;
            }
        } else if (event.kind == EventKind::undo) {
            REQUIRE_FALSE(expected_undo.empty());
            const auto change = expected_undo.back();
            REQUIRE(controller.undo());
            set_expected_value(change, false);
            expected_undo.pop_back();
            expected_redo.push_back(change);
            ++expected.revision;
            expected_dirty = true;
            expected_stale = true;
        } else {
            REQUIRE_FALSE(expected_redo.empty());
            const auto change = expected_redo.back();
            REQUIRE(controller.redo());
            set_expected_value(change, true);
            expected_redo.pop_back();
            expected_undo.push_back(change);
            ++expected.revision;
            expected_dirty = true;
            expected_stale = true;
        }

        CHECK(same_project(controller.project(), expected));
        CHECK(controller.dirty() == expected_dirty);
        CHECK(controller.results_stale() == expected_stale);
        CHECK(controller.history().undo_size() == expected_undo.size());
        CHECK(controller.history().redo_size() == expected_redo.size());
    }
}

TEST_CASE("AR-08.c Parameter gesture updates commit once and cancel without changing project state",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());
    const auto initial_revision = controller.revision();

    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    for (const double value : {2.0, 4.0, 6.0}) {
        REQUIRE(controller.update_parameter_gesture(value).status ==
                audioalgo::editor::ParameterEditStatus::staged);
        CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
        CHECK(controller.revision() == initial_revision);
        CHECK_FALSE(controller.dirty());
        CHECK_FALSE(controller.results_stale());
        CHECK(controller.history().undo_size() == 0);
    }

    CHECK(controller.commit_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{6.0});
    CHECK(controller.revision() == initial_revision + 1);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    REQUIRE(controller.undo());
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{0.0});

    REQUIRE(controller.redo());
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{6.0});
    controller.mark_saved();
    controller.mark_result_current();
    const auto clean_revision = controller.revision();
    const auto undo_size = controller.history().undo_size();
    const auto redo_size = controller.history().redo_size();

    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(8.0).status == audioalgo::editor::ParameterEditStatus::staged);
    CHECK(controller.cancel_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::cancelled);
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{6.0});
    CHECK(controller.revision() == clean_revision);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == undo_size);
    CHECK(controller.history().redo_size() == redo_size);
}

TEST_CASE("AR-08.c Invalid gesture updates preserve the draft and block competing edits",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(probe_project());

    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(2.0).status == audioalgo::editor::ParameterEditStatus::staged);
    const auto invalid = controller.update_parameter_gesture(12.01);
    CHECK(invalid.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(invalid.diagnostic.has_value());
    CHECK(invalid.diagnostic->code == "parameter_out_of_range");
    CHECK(invalid.diagnostic->node_id == "gain");
    CHECK(invalid.diagnostic->port_id == "gain_db");

    const auto competing_edit = controller.set_parameter("gain", "gain_db", 4.0);
    CHECK(competing_edit.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(competing_edit.diagnostic.has_value());
    CHECK(competing_edit.diagnostic->code == "parameter_gesture_active");
    CHECK_FALSE(controller.undo());
    CHECK_FALSE(controller.redo());
    CHECK(controller.begin_parameter_gesture("gain", "gain_db").status ==
          audioalgo::editor::ParameterEditStatus::rejected);
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
    CHECK(controller.revision() == 3);
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);

    REQUIRE(controller.commit_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(controller.project().nodes[1].parameters.at("gain_db") == audioalgo::ParameterValue{2.0});
    CHECK(controller.revision() == 4);
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(2.0).status == audioalgo::editor::ParameterEditStatus::staged);
    CHECK(controller.commit_parameter_gesture().status == audioalgo::editor::ParameterEditStatus::unchanged);
    CHECK(controller.revision() == 4);
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);
}

TEST_CASE("AR-08.c Gesture intents from a replaced project session cannot change the current gesture",
          "[R2][AR-08.c]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    auto initial = probe_project();
    controller.adopt_project(initial);
    const auto stale_view = controller.node_snapshot("gain");
    REQUIRE(stale_view.has_value());
    REQUIRE(controller.begin_parameter_gesture(stale_view->session, "gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(stale_view->session, 8.0).status ==
            audioalgo::editor::ParameterEditStatus::staged);

    auto replacement = initial;
    replacement.name = "Replacement project";
    replacement.nodes[1].parameters["gain_db"] = 3.0;
    controller.adopt_project(replacement);
    const auto current_view = controller.node_snapshot("gain");
    REQUIRE(current_view.has_value());

    const auto stale_begin = controller.begin_parameter_gesture(stale_view->session, "gain", "gain_db");
    CHECK(stale_begin.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(stale_begin.diagnostic.has_value());
    CHECK(stale_begin.diagnostic->code == "project_session_stale");
    REQUIRE(controller.begin_parameter_gesture(current_view->session, "gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    REQUIRE(controller.update_parameter_gesture(current_view->session, 5.0).status ==
            audioalgo::editor::ParameterEditStatus::staged);

    for (const auto& stale_action : {
             controller.update_parameter_gesture(stale_view->session, 9.0),
             controller.commit_parameter_gesture(stale_view->session),
             controller.cancel_parameter_gesture(stale_view->session)}) {
        CHECK(stale_action.status == audioalgo::editor::ParameterEditStatus::rejected);
        REQUIRE(stale_action.diagnostic.has_value());
        CHECK(stale_action.diagnostic->code == "project_session_stale");
    }
    CHECK(same_project_state(controller.project(), replacement));
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);

    const auto committed = controller.commit_parameter_gesture(current_view->session);
    REQUIRE(committed.status == audioalgo::editor::ParameterEditStatus::applied);
    auto expected = replacement;
    expected.nodes[1].parameters["gain_db"] = 5.0;
    expected.revision = 4;
    CHECK(same_project_state(controller.project(), expected));
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());

    REQUIRE(controller.undo());
    expected.nodes[1].parameters["gain_db"] = 3.0;
    expected.revision = 5;
    CHECK(same_project_state(controller.project(), expected));
    REQUIRE(controller.redo());
    expected.nodes[1].parameters["gain_db"] = 5.0;
    expected.revision = 6;
    CHECK(same_project_state(controller.project(), expected));
}

TEST_CASE("AR-08.d MoveNodeLayout changes only saved layout and keeps the current result valid",
          "[R2][AR-08.d]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto moved = controller.move_node_layout("gain", 234.5, 88.25);
    REQUIRE(moved.status == audioalgo::editor::ProjectEditStatus::applied);
    auto expected_moved = initial;
    expected_moved.nodes[1].x = 234.5;
    expected_moved.nodes[1].y = 88.25;
    CHECK(same_project_state(controller.project(), expected_moved));
    CHECK(controller.revision() == initial.revision);
    CHECK(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    CHECK_FALSE(controller.history().can_redo());

    CHECK(controller.move_node_layout("gain", 234.5, 88.25).status ==
          audioalgo::editor::ProjectEditStatus::unchanged);
    CHECK(controller.history().undo_size() == 1);

    REQUIRE(controller.undo());
    CHECK(same_project_state(controller.project(), initial));
    CHECK(controller.revision() == initial.revision);
    CHECK(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().can_redo());

    REQUIRE(controller.redo());
    CHECK(same_project_state(controller.project(), expected_moved));
    CHECK(controller.revision() == initial.revision);
    CHECK(controller.dirty());
    CHECK_FALSE(controller.results_stale());

    const auto before_gesture_conflict = controller.project();
    const auto dirty_before_gesture_conflict = controller.dirty();
    const auto stale_before_gesture_conflict = controller.results_stale();
    const auto undo_before_gesture_conflict = controller.history().undo_size();
    const auto redo_before_gesture_conflict = controller.history().redo_size();
    REQUIRE(controller.begin_parameter_gesture("gain", "gain_db").status ==
            audioalgo::editor::ParameterEditStatus::started);
    const auto gesture_conflict = controller.move_node_layout("gain", 999.0, 999.0);
    REQUIRE(gesture_conflict.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(gesture_conflict.diagnostics.empty());
    CHECK(gesture_conflict.diagnostics.front().code == "parameter_gesture_active");
    CHECK(same_project_state(controller.project(), before_gesture_conflict));
    CHECK(controller.dirty() == dirty_before_gesture_conflict);
    CHECK(controller.results_stale() == stale_before_gesture_conflict);
    CHECK(controller.history().undo_size() == undo_before_gesture_conflict);
    CHECK(controller.history().redo_size() == redo_before_gesture_conflict);
    REQUIRE(controller.cancel_parameter_gesture().status ==
            audioalgo::editor::ParameterEditStatus::cancelled);

    auto duplicate_project = initial;
    duplicate_project.nodes.push_back(duplicate_project.nodes[1]);
    controller.adopt_project(duplicate_project);
    const auto before_ambiguous_move = controller.project();
    const auto ambiguous_move = controller.move_node_layout("gain", 999.0, 999.0);
    REQUIRE(ambiguous_move.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(ambiguous_move.diagnostics.empty());
    CHECK(ambiguous_move.diagnostics.front().code == "node_id_ambiguous");
    CHECK(same_project_state(controller.project(), before_ambiguous_move));
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);
    CHECK(controller.history().redo_size() == 0);
}

TEST_CASE("AR-08.d Layout history composes with parameter history without swallowing either edit",
          "[R2][AR-08.d][AR-08.f]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();

    REQUIRE(controller.move_node_layout("gain", 150.0, 44.0).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.set_parameter("gain", "gain_db", 4.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    CHECK(controller.revision() == initial.revision + 1);
    CHECK(controller.results_stale());

    REQUIRE(controller.undo());
    auto expected_after_parameter_undo = initial;
    expected_after_parameter_undo.nodes[1].x = 150.0;
    expected_after_parameter_undo.nodes[1].y = 44.0;
    expected_after_parameter_undo.revision = initial.revision + 2;
    CHECK(same_project_state(controller.project(), expected_after_parameter_undo));
    CHECK(controller.results_stale());

    REQUIRE(controller.undo());
    auto expected_after_layout_undo = initial;
    expected_after_layout_undo.revision = initial.revision + 2;
    CHECK(same_project_state(controller.project(), expected_after_layout_undo));
    CHECK(controller.results_stale());

    REQUIRE(controller.redo());
    expected_after_layout_undo.nodes[1].x = 150.0;
    expected_after_layout_undo.nodes[1].y = 44.0;
    CHECK(same_project_state(controller.project(), expected_after_layout_undo));
    CHECK(controller.revision() == initial.revision + 2);
    CHECK(controller.results_stale());

    REQUIRE(controller.redo());
    auto expected_redone = expected_after_layout_undo;
    expected_redone.nodes[1].parameters["gain_db"] = 4.0;
    expected_redone.revision = initial.revision + 3;
    CHECK(same_project_state(controller.project(), expected_redone));
    CHECK(controller.results_stale());

    REQUIRE(controller.move_node_layout("output", 260.0, 12.0).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    expected_redone.nodes[2].x = 260.0;
    expected_redone.nodes[2].y = 12.0;
    CHECK(same_project_state(controller.project(), expected_redone));
    CHECK(controller.revision() == initial.revision + 3);
    CHECK(controller.results_stale());
}

TEST_CASE("AR-08.d Invalid layout edits preserve project state and redo history",
          "[R2][AR-08.d]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();
    REQUIRE(controller.move_node_layout("gain", 125.0, 25.0).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.undo());

    const auto before = controller.project();
    const auto dirty_before = controller.dirty();
    const auto stale_before = controller.results_stale();
    const auto undo_before = controller.history().undo_size();
    const auto redo_before = controller.history().redo_size();
    for (const auto& [node_id, x, y] : std::vector<std::tuple<std::string, double, double>>{
             {"missing", 1.0, 2.0}, {"gain", std::numeric_limits<double>::quiet_NaN(), 2.0}}) {
        const auto rejected = controller.move_node_layout(node_id, x, y);
        REQUIRE(rejected.status == audioalgo::editor::ProjectEditStatus::rejected);
        REQUIRE_FALSE(rejected.diagnostics.empty());
        CHECK(same_project_state(controller.project(), before));
        CHECK(controller.dirty() == dirty_before);
        CHECK(controller.results_stale() == stale_before);
        CHECK(controller.history().undo_size() == undo_before);
        CHECK(controller.history().redo_size() == redo_before);
    }

    REQUIRE(controller.redo());
    auto expected_moved = initial;
    expected_moved.nodes[1].x = 125.0;
    expected_moved.nodes[1].y = 25.0;
    CHECK(same_project_state(controller.project(), expected_moved));
    CHECK(controller.revision() == initial.revision);
    CHECK(controller.dirty());
    CHECK_FALSE(controller.results_stale());
}

TEST_CASE("AR-08.d Layout history rejects edits outside node positions",
          "[R2][AR-08.d]") {
    auto project = probe_project();
    const auto initial = project;
    audioalgo::editor::EditorTopologyHistory history;

    CHECK_FALSE(history.apply_layout(project, [](audioalgo::ProjectModel& candidate) {
        candidate.nodes[1].parameters["gain_db"] = 6.0;
        return true;
    }));
    CHECK(same_project_state(project, initial));
    CHECK(history.undo_size() == 0);
    CHECK(history.redo_size() == 0);

    CHECK_FALSE(history.apply_layout(project, [](audioalgo::ProjectModel& candidate) {
        candidate.name = "unexpected rename";
        return true;
    }));
    CHECK(same_project_state(project, initial));
    CHECK(history.undo_size() == 0);
    CHECK(history.redo_size() == 0);
}

TEST_CASE("AR-08.a DeleteNodes removes several nodes in one undoable revision",
          "[R2][AR-08.a]") {
    const auto registry = probe_catalog();
    audioalgo::editor::EditorController controller(registry);
    const auto initial = probe_project();
    controller.adopt_project(initial);
    controller.mark_result_current();

    const auto removed = controller.delete_nodes({"file", "gain"});
    REQUIRE(removed.status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(removed.diagnostics.empty());
    auto expected_removed = initial;
    expected_removed.nodes.erase(std::remove_if(expected_removed.nodes.begin(), expected_removed.nodes.end(),
                                     [](const auto& node) { return node.id == "file" || node.id == "gain"; }),
        expected_removed.nodes.end());
    expected_removed.connections.clear();
    expected_removed.revision = initial.revision + 1;
    CHECK(same_project_state(controller.project(), expected_removed));
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);

    REQUIRE(controller.undo());
    auto expected_restored = initial;
    expected_restored.revision = initial.revision + 2;
    CHECK(same_project_state(controller.project(), expected_restored));
    CHECK(controller.history().can_redo());

    REQUIRE(controller.redo());
    auto expected_redone = expected_removed;
    expected_redone.revision = initial.revision + 3;
    CHECK(same_project_state(controller.project(), expected_redone));

    const auto before_rejection = controller.project();
    const auto undo_before_rejection = controller.history().undo_size();
    const auto redo_before_rejection = controller.history().redo_size();
    const auto rejected = controller.delete_nodes({"output", "missing"});
    REQUIRE(rejected.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(rejected.diagnostics.empty());
    CHECK(same_project_state(controller.project(), before_rejection));
    CHECK(controller.history().undo_size() == undo_before_rejection);
    CHECK(controller.history().redo_size() == redo_before_rejection);
    CHECK(controller.delete_nodes({}).status == audioalgo::editor::ProjectEditStatus::unchanged);
}
