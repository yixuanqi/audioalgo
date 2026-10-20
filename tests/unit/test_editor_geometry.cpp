#include "../../apps/editor/EditorGeometry.h"
#include "../../apps/editor/EditorComponentLayout.h"
#include "../../apps/editor/EditorNodeCreation.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {
audioalgo::NodeDescriptor gain_descriptor() {
    return {"builtin.gain", "1.0.0", "Gain",
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
}

const audioalgo::editor::PortVisualGeometry* find_port(
    const audioalgo::editor::NodeVisualGeometry& geometry, const std::string& id) {
    const auto found = std::find_if(geometry.ports.begin(), geometry.ports.end(), [&](const auto& port) {
        return port.id == id;
    });
    return found == geometry.ports.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE("NED-02.a", "[node_interaction][NED-02]") {
    const audioalgo::NodeInstance gain{"gain", "builtin.gain", {}, 10.0, 20.0};
    const auto descriptor = gain_descriptor();
    const auto geometry = audioalgo::editor::make_node_geometry(gain, &descriptor);
    const auto* input = find_port(geometry, "audio_in");

    REQUIRE(input != nullptr);
    REQUIRE_FALSE(geometry.summary.intersects(input->label_bounds));
}

TEST_CASE("NED-02.b", "[node_interaction][NED-02]") {
    const audioalgo::NodeInstance node{"multi", "test.multi", {}, 30.0, 40.0};
    const audioalgo::NodeDescriptor descriptor{
        "test.multi", "1.0.0", "Multi Port",
        {{"in_a", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"in_b", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"in_c", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"out_a", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"out_b", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    const auto geometry = audioalgo::editor::make_node_geometry(node, &descriptor);

    REQUIRE(geometry.title_bounds.width > 0.0f);
    REQUIRE(geometry.summary.width > 0.0f);
    REQUIRE_FALSE(geometry.title_bounds.intersects(geometry.summary));
    REQUIRE(geometry.ports.size() == descriptor.ports.size());
    for (const auto& port : geometry.ports) {
        REQUIRE(geometry.bounds.contains(port.anchor));
        REQUIRE(geometry.bounds.contains(port.label_bounds));
        REQUIRE_FALSE(geometry.title_bounds.intersects(port.label_bounds));
        REQUIRE_FALSE(geometry.summary.intersects(port.label_bounds));
    }
    for (std::size_t i = 0; i < geometry.ports.size(); ++i)
        for (std::size_t j = i + 1; j < geometry.ports.size(); ++j)
            REQUIRE_FALSE(geometry.ports[i].label_bounds.intersects(geometry.ports[j].label_bounds));
}

TEST_CASE("NED-02.d", "[node_interaction][NED-02]") {
    audioalgo::NodeRegistry registry;
    const audioalgo::NodeDescriptor descriptor{
        "test.multi_port_fixture", "1.0.0", "多输入输出节点",
        {{"input.alpha", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"input.beta", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"input.gamma", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"input.delta", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"input.epsilon", "audio.pcm@1", audioalgo::PortDirection::input, false},
         {"output.alpha", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"output.beta", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"output.gamma", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"output.delta", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    REQUIRE(registry.register_descriptor(descriptor));
    const audioalgo::NodeInstance node{"many-ports", descriptor.type_id, {}, 100.0, 80.0};
    const auto geometry = audioalgo::editor::make_node_geometry(node, &descriptor);
    REQUIRE(geometry.ports.size() == descriptor.ports.size());
    REQUIRE(geometry.bounds.height >= 168.0f);

    for (std::size_t index = 0; index < geometry.ports.size(); ++index) {
        const auto& visual = geometry.ports[index];
        REQUIRE(geometry.bounds.contains(visual.anchor));
        REQUIRE(geometry.bounds.contains(visual.label_bounds));
        const auto hit = audioalgo::editor::find_topmost_port_at({node}, registry, visual.anchor);
        REQUIRE(hit.has_value());
        REQUIRE(hit->node_id == node.id);
        REQUIRE(hit->port_id == visual.id);
        const audioalgo::editor::EditorPoint label_center{
            visual.label_bounds.x + visual.label_bounds.width * 0.5f,
            visual.label_bounds.y + visual.label_bounds.height * 0.5f};
        const auto label_hit = audioalgo::editor::find_topmost_port_at({node}, registry, label_center);
        REQUIRE(label_hit.has_value());
        REQUIRE(label_hit->port_id == visual.id);
        for (std::size_t other = index + 1; other < geometry.ports.size(); ++other)
            REQUIRE_FALSE(visual.label_bounds.intersects(geometry.ports[other].label_bounds));
    }
}

TEST_CASE("NED-02.c", "[node_interaction][NED-02]") {
    const auto default_window = audioalgo::editor::make_initial_editor_window_size(1440, 824);
    REQUIRE(default_window.width == 1280);
    REQUIRE(default_window.height == 800);

    const auto constrained_window = audioalgo::editor::make_initial_editor_window_size(1000, 700);
    REQUIRE(constrained_window.width == 976);
    REQUIRE(constrained_window.height == 676);

    for (const auto [width, height] : {std::pair{1100, 700}, std::pair{1280, 800}, std::pair{1440, 900}}) {
        const auto layout = audioalgo::editor::make_editor_component_layout(width, height);
        const auto visible = [&](const auto& bounds) {
            return bounds.x >= 0 && bounds.y >= 0 && bounds.x + bounds.width <= width &&
                   bounds.y + bounds.height <= height;
        };

        REQUIRE(visible(layout.new_button));
        REQUIRE(visible(layout.open_button));
        REQUIRE(visible(layout.save_button));
        REQUIRE(visible(layout.undo_button));
        REQUIRE(visible(layout.redo_button));
        REQUIRE(visible(layout.run_button));
        REQUIRE(visible(layout.play_button));
        REQUIRE(visible(layout.library_title));
        REQUIRE(visible(layout.library_viewport));
        REQUIRE(visible(layout.delete_button));
        REQUIRE(visible(layout.canvas));
        REQUIRE(visible(layout.inspector));
        REQUIRE(visible(layout.waveform));
        REQUIRE(visible(layout.status));
        REQUIRE(layout.canvas.width >= 180);
        REQUIRE(layout.canvas.height >= 160);
        REQUIRE(layout.canvas.x + layout.canvas.width <= layout.inspector.x);
    }
}

TEST_CASE("NED-05.a", "[node_interaction][NED-05]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    const auto* gain_descriptor = registry.find("builtin.gain");
    REQUIRE(gain_descriptor != nullptr);

    audioalgo::ProjectModel project;
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 60.0, 150.0},
        {"gain", "builtin.gain", {{"gain_db", 6.0}}, 340.0, 150.0},
        {"output", "builtin.output", {}, 620.0, 150.0},
    };

    constexpr float canvas_width = 992.0f;
    constexpr float canvas_height = 640.0f;
    for (int index = 0; index < 10; ++index) {
        const auto position = audioalgo::editor::find_node_placement(
            project.nodes, registry, *gain_descriptor, canvas_width, canvas_height);
        REQUIRE(position.has_value());
        const audioalgo::NodeInstance added{"added-gain-" + std::to_string(index), "builtin.gain",
                                            {{"gain_db", 0.0}}, position->x, position->y};
        const auto added_geometry = audioalgo::editor::make_node_geometry(added, gain_descriptor);
        REQUIRE(added_geometry.bounds.x >= 0.0f);
        REQUIRE(added_geometry.bounds.y >= 0.0f);
        REQUIRE(added_geometry.bounds.x + added_geometry.bounds.width <= canvas_width);
        REQUIRE(added_geometry.bounds.y + added_geometry.bounds.height <= canvas_height);
        for (const auto& existing : project.nodes) {
            const auto* existing_descriptor = registry.find(existing.type_id);
            const auto existing_geometry = audioalgo::editor::make_node_geometry(existing, existing_descriptor);
            REQUIRE_FALSE(added_geometry.bounds.intersects(existing_geometry.bounds));
        }
        project.nodes.push_back(added);
    }
    REQUIRE(project.nodes.size() == 13);

    const auto previous_count = project.nodes.size();
    const auto no_space = audioalgo::editor::find_node_placement(
        project.nodes, registry, *gain_descriptor, 180.0f, 120.0f);
    REQUIRE_FALSE(no_space.has_value());
    REQUIRE(project.nodes.size() == previous_count);
}

TEST_CASE("NED-05.b", "[node_interaction][NED-05]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    const auto* gain = registry.find("builtin.gain");
    REQUIRE(gain != nullptr);
    const auto gain_parameters = audioalgo::editor::default_node_parameters(*gain);
    REQUIRE(gain_parameters.size() == gain->parameters.size());
    for (const auto& parameter : gain->parameters)
        REQUIRE(gain_parameters.at(parameter.id) == parameter.default_value);

    REQUIRE(registry.register_descriptor({
        "test.described_filter", "1.0.0", "Described Filter",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {{"enabled", audioalgo::ParameterKind::boolean, true, {}, {}, {}},
         {"mix", audioalgo::ParameterKind::floating, 0.25, 0.0, 1.0, "ratio"},
         {"mode", audioalgo::ParameterKind::string, std::string("wide"), {}, {}, {}}},
        audioalgo::NodeExecutionKind::block}));
    const auto* filter = registry.find("test.described_filter");
    REQUIRE(filter != nullptr);
    const auto filter_parameters = audioalgo::editor::default_node_parameters(*filter);
    REQUIRE(filter_parameters.size() == 3);
    REQUIRE(filter_parameters.at("enabled") == audioalgo::ParameterValue{true});
    REQUIRE(filter_parameters.at("mix") == audioalgo::ParameterValue{0.25});
    REQUIRE(filter_parameters.at("mode") == audioalgo::ParameterValue{std::string("wide")});

    const std::vector<audioalgo::NodeInstance> existing{{"reused-id", "builtin.gain", gain_parameters, 1.0, 2.0}};
    const std::vector<std::string> generated_ids{"reused-id", "", "fresh-id"};
    std::size_t generated_count = 0;
    const auto unique_id = audioalgo::editor::make_unique_node_id(existing, [&] {
        return generated_ids.at(generated_count++);
    });
    REQUIRE(unique_id.has_value());
    REQUIRE(*unique_id == "fresh-id");
    REQUIRE(generated_count == 3);
}

TEST_CASE("NED-05.c", "[node_interaction][NED-05]") {
    audioalgo::ProjectModel project;
    project.nodes.push_back({"new-node-id", "builtin.gain", {{"gain_db", 0.0}}, 20.0, 30.0});
    audioalgo::editor::NodeSelectionState selection;

    REQUIRE(selection.select(project, "new-node-id"));
    REQUIRE(selection.selected_node_id() == "new-node-id");
    REQUIRE_FALSE(selection.select(project, "missing-node-id"));
    REQUIRE(selection.selected_node_id() == "new-node-id");
    REQUIRE(selection.select(project, {}));
    REQUIRE(selection.selected_node_id().empty());
}

TEST_CASE("NED-05.d", "[node_interaction][NED-05]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    REQUIRE(registry.register_descriptor({
        "test.described_filter", "1.0.0", "Described Filter",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {{"mix", audioalgo::ParameterKind::floating, 0.25, 0.0, 1.0, "ratio"},
         {"enabled", audioalgo::ParameterKind::boolean, true, {}, {}, {}}},
        audioalgo::NodeExecutionKind::block}));

    const auto library = audioalgo::editor::build_node_library_entries(registry);
    const auto entry = std::find_if(library.begin(), library.end(), [](const auto& item) {
        return item.type_id == "test.described_filter";
    });
    REQUIRE(entry != library.end());
    REQUIRE(entry->display_name == "Described Filter");
    REQUIRE(entry->execution_kind == audioalgo::NodeExecutionKind::block);
    REQUIRE(entry->default_parameters.size() == 2);
    REQUIRE(entry->default_parameters.at("mix") == audioalgo::ParameterValue{0.25});
    REQUIRE(entry->default_parameters.at("enabled") == audioalgo::ParameterValue{true});
}

TEST_CASE("NED-05.e", "[node_interaction][NED-05]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    audioalgo::ProjectModel project;
    project.revision = 41;
    project.nodes.push_back({"existing", "builtin.gain", {{"gain_db", 3.0}}, 10.0, 20.0});
    const auto previous_nodes = project.nodes;

    const auto candidate = audioalgo::editor::make_node_instance(
        registry, project.nodes, "missing.processing.node", "new-id", 100.0, 120.0);

    REQUIRE_FALSE(candidate.has_value());
    REQUIRE(project.nodes.size() == previous_nodes.size());
    REQUIRE(project.nodes.front().id == previous_nodes.front().id);
    REQUIRE(project.nodes.front().parameters == previous_nodes.front().parameters);
    REQUIRE(project.nodes.front().x == previous_nodes.front().x);
    REQUIRE(project.nodes.front().y == previous_nodes.front().y);
    REQUIRE(project.revision == 41);
}

TEST_CASE("NED-06.a", "[node_interaction][NED-06]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    const std::vector<audioalgo::NodeInstance> nodes{
        {"painted-first", "builtin.gain", {{"gain_db", 0.0}}, 100.0, 100.0},
        {"painted-last", "builtin.output", {}, 130.0, 120.0},
    };

    const auto* hit = audioalgo::editor::find_topmost_node_at(nodes, registry, {160.0f, 150.0f});

    REQUIRE(hit != nullptr);
    REQUIRE(hit->id == "painted-last");
}

TEST_CASE("NED-06.b", "[node_interaction][NED-06]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    audioalgo::ProjectModel project;
    project.nodes = {
        {"source", "builtin.file_source", {{"path", std::string("input.wav")}}, 20.0, 100.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 350.0, 100.0},
    };
    project.connections.push_back({"source", "audio", "gain", "audio_in"});
    constexpr audioalgo::editor::EditorPoint on_visible_edge{273.0f, 164.0f};

    const auto visible_edge = audioalgo::editor::find_visible_connection_at(project, registry, on_visible_edge);
    REQUIRE(visible_edge == 0);

    project.nodes.push_back({"occluding-node", "builtin.output", {}, 250.0, 130.0});
    const auto* topmost = audioalgo::editor::find_topmost_node_at(project.nodes, registry, on_visible_edge);
    REQUIRE(topmost != nullptr);
    REQUIRE(topmost->id == "occluding-node");
    REQUIRE_FALSE(audioalgo::editor::find_visible_connection_at(project, registry, on_visible_edge).has_value());
}

TEST_CASE("NED-06.c", "[node_interaction][NED-06]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    const std::vector<audioalgo::NodeInstance> nodes{
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 100.0, 100.0},
    };

    const auto input = audioalgo::editor::find_topmost_port_at(nodes, registry, {102.0f, 164.0f});
    REQUIRE(input.has_value());
    REQUIRE(input->node_id == "gain");
    REQUIRE(input->port_id == "audio_in");
    REQUIRE(input->direction == audioalgo::PortDirection::input);

    const auto output = audioalgo::editor::find_topmost_port_at(nodes, registry, {274.0f, 164.0f});
    REQUIRE(output.has_value());
    REQUIRE(output->port_id == "audio_out");
    REQUIRE(output->direction == audioalgo::PortDirection::output);
    SECTION("a protruding socket remains draggable from outside the node rectangle") {
        const auto output_outside_body = audioalgo::editor::find_topmost_port_at(
            nodes, registry, {286.0f, 164.0f});
        REQUIRE(output_outside_body.has_value());
        REQUIRE(output_outside_body->node_id == "gain");
        REQUIRE(output_outside_body->port_id == "audio_out");
    }
    REQUIRE_FALSE(audioalgo::editor::find_topmost_port_at(nodes, registry, {200.0f, 190.0f}).has_value());
}

TEST_CASE("NED-06.e", "[node_interaction][NED-06]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    audioalgo::editor::CanvasCoordinateTransform transform;
    transform.pan_by({35.0f, -18.0f});
    const audioalgo::editor::EditorPoint model_position{300.0f, 100.0f};
    const auto canvas_position = transform.model_to_canvas(model_position);
    REQUIRE(canvas_position.x == 335.0f);
    REQUIRE(canvas_position.y == 82.0f);
    const auto event_position = transform.event_to_canvas(canvas_position);
    const auto round_trip = transform.canvas_to_model(event_position);
    REQUIRE(round_trip.x == model_position.x);
    REQUIRE(round_trip.y == model_position.y);

    audioalgo::ProjectModel project;
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40.0, 100.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, model_position.x, model_position.y},
        {"output", "builtin.output", {}, 580.0, 100.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    const auto* gain_descriptor = registry.find("builtin.gain");
    REQUIRE(gain_descriptor != nullptr);
    const auto gain_geometry = audioalgo::editor::make_node_geometry(project.nodes[1], gain_descriptor, transform);
    REQUIRE(gain_geometry.bounds.x == canvas_position.x);
    REQUIRE(gain_geometry.bounds.y == canvas_position.y);
    const auto* hit_node = audioalgo::editor::find_topmost_node_at(project.nodes, registry, event_position, transform);
    REQUIRE(hit_node != nullptr);
    REQUIRE(hit_node->id == "gain");

    const auto input_anchor = audioalgo::editor::EditorPoint{canvas_position.x, canvas_position.y + 64.0f};
    const auto input_hit = audioalgo::editor::find_topmost_port_at(project.nodes, registry, input_anchor, transform);
    REQUIRE(input_hit.has_value());
    REQUIRE(input_hit->node_id == "gain");
    REQUIRE(audioalgo::editor::find_visible_connection_at(
        project, registry, {293.0f, 146.0f}, 8.0f, transform) == 0);

    const auto placement = audioalgo::editor::find_node_placement(
        project.nodes, registry, *gain_descriptor, 800.0f, 520.0f, transform);
    REQUIRE(placement.has_value());
    audioalgo::NodeInstance candidate{"candidate", "builtin.gain", {}, placement->x, placement->y};
    const auto placed_geometry = audioalgo::editor::make_node_geometry(candidate, gain_descriptor, transform);
    REQUIRE(placed_geometry.bounds.x >= 0.0f);
    REQUIRE(placed_geometry.bounds.y >= 0.0f);
    REQUIRE(placed_geometry.bounds.x + placed_geometry.bounds.width <= 800.0f);
    REQUIRE(placed_geometry.bounds.y + placed_geometry.bounds.height <= 520.0f);
}

TEST_CASE("NED-10.e", "[node_interaction][NED-10]") {
    audioalgo::NodeRegistry registry;
    audioalgo::register_builtin_nodes(registry);
    audioalgo::ProjectModel branched;
    branched.nodes = {
        {"file-near", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 10.0},
        {"file-far", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 18.0},
        {"gain-near", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 10.0},
        {"gain-far", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 18.0},
    };
    branched.connections = {
        {"file-near", "audio", "gain-near", "audio_in"},
        {"file-far", "audio", "gain-far", "audio_in"},
    };
    const auto near_curve = audioalgo::editor::make_connection_geometry(
        branched, registry, branched.connections[0]);
    const auto far_curve = audioalgo::editor::make_connection_geometry(
        branched, registry, branched.connections[1]);
    REQUIRE(near_curve.has_value());
    REQUIRE(far_curve.has_value());
    const audioalgo::editor::EditorPoint near_midpoint{
        (near_curve->start.x + 3.0f * near_curve->control1.x + 3.0f * near_curve->control2.x + near_curve->finish.x) / 8.0f,
        (near_curve->start.y + 3.0f * near_curve->control1.y + 3.0f * near_curve->control2.y + near_curve->finish.y) / 8.0f};
    const auto near_distance = audioalgo::editor::distance_to_connection(*near_curve, near_midpoint);
    const auto far_distance = audioalgo::editor::distance_to_connection(*far_curve, near_midpoint);
    REQUIRE(near_distance < far_distance);
    REQUIRE(far_distance <= 8.0f);
    REQUIRE(audioalgo::editor::find_visible_connection_at(branched, registry, near_midpoint, 8.0f) == 0);

    const audioalgo::editor::EditorPoint tie_point{near_midpoint.x, 78.0f};
    const auto near_tie_distance = audioalgo::editor::distance_to_connection(*near_curve, tie_point);
    const auto far_tie_distance = audioalgo::editor::distance_to_connection(*far_curve, tie_point);
    REQUIRE(std::abs(near_tie_distance - far_tie_distance) <= 0.05f);
    REQUIRE(audioalgo::editor::find_visible_connection_at(branched, registry, tie_point, 8.0f) == 0);

    audioalgo::ProjectModel flat;
    flat.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 10.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 10.0},
    };
    flat.connections = {{"file", "audio", "gain", "audio_in"}};
    REQUIRE(audioalgo::editor::find_visible_connection_at(flat, registry, {213.0f, 82.0f}, 8.0f) == 0);
    REQUIRE_FALSE(audioalgo::editor::find_visible_connection_at(flat, registry, {213.0f, 82.01f}, 8.0f));
    REQUIRE_FALSE(audioalgo::editor::find_visible_connection_at(flat, registry, {240.0f, 74.0f}, 8.0f));
}

