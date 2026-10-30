#include "EditorNodeParameterBinding.h"
#include "EditorParameterPanel.h"
#include "EditorNodeInspector.h"
#include "EditorGraphCanvas.h"
#include "EditorNodeLibrary.h"
#include "EditorWaveformView.h"
#include "EditorRunStatusPanel.h"
#include "../fixtures/ParameterProbeNode.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_run_service/RunService.h"
#include "plugins/builtin/registration.h"

#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/EditorProjectDocuments.h"
#include "../../apps/editor/EditorHostServices.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <variant>

namespace {

audioalgo::NodeDescriptor parameter_binding_descriptor() {
    audioalgo::ParameterDescriptor level{"level", audioalgo::ParameterKind::floating,
                                          0.0, 0.0, 10.0, "dB"};
    level.display_name = "Level";
    level.step = 0.5;
    return {"test.parameter_binding", "1.0.0", "Binding test node", {}, {std::move(level)},
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

double parameter_value(const audioalgo::ProjectModel& project, const std::string& node_id) {
    for (const auto& node : project.nodes) {
        if (node.id == node_id)
            return std::get<double>(node.parameters.at("level"));
    }
    return -999.0;
}

} // namespace

namespace {
juce::MouseEvent canvas_mouse(juce::Component& component, juce::Point<float> position,
                             int buttons, juce::Point<float> origin, bool dragged = false) {
    return {juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(buttons),
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component,
            juce::Time(100), origin, juce::Time(0), 1, dragged};
}

struct CanvasInteractionFixture {
    audioalgo::NodeRegistry registry;
    audioalgo::editor::EditorController controller{registry};
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector{controller, selection, [] {}};
    int layout_changes = 0;
    int selection_callback_count = 0;
    std::vector<std::string> selection_callback_ids;
    std::vector<std::string> errors;
    audioalgo::editor::GraphCanvas canvas{controller.project(), registry, selection, controller,
        [this](std::string id) {
            ++selection_callback_count;
            selection_callback_ids.push_back(id);
            inspector.show_node(id);
        }, [this] { ++layout_changes; },
        [this](std::string message) { errors.push_back(std::move(message)); }, {}, {}};

    CanvasInteractionFixture() {
        REQUIRE(audioalgo::register_builtin_nodes(registry));
        audioalgo::ProjectModel project;
        project.project_id = "canvas-interaction-regression";
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100},
                         {"gain", "builtin.gain", {{"gain_db", 0.0}}, 340, 100},
                         {"output", "builtin.output", {}, 640, 100}};
        project.connections = {{"file", "audio", "gain", "audio_in"},
                               {"gain", "audio_out", "output", "audio"}};
        project.selected_output_node_id = "output";
        controller.adopt_project(std::move(project));
        inspector.set_descriptor_registry(registry);
        canvas.setSize(1000, 600);
    }

    void drag(juce::Point<float> from, juce::Point<float> to, int button) {
        canvas.mouseDown(canvas_mouse(canvas, from, button, from));
        canvas.mouseDrag(canvas_mouse(canvas, to, button, from, true));
        canvas.mouseUp(canvas_mouse(canvas, to, 0, from, true));
    }
};

bool same_project_snapshot(const audioalgo::ProjectModel& actual,
                           const audioalgo::ProjectModel& expected) {
    if (actual.schema_version != expected.schema_version || actual.project_id != expected.project_id ||
        actual.name != expected.name || actual.revision != expected.revision ||
        actual.selected_output_node_id != expected.selected_output_node_id ||
        actual.selected_output_port_id != expected.selected_output_port_id ||
        actual.serialized_extensions != expected.serialized_extensions ||
        actual.nodes.size() != expected.nodes.size() || actual.connections.size() != expected.connections.size())
        return false;
    for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
        const auto& left = actual.nodes[index];
        const auto& right = expected.nodes[index];
        if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
            left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
            left.serialized_extensions != right.serialized_extensions || left.managed_version != right.managed_version)
            return false;
    }
    for (std::size_t index = 0; index < actual.connections.size(); ++index) {
        const auto& left = actual.connections[index];
        const auto& right = expected.connections[index];
        if (left.from_node != right.from_node || left.from_port != right.from_port ||
            left.to_node != right.to_node || left.to_port != right.to_port ||
            left.serialized_extensions != right.serialized_extensions)
            return false;
    }
    return true;
}

std::string canonical_json_fragment(const std::string& json) {
    const auto wrapped = std::string("{\"value\":") + json + "}";
    const auto wrapped_string = juce::String::fromUTF8(wrapped.c_str());
    const auto parsed = juce::JSON::parse(wrapped_string);
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr)
        return "<invalid-json>" + json;
    const auto compact_json = juce::JSON::toString(object->getProperty("value"), false);
    const auto compact = compact_json.toUTF8();
    return std::string(compact.getAddress(), static_cast<std::size_t>(compact.sizeInBytes() - 1));
}

bool same_json_extensions(const std::vector<audioalgo::OpaqueJsonField>& actual,
                          const std::vector<audioalgo::OpaqueJsonField>& expected) {
    if (actual.size() != expected.size())
        return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index].name != expected[index].name ||
            canonical_json_fragment(actual[index].value_json) != canonical_json_fragment(expected[index].value_json))
            return false;
    }
    return true;
}

bool same_persisted_project(const audioalgo::ProjectModel& actual,
                            const audioalgo::ProjectModel& expected) {
    if (actual.schema_version != expected.schema_version || actual.project_id != expected.project_id ||
        actual.name != expected.name || actual.revision != expected.revision ||
        actual.selected_output_node_id != expected.selected_output_node_id ||
        actual.selected_output_port_id != expected.selected_output_port_id ||
        !same_json_extensions(actual.serialized_extensions, expected.serialized_extensions) ||
        actual.nodes.size() != expected.nodes.size() || actual.connections.size() != expected.connections.size())
        return false;
    for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
        const auto& left = actual.nodes[index];
        const auto& right = expected.nodes[index];
        if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
            left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
            !same_json_extensions(left.serialized_extensions, right.serialized_extensions) ||
            left.managed_version.has_value() != right.managed_version.has_value())
            return false;
        if (left.managed_version) {
            const auto& left_lock = *left.managed_version;
            const auto& right_lock = *right.managed_version;
            if (left_lock.package_id != right_lock.package_id ||
                left_lock.content_sha256 != right_lock.content_sha256 ||
                left_lock.package_path != right_lock.package_path ||
                left_lock.wrapper_version != right_lock.wrapper_version ||
                !same_json_extensions(left_lock.serialized_extensions, right_lock.serialized_extensions))
                return false;
        }
    }
    for (std::size_t index = 0; index < actual.connections.size(); ++index) {
        const auto& left = actual.connections[index];
        const auto& right = expected.connections[index];
        if (left.from_node != right.from_node || left.from_port != right.from_port ||
            left.to_node != right.to_node || left.to_port != right.to_port ||
            !same_json_extensions(left.serialized_extensions, right.serialized_extensions))
            return false;
    }
    return true;
}

std::vector<char> read_project_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

template <typename T>
T* descendant(juce::Component& component, const juce::String& id) {
    if (component.getComponentID() == id)
        return dynamic_cast<T*>(&component);
    for (auto* child : component.getChildren())
        if (auto* found = descendant<T>(*child, id))
            return found;
    return nullptr;
}
}

TEST_CASE("NCE-01.b Inspector disconnects every incident edge in one undoable edit while preserving the node",
          "[NCE-01.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 40;
    initial.serialized_extensions = {{"project_extension", "{\"keep\":true}"}};
    for (auto& node : initial.nodes) {
        if (node.id != "gain")
            continue;
        node.opaque_parameters = {{"vendor_value", "{\"kept\":1}"}};
        node.serialized_extensions = {{"node_extension", "\"preserve me\""}};
        node.managed_version = audioalgo::NodeInstance::ManagedVersion{
            "example.package", "content-hash-v1", "packages/example/v1", "wrapper-v1", {}};
    }
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();
    fixture.inspector.setSize(300, 500);
    fixture.canvas.select("gain");
    REQUIRE(fixture.selection.selected_node_id() == "gain");
    fixture.inspector.show_node("gain");

    auto* disconnect_button = descendant<juce::TextButton>(
        fixture.inspector, "inspector.disconnect-all-connections");
    REQUIRE(disconnect_button != nullptr);
    CHECK(disconnect_button->getButtonText() == "Disconnect all connections");
    REQUIRE(disconnect_button->isEnabled());
    CHECK(fixture.controller.project().connections.size() == 2);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());

    const auto same_node = [](const audioalgo::NodeInstance& actual,
                              const audioalgo::NodeInstance& expected) {
        return actual.id == expected.id && actual.type_id == expected.type_id &&
               actual.parameters == expected.parameters && actual.x == expected.x && actual.y == expected.y &&
               actual.opaque_parameters == expected.opaque_parameters &&
               actual.serialized_extensions == expected.serialized_extensions &&
               actual.managed_version == expected.managed_version;
    };
    const auto same_connection = [](const audioalgo::Connection& actual,
                                   const audioalgo::Connection& expected) {
        return actual.from_node == expected.from_node && actual.from_port == expected.from_port &&
               actual.to_node == expected.to_node && actual.to_port == expected.to_port &&
               actual.serialized_extensions == expected.serialized_extensions;
    };
    const auto assert_nodes_preserved = [&](const auto& actual) {
        REQUIRE(actual.nodes.size() == initial.nodes.size());
        for (std::size_t index = 0; index < initial.nodes.size(); ++index)
            CHECK(same_node(actual.nodes[index], initial.nodes[index]));
    };
    const auto assert_original_connections = [&](const auto& actual) {
        REQUIRE(actual.connections.size() == initial.connections.size());
        for (std::size_t index = 0; index < initial.connections.size(); ++index)
            CHECK(same_connection(actual.connections[index], initial.connections[index]));
    };

    // The component has no native window in this component-level test; invoke the
    // same production Button callback that JUCE dispatches after a real click.
    disconnect_button->onClick();

    const auto& disconnected = fixture.controller.project();
    CHECK(disconnected.connections.empty());
    CHECK(disconnected.project_id == initial.project_id);
    CHECK(disconnected.name == initial.name);
    CHECK(disconnected.selected_output_node_id == initial.selected_output_node_id);
    CHECK(disconnected.selected_output_port_id == initial.selected_output_port_id);
    CHECK(disconnected.serialized_extensions == initial.serialized_extensions);
    assert_nodes_preserved(disconnected);
    CHECK(disconnected.revision == initial.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.history().redo_size() == 0);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(fixture.selection.selected_node_id() == "gain");
    CHECK_FALSE(disconnect_button->isEnabled());

    REQUIRE(fixture.controller.undo());
    const auto& undone = fixture.controller.project();
    assert_original_connections(undone);
    assert_nodes_preserved(undone);
    CHECK(undone.revision == initial.revision + 2);
    CHECK(fixture.controller.history().can_redo());

    REQUIRE(fixture.controller.redo());
    CHECK(fixture.controller.project().connections.empty());
    assert_nodes_preserved(fixture.controller.project());
    CHECK(fixture.controller.revision() == initial.revision + 3);
    CHECK_FALSE(fixture.controller.history().can_redo());


}

TEST_CASE("NCE-01.c Inspector disconnects the selected-node edge union in one undoable edit",
          "[NCE-01.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 70;
    initial.serialized_extensions = {{"project_extension", "{\"keep\":true}"}};
    for (auto& node : initial.nodes) {
        if (node.id == "file") {
            node.opaque_parameters = {{"resource_note", "\"keep file metadata\""}};
            node.serialized_extensions = {{"node_extension", "\"file\""}};
        } else if (node.id == "gain") {
            node.opaque_parameters = {{"vendor_value", "{\"kept\":1}"}};
            node.serialized_extensions = {{"node_extension", "\"gain\""}};
            node.managed_version = audioalgo::NodeInstance::ManagedVersion{
                "example.package", "content-hash-v1", "packages/example/v1", "wrapper-v1", {}};
        }
    }
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();
    fixture.inspector.setSize(300, 500);
    fixture.drag({20, 90}, {540, 220}, juce::ModifierKeys::leftButtonModifier);
    REQUIRE(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
    CHECK(fixture.selection.selected_node_id() == "file");

    auto* disconnect_button = descendant<juce::TextButton>(
        fixture.inspector, "inspector.disconnect-selected-node-connections");
    REQUIRE(disconnect_button != nullptr);
    CHECK(disconnect_button->getButtonText() == "Disconnect selected nodes");
    REQUIRE(disconnect_button->isEnabled());
    REQUIRE(fixture.controller.project().connections.size() == 2);

    const auto same_node = [](const audioalgo::NodeInstance& actual,
                              const audioalgo::NodeInstance& expected) {
        return actual.id == expected.id && actual.type_id == expected.type_id &&
               actual.parameters == expected.parameters && actual.x == expected.x && actual.y == expected.y &&
               actual.opaque_parameters == expected.opaque_parameters &&
               actual.serialized_extensions == expected.serialized_extensions &&
               actual.managed_version == expected.managed_version;
    };
    const auto assert_nodes_preserved = [&](const auto& actual) {
        REQUIRE(actual.nodes.size() == initial.nodes.size());
        for (std::size_t index = 0; index < initial.nodes.size(); ++index)
            CHECK(same_node(actual.nodes[index], initial.nodes[index]));
    };
    const auto assert_original_connections = [&](const auto& actual) {
        REQUIRE(actual.connections.size() == initial.connections.size());
        for (std::size_t index = 0; index < initial.connections.size(); ++index) {
            CHECK(actual.connections[index].from_node == initial.connections[index].from_node);
            CHECK(actual.connections[index].from_port == initial.connections[index].from_port);
            CHECK(actual.connections[index].to_node == initial.connections[index].to_node);
            CHECK(actual.connections[index].to_port == initial.connections[index].to_port);
        }
    };

    // The production marquee selected both endpoints of the File -> Gain edge.
    // Disconnecting the union must remove that interior edge once in one history edit.
    disconnect_button->onClick();
    CHECK(fixture.controller.project().connections.empty());
    assert_nodes_preserved(fixture.controller.project());
    CHECK(fixture.controller.project().project_id == initial.project_id);
    CHECK(fixture.controller.project().selected_output_node_id == initial.selected_output_node_id);
    CHECK(fixture.controller.project().selected_output_port_id == initial.selected_output_port_id);
    CHECK(fixture.controller.project().serialized_extensions == initial.serialized_extensions);
    CHECK(fixture.controller.revision() == initial.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.history().redo_size() == 0);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
    CHECK_FALSE(disconnect_button->isEnabled());

    REQUIRE(fixture.controller.undo());
    assert_original_connections(fixture.controller.project());
    assert_nodes_preserved(fixture.controller.project());
    CHECK(fixture.controller.revision() == initial.revision + 2);
    CHECK(fixture.controller.history().can_redo());

    REQUIRE(fixture.controller.redo());
    CHECK(fixture.controller.project().connections.empty());
    assert_nodes_preserved(fixture.controller.project());
    CHECK(fixture.controller.revision() == initial.revision + 3);
    CHECK_FALSE(fixture.controller.history().can_redo());

    audioalgo::editor::EditorController batch_controller(fixture.registry);
    batch_controller.adopt_project(initial);
    batch_controller.mark_result_current();
    const auto empty_selection = batch_controller.disconnect_node_connections(std::vector<std::string>{});
    CHECK(empty_selection.status == audioalgo::editor::ProjectEditStatus::unchanged);
    CHECK(batch_controller.revision() == initial.revision);
    CHECK(batch_controller.history().undo_size() == 0);
    CHECK(batch_controller.history().redo_size() == 0);
    CHECK_FALSE(batch_controller.dirty());
    CHECK_FALSE(batch_controller.results_stale());

    const auto missing_node = batch_controller.disconnect_node_connections(
        std::vector<std::string>{"file", "missing"});
    CHECK(missing_node.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(missing_node.diagnostics.empty());
    CHECK(missing_node.diagnostics.front().code == "node_missing");
    CHECK(batch_controller.revision() == initial.revision);
    CHECK(batch_controller.project().connections.size() == initial.connections.size());
    assert_nodes_preserved(batch_controller.project());
    CHECK(batch_controller.history().undo_size() == 0);
    CHECK(batch_controller.history().redo_size() == 0);
    CHECK_FALSE(batch_controller.dirty());
    CHECK_FALSE(batch_controller.results_stale());

    const auto duplicate_nodes = batch_controller.disconnect_node_connections(
        std::vector<std::string>{"file", "gain", "file", "gain"});
    CHECK(duplicate_nodes.status == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(batch_controller.project().connections.empty());
    assert_nodes_preserved(batch_controller.project());
    CHECK(batch_controller.revision() == initial.revision + 1);
    CHECK(batch_controller.history().undo_size() == 1);
    CHECK(batch_controller.history().redo_size() == 0);
    CHECK(batch_controller.dirty());
    CHECK(batch_controller.results_stale());

    REQUIRE(batch_controller.undo());
    CHECK(batch_controller.project().connections.size() == initial.connections.size());
    CHECK(batch_controller.revision() == initial.revision + 2);
    CHECK(batch_controller.history().redo_size() == 1);
    const auto empty_after_undo = batch_controller.disconnect_node_connections(std::vector<std::string>{});
    CHECK(empty_after_undo.status == audioalgo::editor::ProjectEditStatus::unchanged);
    CHECK(batch_controller.revision() == initial.revision + 2);
    CHECK(batch_controller.history().redo_size() == 1);
}

TEST_CASE("NCE-01.a Selected-edge disconnect menu removes only that edge and preserves Delete semantics",
          "[NCE-01.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 90;
    initial.serialized_extensions = {{"project_extension", "{\"keep\":true}"}};
    for (auto& node : initial.nodes) {
        if (node.id == "gain") {
            node.opaque_parameters = {{"vendor_value", "{\"keep\":true}"}};
            node.serialized_extensions = {{"node_extension", "\"gain\""}};
            node.managed_version = audioalgo::NodeInstance::ManagedVersion{
                "example.package", "content-hash-v1", "packages/example/v1", "wrapper-v1", {}};
        }
    }
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();
    fixture.canvas.select("output");
    std::size_t menu_requests = 0;
    std::string menu_label;
    std::function<void()> disconnect_action;
    fixture.canvas.set_connection_context_menu_handler(
        [&](std::string label, std::function<void()> action) {
            ++menu_requests;
            menu_label = std::move(label);
            disconnect_action = std::move(action);
        });

    const juce::Point<float> edge_point{278, 164};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, edge_point,
        juce::ModifierKeys::rightButtonModifier, edge_point));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, edge_point, 0, edge_point));
    if (menu_requests == 0) {
        FAIL("A right click on a connection did not request its disconnect menu.");
        return;
    }

    CHECK(menu_label == "Disconnect connection");
    REQUIRE(disconnect_action);
    CHECK(fixture.selection.selected_node_id() == "output");
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    CHECK(fixture.controller.history().undo_size() == 0);
    CHECK(fixture.layout_changes == 0);

    const auto same_node = [](const audioalgo::NodeInstance& actual,
                              const audioalgo::NodeInstance& expected) {
        return actual.id == expected.id && actual.type_id == expected.type_id &&
               actual.parameters == expected.parameters && actual.x == expected.x && actual.y == expected.y &&
               actual.opaque_parameters == expected.opaque_parameters &&
               actual.serialized_extensions == expected.serialized_extensions &&
               actual.managed_version == expected.managed_version;
    };
    const auto assert_nodes_preserved = [&] {
        REQUIRE(fixture.controller.project().nodes.size() == initial.nodes.size());
        for (std::size_t index = 0; index < initial.nodes.size(); ++index)
            CHECK(same_node(fixture.controller.project().nodes[index], initial.nodes[index]));
    };

    disconnect_action();
    REQUIRE(fixture.controller.project().connections.size() == 1);
    CHECK(fixture.controller.project().connections.front().from_node == "gain");
    CHECK(fixture.controller.project().connections.front().to_node == "output");
    assert_nodes_preserved();
    CHECK(fixture.controller.project().serialized_extensions == initial.serialized_extensions);
    CHECK(fixture.controller.revision() == initial.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(fixture.selection.selected_node_id() == "output");
    CHECK(fixture.layout_changes == 1);

    REQUIRE(fixture.controller.undo());
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision + 2);
    CHECK(fixture.controller.history().can_redo());
    REQUIRE(fixture.controller.redo());
    CHECK(fixture.controller.project().connections.size() == 1);
    CHECK(fixture.controller.revision() == initial.revision + 3);
    REQUIRE(fixture.controller.undo());

    auto expected_revision = initial.revision + 4;
    const auto remove_selected_edge_with_key = [&](int key) {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, edge_point,
            juce::ModifierKeys::leftButtonModifier, edge_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, edge_point, 0, edge_point));
        CHECK(fixture.canvas.keyPressed(juce::KeyPress(key)));
        CHECK(fixture.controller.project().connections.size() == 1);
        CHECK(fixture.controller.project().connections.front().from_node == "gain");
        CHECK(fixture.controller.project().revision == expected_revision + 1);
        CHECK(fixture.controller.project().nodes.size() == initial.nodes.size());
        assert_nodes_preserved();
        REQUIRE(fixture.controller.undo());
        CHECK(fixture.controller.project().connections.size() == initial.connections.size());
        expected_revision += 2;
    };
    remove_selected_edge_with_key(juce::KeyPress::deleteKey);
    remove_selected_edge_with_key(juce::KeyPress::backspaceKey);

    const auto before_blank_click = fixture.controller.project();
    const auto redo_before_blank_click = fixture.controller.history().redo_size();
    const juce::Point<float> blank{900, 500};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, blank,
        juce::ModifierKeys::rightButtonModifier, blank));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, blank, 0, blank));
    CHECK(menu_requests == 1);
    CHECK(fixture.controller.project().connections.size() == before_blank_click.connections.size());
    CHECK(fixture.controller.revision() == before_blank_click.revision);
    CHECK(fixture.controller.history().redo_size() == redo_before_blank_click);

    const auto before_right_drag = fixture.controller.project();
    fixture.drag(edge_point, {20, 90}, juce::ModifierKeys::rightButtonModifier);
    CHECK(menu_requests == 1);
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file"});
    CHECK(fixture.controller.project().connections.size() == before_right_drag.connections.size());
    CHECK(fixture.controller.revision() == before_right_drag.revision);
    CHECK(fixture.controller.history().redo_size() == redo_before_blank_click);
}

TEST_CASE("NCE-02.a Port menu lists exact endpoints and removes only the selected edge",
          "[NCE-02.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 120;
    initial.serialized_extensions = {{"project_extension", "{\"keep\":true}"}};
    initial.nodes.push_back({"file2", "builtin.file_source", {{"path", std::string("input2.wav")}}, 40, 320});
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    initial.nodes.push_back({"out2", "builtin.output", {}, 640, 320});
    initial.connections.push_back({"gain", "audio_out", "out2", "audio"});
    initial.connections.push_back({"file2", "audio", "mixer", "primary"});
    initial.connections.push_back({"gain", "audio_out", "mixer", "secondary"});
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();
    fixture.canvas.select("gain");

    std::size_t menu_requests = 0;
    std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> menu_items;
    fixture.canvas.set_port_connection_context_menu_handler(
        [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
            ++menu_requests;
            menu_items = std::move(items);
        });

    // The fixed point is the Gain.audio_out socket in this production fixture.
    const juce::Point<float> output_port{516, 164};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, output_port,
        juce::ModifierKeys::rightButtonModifier, output_port));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, output_port, 0, output_port));
    if (menu_requests == 0) {
        FAIL("A right click on a connected output port did not request its edge list.");
        return;
    }

    REQUIRE(menu_items.size() == 4);
    CHECK(menu_items[0].label == "gain:audio_out -> output:audio");
    CHECK(menu_items[1].label == "gain:audio_out -> out2:audio");
    CHECK(menu_items[2].label == "gain:audio_out -> mixer:secondary");
    CHECK(menu_items[3].label == "Disconnect all from gain:audio_out");
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    CHECK(fixture.controller.history().undo_size() == 0);
    CHECK(fixture.selection.selected_node_id() == "gain");

    menu_items[2].action();
    const auto& disconnected = fixture.controller.project();
    REQUIRE(disconnected.connections.size() == initial.connections.size() - 1);
    const auto has_connection = [&](const audioalgo::Connection& expected) {
        return std::any_of(disconnected.connections.begin(), disconnected.connections.end(), [&](const auto& actual) {
            return actual.from_node == expected.from_node && actual.from_port == expected.from_port &&
                   actual.to_node == expected.to_node && actual.to_port == expected.to_port;
        });
    };
    CHECK_FALSE(has_connection(initial.connections.back()));
    CHECK(has_connection(initial.connections[0])); // Gain's other input port remains connected.
    CHECK(has_connection(initial.connections[1])); // The selected output port's other branch remains.
    CHECK(has_connection(initial.connections[3])); // Mixer.primary remains when Mixer.secondary is disconnected.
    CHECK(has_connection(initial.connections[2])); // A second branch from Gain.audio_out remains.
    CHECK(disconnected.serialized_extensions == initial.serialized_extensions);
    CHECK(disconnected.selected_output_node_id == initial.selected_output_node_id);
    CHECK(disconnected.nodes.size() == initial.nodes.size());
    CHECK(disconnected.revision == initial.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.history().redo_size() == 0);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(fixture.selection.selected_node_id() == "gain");
    CHECK(fixture.layout_changes == 1);

    REQUIRE(fixture.controller.undo());
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision + 2);
    REQUIRE(fixture.controller.redo());
    CHECK(fixture.controller.project().connections.size() == initial.connections.size() - 1);
    CHECK(fixture.controller.revision() == initial.revision + 3);
}

TEST_CASE("NCE-02.b Multi-edge port menu disconnects all branches atomically and leaves empty port a no-op",
          "[NCE-02.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 160;
    initial.serialized_extensions = {{"project_extension", "{\"keep\":true}"}};
    initial.nodes.push_back({"file2", "builtin.file_source", {{"path", std::string("input2.wav")}}, 40, 320});
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    initial.nodes.push_back({"out2", "builtin.output", {}, 640, 320});
    initial.connections.push_back({"gain", "audio_out", "out2", "audio"});
    initial.connections.push_back({"file2", "audio", "mixer", "primary"});
    initial.connections.push_back({"gain", "audio_out", "mixer", "secondary"});
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();
    fixture.canvas.select("gain");

    std::size_t menu_requests = 0;
    std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> menu_items;
    fixture.canvas.set_port_connection_context_menu_handler(
        [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
            ++menu_requests;
            menu_items = std::move(items);
        });

    const juce::Point<float> gain_output{516, 164};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, gain_output,
        juce::ModifierKeys::rightButtonModifier, gain_output));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, gain_output, 0, gain_output));
    REQUIRE(menu_requests == 1);
    REQUIRE(menu_items.size() == 4);
    CHECK(menu_items[0].label == "gain:audio_out -> output:audio");
    CHECK(menu_items[1].label == "gain:audio_out -> out2:audio");
    CHECK(menu_items[2].label == "gain:audio_out -> mixer:secondary");
    CHECK(menu_items[3].label == "Disconnect all from gain:audio_out");
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    CHECK(fixture.controller.history().undo_size() == 0);

    menu_items[3].action();
    const auto& disconnected = fixture.controller.project();
    REQUIRE(disconnected.connections.size() == 2);
    CHECK(disconnected.connections[0].from_node == "file");
    CHECK(disconnected.connections[0].to_node == "gain");
    CHECK(disconnected.connections[1].from_node == "file2");
    CHECK(disconnected.connections[1].to_node == "mixer");
    CHECK(disconnected.connections[1].to_port == "primary");
    CHECK(disconnected.serialized_extensions == initial.serialized_extensions);
    CHECK(disconnected.selected_output_node_id == initial.selected_output_node_id);
    CHECK(disconnected.nodes.size() == initial.nodes.size());
    CHECK(disconnected.revision == initial.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.history().redo_size() == 0);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(fixture.selection.selected_node_id() == "gain");
    CHECK(fixture.layout_changes == 1);

    REQUIRE(fixture.controller.undo());
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision + 2);
    CHECK(fixture.controller.history().redo_size() == 1);

    // Mixer.sum has no edges. Its right-click is a no-op and must preserve redo.
    const bool dirty_before_empty_port = fixture.controller.dirty();
    const bool stale_before_empty_port = fixture.controller.results_stale();
    const juce::Point<float> empty_port{516, 384};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, empty_port,
        juce::ModifierKeys::rightButtonModifier, empty_port));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, empty_port, 0, empty_port));
    CHECK(menu_requests == 1);
    CHECK(fixture.controller.project().connections.size() == initial.connections.size());
    CHECK(fixture.controller.revision() == initial.revision + 2);
    CHECK(fixture.controller.history().redo_size() == 1);
    CHECK(fixture.controller.dirty() == dirty_before_empty_port);
    CHECK(fixture.controller.results_stale() == stale_before_empty_port);
}

TEST_CASE("NCE-02.c Invalid or stale port menu actions reject exact identities without editing the replacement project",
          "[NCE-02.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    const auto initial = fixture.controller.project();

    const auto missing_node = fixture.controller.disconnect_port_connections("absent", "audio_out");
    REQUIRE(missing_node.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(missing_node.diagnostics.empty());
    CHECK(missing_node.diagnostics.front().code == "node_missing");
    CHECK(missing_node.diagnostics.front().node_id == "absent");
    CHECK(missing_node.diagnostics.front().port_id == "audio_out");

    const auto missing_port = fixture.controller.disconnect_port_connections("gain", "absent");
    REQUIRE(missing_port.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(missing_port.diagnostics.empty());
    CHECK(missing_port.diagnostics.front().code == "port_missing");
    CHECK(missing_port.diagnostics.front().node_id == "gain");
    CHECK(missing_port.diagnostics.front().port_id == "absent");

    const audioalgo::Connection missing_connection{"gain", "audio_out", "absent", "audio"};
    const auto missing_edge = fixture.controller.disconnect(missing_connection);
    REQUIRE(missing_edge.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(missing_edge.diagnostics.empty());
    CHECK(missing_edge.diagnostics.front().code == "connection_missing");
    CHECK(missing_edge.diagnostics.front().node_id == "absent");
    CHECK(missing_edge.diagnostics.front().port_id == "audio");

    auto duplicate_edge_project = initial;
    duplicate_edge_project.connections.push_back(initial.connections.back());
    fixture.controller.adopt_project(duplicate_edge_project);
    const auto before_ambiguous_edge = fixture.controller.project();
    const auto ambiguous_edge = fixture.controller.disconnect(initial.connections.back());
    REQUIRE(ambiguous_edge.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(ambiguous_edge.diagnostics.empty());
    CHECK(ambiguous_edge.diagnostics.front().code == "connection_ambiguous");
    CHECK(ambiguous_edge.diagnostics.front().node_id == "output");
    CHECK(ambiguous_edge.diagnostics.front().port_id == "audio");
    CHECK(fixture.controller.project().connections.size() == before_ambiguous_edge.connections.size());
    CHECK(fixture.controller.revision() == before_ambiguous_edge.revision);

    auto duplicate_node_project = initial;
    duplicate_node_project.nodes.push_back(initial.nodes[1]);
    fixture.controller.adopt_project(duplicate_node_project);
    const auto before_ambiguous_node = fixture.controller.project();
    const auto ambiguous_node = fixture.controller.disconnect_port_connections("gain", "audio_out");
    REQUIRE(ambiguous_node.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(ambiguous_node.diagnostics.empty());
    CHECK(ambiguous_node.diagnostics.front().code == "node_id_ambiguous");
    CHECK(ambiguous_node.diagnostics.front().node_id == "gain");
    CHECK(ambiguous_node.diagnostics.front().port_id == "audio_out");
    CHECK(fixture.controller.project().connections.size() == before_ambiguous_node.connections.size());
    CHECK(fixture.controller.revision() == before_ambiguous_node.revision);

    class DuplicatePortCatalog final : public audioalgo::NodeCatalog {
    public:
        DuplicatePortCatalog() {
            audioalgo::NodeDescriptor descriptor;
            descriptor.type_id = "test.duplicate_ports";
            descriptor.version = "1.0.0";
            descriptor.display_name = "Duplicate ports";
            descriptor.ports = {
                {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false},
                {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false},
            };
            descriptors_.emplace(descriptor.type_id, std::move(descriptor));
        }

        const audioalgo::NodeDescriptor* find(const std::string& type_id) const noexcept override {
            const auto found = descriptors_.find(type_id);
            return found == descriptors_.end() ? nullptr : &found->second;
        }

        const std::map<std::string, audioalgo::NodeDescriptor, std::less<>>& descriptors() const noexcept override {
            return descriptors_;
        }

    private:
        std::map<std::string, audioalgo::NodeDescriptor, std::less<>> descriptors_;
    } duplicate_port_catalog;
    audioalgo::editor::EditorController duplicate_port_controller(duplicate_port_catalog);
    audioalgo::ProjectModel duplicate_port_project;
    duplicate_port_project.project_id = "duplicate-port-contract";
    duplicate_port_project.nodes.push_back({"node", "test.duplicate_ports", {}, 0, 0});
    duplicate_port_controller.adopt_project(duplicate_port_project);
    const auto ambiguous_port = duplicate_port_controller.disconnect_port_connections("node", "audio_out");
    REQUIRE(ambiguous_port.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(ambiguous_port.diagnostics.empty());
    CHECK(ambiguous_port.diagnostics.front().code == "port_ambiguous");
    CHECK(ambiguous_port.diagnostics.front().node_id == "node");
    CHECK(ambiguous_port.diagnostics.front().port_id == "audio_out");

    auto context_project = initial;
    context_project.nodes.push_back({"out2", "builtin.output", {}, 800, 100});
    fixture.controller.adopt_project(context_project);
    std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> menu_items;
    fixture.canvas.set_port_connection_context_menu_handler(
        [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
            menu_items = std::move(items);
        });
    const auto open_menu = [&](juce::Point<float> port) {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, port,
            juce::ModifierKeys::rightButtonModifier, port));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, port, 0, port));
    };

    const juce::Point<float> gain_output{516, 164};
    open_menu(gain_output);
    REQUIRE(menu_items.size() == 1);
    const auto stale_after_edit = menu_items.front().action;
    REQUIRE(fixture.controller.connect({"gain", "audio_out", "out2", "audio"}).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    const auto after_connection_edit = fixture.controller.project();
    stale_after_edit();
    CHECK(fixture.controller.project().connections.size() == context_project.connections.size() + 1);
    CHECK(fixture.controller.revision() == after_connection_edit.revision);
    CHECK(fixture.controller.project().connections.back().to_node == "out2");
    REQUIRE_FALSE(fixture.errors.empty());
    CHECK(fixture.errors.back().find("gain:audio_out -> output:audio") != std::string::npos);

    menu_items.clear();
    open_menu(gain_output);
    REQUIRE(menu_items.size() == 3);
    const auto stale_after_project_switch = menu_items.front().action;
    auto replacement_project = fixture.controller.project();
    replacement_project.project_id = "replacement-project";
    fixture.controller.adopt_project(replacement_project);
    fixture.canvas.set_project_epoch(99);
    const auto replacement_before_callback = fixture.controller.project();
    stale_after_project_switch();
    CHECK(fixture.controller.project().project_id == "replacement-project");
    CHECK(fixture.controller.project().connections.size() == replacement_before_callback.connections.size());
    CHECK(fixture.controller.revision() == replacement_before_callback.revision);
    REQUIRE(fixture.errors.size() >= 2);
    CHECK(fixture.errors.back().find("gain:audio_out -> output:audio") != std::string::npos);
}

TEST_CASE("NCE-03.a Output drag and output-click then input-click create the same canonical edge",
          "[NCE-03.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    fixture.controller.adopt_project(initial);

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto output_anchor = port_anchor("file", "audio");
    const auto input_anchor = port_anchor("mixer", "primary");
    REQUIRE(output_anchor.has_value());
    REQUIRE(input_anchor.has_value());
    const auto output = *output_anchor;
    const auto input = *input_anchor;
    const audioalgo::Connection expected{"file", "audio", "mixer", "primary"};
    const auto contains_expected = [&](const audioalgo::ProjectModel& project) {
        return std::any_of(project.connections.begin(), project.connections.end(), [&](const auto& actual) {
            return actual.from_node == expected.from_node && actual.from_port == expected.from_port &&
                   actual.to_node == expected.to_node && actual.to_port == expected.to_port;
        });
    };
    const auto positions_unchanged = [&](const audioalgo::ProjectModel& project) {
        if (project.nodes.size() != initial.nodes.size())
            return false;
        for (std::size_t index = 0; index < initial.nodes.size(); ++index)
            if (project.nodes[index].id != initial.nodes[index].id ||
                project.nodes[index].x != initial.nodes[index].x ||
                project.nodes[index].y != initial.nodes[index].y)
                return false;
        return true;
    };

    const auto before_drag = fixture.controller.project();
    fixture.drag(output, input, juce::ModifierKeys::leftButtonModifier);
    const auto after_drag = fixture.controller.project();
    CHECK(after_drag.connections.size() == before_drag.connections.size() + 1);
    CHECK(contains_expected(after_drag));
    CHECK(positions_unchanged(after_drag));
    CHECK(after_drag.revision == before_drag.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);

    fixture.controller.adopt_project(initial);
    const auto before_output_click = fixture.controller.project();
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, output,
        juce::ModifierKeys::leftButtonModifier, output));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, output, 0, output));
    CHECK(fixture.controller.project().connections.size() == before_output_click.connections.size());
    CHECK(fixture.controller.revision() == before_output_click.revision);
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, input,
        juce::ModifierKeys::leftButtonModifier, input));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, input, 0, input));
    const auto after_clicks = fixture.controller.project();
    CHECK(after_clicks.connections.size() == before_output_click.connections.size() + 1);
    CHECK(contains_expected(after_clicks));
    CHECK(positions_unchanged(after_clicks));
    CHECK(after_clicks.revision == before_output_click.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(after_clicks.connections.back().from_node == expected.from_node);
    CHECK(after_clicks.connections.back().from_port == expected.from_port);
    CHECK(after_clicks.connections.back().to_node == expected.to_node);
    CHECK(after_clicks.connections.back().to_port == expected.to_port);
}

TEST_CASE("NCE-03.b Dragging from an idle input connects the exact named output",
          "[NCE-03.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    audioalgo::NodeDescriptor source_descriptor;
    source_descriptor.type_id = "test.multi_output_source";
    source_descriptor.version = "1.0.0";
    source_descriptor.display_name = "Multi output source";
    source_descriptor.ports = {
        {"left_out", "audio.pcm@1", audioalgo::PortDirection::output, false},
        {"right_out", "audio.pcm@1", audioalgo::PortDirection::output, false},
    };
    source_descriptor.execution_kind = audioalgo::NodeExecutionKind::source;
    REQUIRE(fixture.registry.register_descriptor(source_descriptor));

    auto initial = fixture.controller.project();
    initial.revision = 340;
    initial.nodes.push_back({"source", source_descriptor.type_id, {}, 40, 320});
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    fixture.controller.adopt_project(initial);

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto input_anchor = port_anchor("mixer", "primary");
    const auto left_output_anchor = port_anchor("source", "left_out");
    const auto right_output_anchor = port_anchor("source", "right_out");
    REQUIRE(input_anchor.has_value());
    REQUIRE(left_output_anchor.has_value());
    REQUIRE(right_output_anchor.has_value());

    const auto drag_from_input = [&](juce::Point<float> output_anchor, const std::string& output_port) {
        const auto before = fixture.controller.project();
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *input_anchor,
            juce::ModifierKeys::leftButtonModifier, *input_anchor));
        CHECK(fixture.controller.project().connections.size() == before.connections.size());
        CHECK(fixture.controller.revision() == before.revision);
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, output_anchor,
            juce::ModifierKeys::leftButtonModifier, *input_anchor, true));
        CHECK(fixture.controller.project().connections.size() == before.connections.size());
        CHECK(fixture.controller.revision() == before.revision);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, output_anchor, 0, *input_anchor, true));
        const auto after = fixture.controller.project();
        CHECK(after.connections.size() == before.connections.size() + 1);
        CHECK(after.revision == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(after.nodes.size() == before.nodes.size());
        for (std::size_t index = 0; index < before.nodes.size(); ++index) {
            CHECK(after.nodes[index].x == before.nodes[index].x);
            CHECK(after.nodes[index].y == before.nodes[index].y);
        }
        REQUIRE(after.connections.size() == before.connections.size() + 1);
        CHECK(after.connections.back().from_node == "source");
        CHECK(after.connections.back().from_port == output_port);
        CHECK(after.connections.back().to_node == "mixer");
        CHECK(after.connections.back().to_port == "primary");
    };

    drag_from_input(*right_output_anchor, "right_out");
    fixture.controller.adopt_project(initial);
    drag_from_input(*left_output_anchor, "left_out");
}

TEST_CASE("NCE-03.c Connected output creates a branch and rejects duplicate or occupied targets",
          "[NCE-03.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    initial.nodes.push_back({"file-2", "builtin.file_source", {{"path", std::string("second.wav")}}, 40, 360});
    fixture.controller.adopt_project(initial);

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto file_output = port_anchor("file", "audio");
    const auto file2_output = port_anchor("file-2", "audio");
    const auto mixer_input = port_anchor("mixer", "primary");
    const auto occupied_input = port_anchor("gain", "audio_in");
    REQUIRE(file_output.has_value());
    REQUIRE(file2_output.has_value());
    REQUIRE(mixer_input.has_value());
    REQUIRE(occupied_input.has_value());

    const auto same_edge = [](const audioalgo::Connection& left, const audioalgo::Connection& right) {
        return left.from_node == right.from_node && left.from_port == right.from_port &&
               left.to_node == right.to_node && left.to_port == right.to_port;
    };
    const auto same_connections = [&](const auto& actual, const auto& expected) {
        return actual.size() == expected.size() &&
               std::equal(actual.begin(), actual.end(), expected.begin(), same_edge);
    };
    const auto same_node_positions = [](const auto& actual, const auto& expected) {
        if (actual.size() != expected.size())
            return false;
        for (std::size_t index = 0; index < actual.size(); ++index)
            if (actual[index].id != expected[index].id || actual[index].x != expected[index].x ||
                actual[index].y != expected[index].y)
                return false;
        return true;
    };
    const auto drag_connection = [&](juce::Point<float> from, juce::Point<float> to) {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, from,
            juce::ModifierKeys::leftButtonModifier, from));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, to,
            juce::ModifierKeys::leftButtonModifier, from, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, to, 0, from, true));
    };

    const auto before_branch = fixture.controller.project();
    REQUIRE(before_branch.connections.size() == 2);
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *file_output,
        juce::ModifierKeys::leftButtonModifier, *file_output));
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *mixer_input,
        juce::ModifierKeys::leftButtonModifier, *file_output, true));
    CHECK(same_connections(fixture.controller.project().connections, before_branch.connections));
    CHECK(fixture.controller.revision() == before_branch.revision);
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *mixer_input, 0, *file_output, true));

    const auto after_branch = fixture.controller.project();
    REQUIRE(after_branch.connections.size() == before_branch.connections.size() + 1);
    CHECK(same_edge(after_branch.connections[0], before_branch.connections[0]));
    CHECK(same_edge(after_branch.connections[1], before_branch.connections[1]));
    CHECK(same_edge(after_branch.connections.back(), {"file", "audio", "mixer", "primary"}));
    CHECK(after_branch.revision == before_branch.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(same_node_positions(after_branch.nodes, before_branch.nodes));
    CHECK(fixture.layout_changes == 1);

    const auto before_duplicate = fixture.controller.project();
    const auto duplicate_undo = fixture.controller.history().undo_size();
    const auto duplicate_redo = fixture.controller.history().redo_size();
    const auto duplicate_errors = fixture.errors.size();
    drag_connection(*file_output, *occupied_input);
    CHECK(same_connections(fixture.controller.project().connections, before_duplicate.connections));
    CHECK(fixture.controller.revision() == before_duplicate.revision);
    CHECK(fixture.controller.history().undo_size() == duplicate_undo);
    CHECK(fixture.controller.history().redo_size() == duplicate_redo);
    CHECK(fixture.layout_changes == 1);
    REQUIRE(fixture.errors.size() == duplicate_errors + 1);
    CHECK(fixture.errors.back().find("connection_duplicate") != std::string::npos);

    const auto before_occupied = fixture.controller.project();
    const auto occupied_undo = fixture.controller.history().undo_size();
    const auto occupied_redo = fixture.controller.history().redo_size();
    const auto occupied_errors = fixture.errors.size();
    drag_connection(*file2_output, *occupied_input);
    CHECK(same_connections(fixture.controller.project().connections, before_occupied.connections));
    CHECK(fixture.controller.revision() == before_occupied.revision);
    CHECK(fixture.controller.history().undo_size() == occupied_undo);
    CHECK(fixture.controller.history().redo_size() == occupied_redo);
    CHECK(same_node_positions(fixture.controller.project().nodes, before_occupied.nodes));
    CHECK(fixture.layout_changes == 1);
    REQUIRE(fixture.errors.size() == occupied_errors + 1);
    CHECK(fixture.errors.back().find("input_connection_duplicate") != std::string::npos);
}

TEST_CASE("NCE-04.a Dragging a connected input changes only its target on release",
          "[NCE-04.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 71;
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto connected_input = port_anchor("gain", "audio_in");
    const auto idle_input = port_anchor("mixer", "primary");
    REQUIRE(connected_input.has_value());
    REQUIRE(idle_input.has_value());

    const auto same_edge = [](const audioalgo::Connection& left, const audioalgo::Connection& right) {
        return left.from_node == right.from_node && left.from_port == right.from_port &&
               left.to_node == right.to_node && left.to_port == right.to_port;
    };
    const auto same_connections = [&](const auto& actual, const auto& expected) {
        return actual.size() == expected.size() &&
               std::equal(actual.begin(), actual.end(), expected.begin(), same_edge);
    };
    const auto same_nodes = [](const auto& actual, const auto& expected) {
        if (actual.size() != expected.size())
            return false;
        for (std::size_t index = 0; index < actual.size(); ++index)
            if (actual[index].id != expected[index].id || actual[index].type_id != expected[index].type_id ||
                actual[index].parameters != expected[index].parameters ||
                actual[index].x != expected[index].x || actual[index].y != expected[index].y ||
                actual[index].opaque_parameters != expected[index].opaque_parameters ||
                actual[index].serialized_extensions != expected[index].serialized_extensions ||
                actual[index].managed_version != expected[index].managed_version)
                return false;
        return true;
    };
    const auto same_model_snapshot = [&](const audioalgo::ProjectModel& actual,
                                         const audioalgo::ProjectModel& expected) {
        return actual.project_id == expected.project_id && actual.revision == expected.revision &&
               actual.selected_output_node_id == expected.selected_output_node_id &&
               actual.serialized_extensions == expected.serialized_extensions &&
               same_nodes(actual.nodes, expected.nodes) && same_connections(actual.connections, expected.connections);
    };

    const auto before = fixture.controller.project();
    REQUIRE(before.connections.size() == 2);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    const auto undo_before = fixture.controller.history().undo_size();
    const auto redo_before = fixture.controller.history().redo_size();
    const auto layout_changes_before = fixture.layout_changes;

    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *connected_input,
        juce::ModifierKeys::leftButtonModifier, *connected_input));
    CHECK(same_model_snapshot(fixture.controller.project(), before));
    CHECK(fixture.controller.history().undo_size() == undo_before);
    CHECK(fixture.controller.history().redo_size() == redo_before);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *idle_input,
        juce::ModifierKeys::leftButtonModifier, *connected_input, true));
    CHECK(same_model_snapshot(fixture.controller.project(), before));
    CHECK(fixture.controller.history().undo_size() == undo_before);
    CHECK(fixture.controller.history().redo_size() == redo_before);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK_FALSE(fixture.controller.results_stale());
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *idle_input, 0, *connected_input, true));

    const auto after = fixture.controller.project();
    const bool changed_to_expected_target =
        std::find_if(after.connections.begin(), after.connections.end(), [&](const auto& edge) {
            return same_edge(edge, {"file", "audio", "mixer", "primary"});
        }) != after.connections.end();
    CHECK(after.connections.size() == before.connections.size());
    CHECK(changed_to_expected_target);
    CHECK(std::none_of(after.connections.begin(), after.connections.end(), [&](const auto& edge) {
        return same_edge(edge, {"file", "audio", "gain", "audio_in"});
    }));
    CHECK(std::any_of(after.connections.begin(), after.connections.end(), [&](const auto& edge) {
        return same_edge(edge, {"gain", "audio_out", "output", "audio"});
    }));
    CHECK(after.revision == before.revision + 1);
    CHECK(fixture.controller.history().undo_size() == undo_before + 1);
    CHECK(fixture.controller.history().redo_size() == redo_before);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(same_nodes(after.nodes, before.nodes));
    CHECK(fixture.layout_changes == layout_changes_before + 1);

    if (changed_to_expected_target) {
        CHECK(fixture.controller.undo());
        CHECK(same_connections(fixture.controller.project().connections, before.connections));
        CHECK(fixture.controller.project().revision == before.revision + 2);
        CHECK(fixture.controller.history().undo_size() == undo_before);
        CHECK(fixture.controller.history().redo_size() == redo_before + 1);
        CHECK(fixture.controller.redo());
        CHECK(std::any_of(fixture.controller.project().connections.begin(),
                          fixture.controller.project().connections.end(), [&](const auto& edge) {
            return same_edge(edge, {"file", "audio", "mixer", "primary"});
        }));
        CHECK(fixture.controller.project().revision == before.revision + 3);
    }
}

TEST_CASE("NCE-04.b Selected edge source handle reconnects one branch while output sockets still branch",
          "[NCE-04.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto initial = fixture.controller.project();
    initial.revision = 83;
    initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
    initial.nodes.push_back({"mixer-2", "builtin.mixer", {}, 340, 470});
    initial.nodes.push_back({"file-2", "builtin.file_source", {{"path", std::string("second.wav")}}, 40, 360});
    initial.connections.push_back({"file", "audio", "mixer", "primary"});
    fixture.controller.adopt_project(initial);
    fixture.controller.mark_result_current();

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto source_handle = [&] {
        const auto geometry = audioalgo::editor::make_source_endpoint_handle_geometry(
            fixture.controller.project(), fixture.registry, fixture.controller.project().connections.front(),
            fixture.canvas.coordinate_transform());
        if (!geometry)
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{
            juce::Point<float>{geometry->center.x, geometry->center.y}};
    };
    const auto source_to_gain_geometry = audioalgo::editor::make_connection_geometry(
        fixture.controller.project(), fixture.registry, fixture.controller.project().connections.front(),
        fixture.canvas.coordinate_transform());
    const auto old_source_handle = source_handle();
    const auto new_source_output = port_anchor("file-2", "audio");
    const auto ordinary_output = port_anchor("file", "audio");
    const auto mixer2_input = port_anchor("mixer-2", "primary");
    REQUIRE(source_to_gain_geometry.has_value());
    REQUIRE(old_source_handle.has_value());
    REQUIRE(new_source_output.has_value());
    REQUIRE(ordinary_output.has_value());
    REQUIRE(mixer2_input.has_value());
    CHECK_FALSE(audioalgo::editor::find_topmost_port_at(
        fixture.controller.project().nodes, fixture.registry,
        {old_source_handle->x, old_source_handle->y}, fixture.canvas.coordinate_transform()).has_value());
    CHECK(audioalgo::editor::find_topmost_node_at(
        fixture.controller.project().nodes, fixture.registry,
        {old_source_handle->x, old_source_handle->y}, fixture.canvas.coordinate_transform()) == nullptr);

    const auto same_edge = [](const audioalgo::Connection& left, const audioalgo::Connection& right) {
        return left.from_node == right.from_node && left.from_port == right.from_port &&
               left.to_node == right.to_node && left.to_port == right.to_port;
    };
    const auto same_connections = [&](const auto& actual, const auto& expected) {
        return actual.size() == expected.size() &&
               std::equal(actual.begin(), actual.end(), expected.begin(), same_edge);
    };
    const auto same_positions = [](const auto& actual, const auto& expected) {
        if (actual.size() != expected.size())
            return false;
        for (std::size_t index = 0; index < actual.size(); ++index)
            if (actual[index].id != expected[index].id || actual[index].x != expected[index].x ||
                actual[index].y != expected[index].y)
                return false;
        return true;
    };
    const auto bezier_midpoint = [](const audioalgo::editor::CubicConnectionGeometry& curve) {
        constexpr float t = 0.5f;
        constexpr float u = 1.0f - t;
        const auto coordinate = [&](float p0, float p1, float p2, float p3) {
            return u * u * u * p0 + 3.0f * u * u * t * p1 +
                   3.0f * u * t * t * p2 + t * t * t * p3;
        };
        return juce::Point<float>{coordinate(curve.start.x, curve.control1.x, curve.control2.x, curve.finish.x),
                                  coordinate(curve.start.y, curve.control1.y, curve.control2.y, curve.finish.y)};
    };

    const auto before_selection = fixture.controller.project();
    REQUIRE(before_selection.connections.size() == 3);
    const auto selected_edge = before_selection.connections.front();
    const auto original_branch = before_selection.connections.back();
    const auto select_point = bezier_midpoint(*source_to_gain_geometry);
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, select_point,
        juce::ModifierKeys::leftButtonModifier, select_point));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, select_point, 0, select_point));
    CHECK(same_connections(fixture.controller.project().connections, before_selection.connections));
    CHECK(fixture.controller.revision() == before_selection.revision);
    CHECK(fixture.controller.history().undo_size() == 0);

    const auto before_reconnect = fixture.controller.project();
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *old_source_handle,
        juce::ModifierKeys::leftButtonModifier, *old_source_handle));
    CHECK(same_connections(fixture.controller.project().connections, before_reconnect.connections));
    CHECK(fixture.controller.revision() == before_reconnect.revision);
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *new_source_output,
        juce::ModifierKeys::leftButtonModifier, *old_source_handle, true));
    CHECK(same_connections(fixture.controller.project().connections, before_reconnect.connections));
    CHECK(fixture.controller.revision() == before_reconnect.revision);
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *new_source_output, 0, *old_source_handle, true));

    const auto after_reconnect = fixture.controller.project();
    CHECK(after_reconnect.connections.size() == before_reconnect.connections.size());
    CHECK(same_edge(after_reconnect.connections.front(), {"file-2", "audio", "gain", "audio_in"}));
    CHECK(same_edge(after_reconnect.connections[1], before_reconnect.connections[1]));
    CHECK(same_edge(after_reconnect.connections[2], original_branch));
    CHECK_FALSE(same_edge(after_reconnect.connections.front(), selected_edge));
    CHECK(after_reconnect.revision == before_reconnect.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.controller.dirty());
    CHECK(fixture.controller.results_stale());
    CHECK(same_positions(after_reconnect.nodes, before_reconnect.nodes));

    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *ordinary_output,
        juce::ModifierKeys::leftButtonModifier, *ordinary_output));
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *mixer2_input,
        juce::ModifierKeys::leftButtonModifier, *ordinary_output, true));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *mixer2_input, 0, *ordinary_output, true));
    const auto after_branch = fixture.controller.project();
    CHECK(after_branch.connections.size() == after_reconnect.connections.size() + 1);
    CHECK(same_edge(after_branch.connections.front(), after_reconnect.connections.front()));
    CHECK(same_edge(after_branch.connections[2], original_branch));
    CHECK(same_edge(after_branch.connections.back(), {"file", "audio", "mixer-2", "primary"}));
    CHECK(after_branch.revision == after_reconnect.revision + 1);
    CHECK(fixture.controller.history().undo_size() == 2);
    CHECK(same_positions(after_branch.nodes, after_reconnect.nodes));
}

TEST_CASE("NCE-04.c Reconnect rejection is atomic and unchanged reconnect preserves redo",
          "[NCE-04.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto same_project = [](const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.revision != right.revision ||
            left.selected_output_node_id != right.selected_output_node_id ||
            left.selected_output_port_id != right.selected_output_port_id ||
            left.serialized_extensions != right.serialized_extensions || left.nodes.size() != right.nodes.size() ||
            left.connections.size() != right.connections.size())
            return false;
        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            const auto& a = left.nodes[index];
            const auto& b = right.nodes[index];
            if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters ||
                a.x != b.x || a.y != b.y || a.opaque_parameters != b.opaque_parameters ||
                a.serialized_extensions != b.serialized_extensions || a.managed_version != b.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& a = left.connections[index];
            const auto& b = right.connections[index];
            if (a.from_node != b.from_node || a.from_port != b.from_port ||
                a.to_node != b.to_node || a.to_port != b.to_port ||
                a.serialized_extensions != b.serialized_extensions)
                return false;
        }
        return true;
    };
    const auto prepare_redo = [](auto& controller) {
        REQUIRE(controller.set_parameter("gain", "gain_db", 3.0).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        REQUIRE(controller.undo());
        REQUIRE(controller.history().can_redo());
    };
    const auto check_state_unchanged = [&](auto& controller, const audioalgo::ProjectModel& before,
                                            bool dirty, bool results_stale,
                                            std::size_t undo_size, std::size_t redo_size) {
        CHECK(same_project(controller.project(), before));
        CHECK(controller.revision() == before.revision);
        CHECK(controller.dirty() == dirty);
        CHECK(controller.results_stale() == results_stale);
        CHECK(controller.history().undo_size() == undo_size);
        CHECK(controller.history().redo_size() == redo_size);
        CHECK(controller.history().can_redo() == (redo_size != 0));
    };

    SECTION("Controller rejects an invalid replacement and returns unchanged for the original edge") {
        CanvasInteractionFixture fixture;
        fixture.controller.mark_result_current();
        prepare_redo(fixture.controller);
        const auto original = fixture.controller.project().connections.front();
        const auto before = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo_size = fixture.controller.history().undo_size();
        const auto redo_size = fixture.controller.history().redo_size();

        const auto invalid = fixture.controller.reconnect_connection(
            original, {"gain", "audio_out", "gain", "audio_in"});
        REQUIRE(invalid.status == audioalgo::editor::ProjectEditStatus::rejected);
        REQUIRE_FALSE(invalid.diagnostics.empty());
        CHECK(invalid.diagnostics.front().code == "connection_self");
        check_state_unchanged(fixture.controller, before, dirty, stale, undo_size, redo_size);

        const auto unchanged = fixture.controller.reconnect_connection(original, original);
        CHECK(unchanged.status == audioalgo::editor::ProjectEditStatus::unchanged);
        CHECK(unchanged.diagnostics.empty());
        check_state_unchanged(fixture.controller, before, dirty, stale, undo_size, redo_size);
    }

    SECTION("GraphCanvas rejection preserves the edge and unchanged reconnect is silent") {
        CanvasInteractionFixture fixture;
        fixture.controller.mark_result_current();
        prepare_redo(fixture.controller);
        const auto original = fixture.controller.project().connections.front();
        const auto before = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo_size = fixture.controller.history().undo_size();
        const auto redo_size = fixture.controller.history().redo_size();
        const auto geometry = audioalgo::editor::make_connection_geometry(
            fixture.controller.project(), fixture.registry, original, fixture.canvas.coordinate_transform());
        REQUIRE(geometry.has_value());
        const auto handle_geometry = audioalgo::editor::make_source_endpoint_handle_geometry(
            fixture.controller.project(), fixture.registry, original, fixture.canvas.coordinate_transform());
        REQUIRE(handle_geometry.has_value());
        const auto source_handle = juce::Point<float>{handle_geometry->center.x, handle_geometry->center.y};
        const auto source_node = std::find_if(fixture.controller.project().nodes.begin(),
                                              fixture.controller.project().nodes.end(),
            [&](const auto& node) { return node.id == original.from_node; });
        const auto gain_node = std::find_if(fixture.controller.project().nodes.begin(),
                                            fixture.controller.project().nodes.end(),
            [](const auto& node) { return node.id == "gain"; });
        REQUIRE(source_node != fixture.controller.project().nodes.end());
        REQUIRE(gain_node != fixture.controller.project().nodes.end());
        const auto old_source = audioalgo::editor::make_node_geometry(
            *source_node,
            fixture.registry.find("builtin.file_source"), fixture.canvas.coordinate_transform());
        const auto gain_geometry = audioalgo::editor::make_node_geometry(
            *gain_node,
            fixture.registry.find("builtin.gain"), fixture.canvas.coordinate_transform());
        const auto output_port = std::find_if(old_source.ports.begin(), old_source.ports.end(), [&](const auto& port) {
            return port.id == original.from_port && port.direction == audioalgo::PortDirection::output;
        });
        const auto invalid_output = std::find_if(gain_geometry.ports.begin(), gain_geometry.ports.end(),
            [](const auto& port) { return port.id == "audio_out" && port.direction == audioalgo::PortDirection::output; });
        REQUIRE(output_port != old_source.ports.end());
        REQUIRE(invalid_output != gain_geometry.ports.end());
        const auto select_point = juce::Point<float>{
            (geometry->start.x + 3.0f * geometry->control1.x + 3.0f * geometry->control2.x + geometry->finish.x) / 8.0f,
            (geometry->start.y + 3.0f * geometry->control1.y + 3.0f * geometry->control2.y + geometry->finish.y) / 8.0f};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, select_point,
            juce::ModifierKeys::leftButtonModifier, select_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, select_point, 0, select_point));
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, source_handle,
            juce::ModifierKeys::leftButtonModifier, source_handle));
        const auto invalid_target = juce::Point<float>{invalid_output->anchor.x, invalid_output->anchor.y};
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, invalid_target,
            juce::ModifierKeys::leftButtonModifier, source_handle, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, invalid_target, 0, source_handle, true));
        check_state_unchanged(fixture.controller, before, dirty, stale, undo_size, redo_size);
        REQUIRE(fixture.errors.size() == 1);
        CHECK(fixture.errors.front().find("connection_self") != std::string::npos);

        fixture.errors.clear();
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, source_handle,
            juce::ModifierKeys::leftButtonModifier, source_handle));
        const auto original_target = juce::Point<float>{output_port->anchor.x, output_port->anchor.y};
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, original_target,
            juce::ModifierKeys::leftButtonModifier, source_handle, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, original_target, 0, source_handle, true));
        check_state_unchanged(fixture.controller, before, dirty, stale, undo_size, redo_size);
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("NCE-06.a Wrong-direction socket release reports a concrete rejection without editing",
          "[NCE-06.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    REQUIRE(fixture.registry.register_descriptor({
        "test.canvas_control_source", "1.0.0", "Canvas control source",
        {{"control", "control.value@1", audioalgo::PortDirection::output, false}}, {},
        audioalgo::NodeExecutionKind::source}));
    auto expanded = fixture.controller.project();
    expanded.nodes.push_back({"control", "test.canvas_control_source", {}, 40.0, 360.0});
    expanded.nodes.push_back({"cycle-a", "builtin.gain", {{"gain_db", 0.0}}, 340.0, 360.0});
    expanded.nodes.push_back({"cycle-b", "builtin.gain", {{"gain_db", 0.0}}, 640.0, 360.0});
    fixture.controller.adopt_project(std::move(expanded));

    const auto port_anchor = [&](const std::string& node_id, const std::string& port_id,
                                 audioalgo::PortDirection direction) {
        const auto node = std::find_if(fixture.controller.project().nodes.begin(),
                                       fixture.controller.project().nodes.end(),
            [&](const auto& item) { return item.id == node_id; });
        if (node == fixture.controller.project().nodes.end())
            return std::optional<juce::Point<float>>{};
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [&](const auto& item) { return item.id == port_id && item.direction == direction; });
        if (port == geometry.ports.end())
            return std::optional<juce::Point<float>>{};
        return std::optional<juce::Point<float>>{juce::Point<float>{port->anchor.x, port->anchor.y}};
    };
    const auto source = port_anchor("file", "audio", audioalgo::PortDirection::output);
    const auto wrong_direction_target = port_anchor("gain", "audio_out", audioalgo::PortDirection::output);
    const auto gain_input = port_anchor("gain", "audio_in", audioalgo::PortDirection::input);
    const auto control_output = port_anchor("control", "control", audioalgo::PortDirection::output);
    const auto cycle_a_output = port_anchor("cycle-a", "audio_out", audioalgo::PortDirection::output);
    const auto cycle_a_input = port_anchor("cycle-a", "audio_in", audioalgo::PortDirection::input);
    const auto cycle_b_output = port_anchor("cycle-b", "audio_out", audioalgo::PortDirection::output);
    const auto cycle_b_input = port_anchor("cycle-b", "audio_in", audioalgo::PortDirection::input);
    REQUIRE(source.has_value());
    REQUIRE(wrong_direction_target.has_value());
    REQUIRE(gain_input.has_value());
    REQUIRE(control_output.has_value());
    REQUIRE(cycle_a_output.has_value());
    REQUIRE(cycle_a_input.has_value());
    REQUIRE(cycle_b_output.has_value());
    REQUIRE(cycle_b_input.has_value());

    const auto same_project_snapshot = [](const audioalgo::ProjectModel& left,
                                          const audioalgo::ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.revision != right.revision ||
            left.selected_output_node_id != right.selected_output_node_id ||
            left.selected_output_port_id != right.selected_output_port_id ||
            left.serialized_extensions != right.serialized_extensions || left.nodes.size() != right.nodes.size() ||
            left.connections.size() != right.connections.size())
            return false;
        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            const auto& actual = left.nodes[index];
            const auto& expected = right.nodes[index];
            if (actual.id != expected.id || actual.type_id != expected.type_id ||
                actual.parameters != expected.parameters || actual.x != expected.x || actual.y != expected.y ||
                actual.opaque_parameters != expected.opaque_parameters ||
                actual.serialized_extensions != expected.serialized_extensions ||
                actual.managed_version != expected.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& actual = left.connections[index];
            const auto& expected = right.connections[index];
            if (actual.from_node != expected.from_node || actual.from_port != expected.from_port ||
                actual.to_node != expected.to_node || actual.to_port != expected.to_port ||
                actual.serialized_extensions != expected.serialized_extensions)
                return false;
        }
        return true;
    };
    const auto expect_rejection_without_edit = [&](juce::Point<float> from, juce::Point<float> to,
                                                    const std::string& code, const std::string& message) {
        const auto before = fixture.controller.project();
        const auto dirty_before = fixture.controller.dirty();
        const auto stale_before = fixture.controller.results_stale();
        const auto undo_before = fixture.controller.history().undo_size();
        const auto redo_before = fixture.controller.history().redo_size();
        const auto errors_before = fixture.errors.size();
        const auto layouts_before = fixture.layout_changes;
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, from,
            juce::ModifierKeys::leftButtonModifier, from));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, to,
            juce::ModifierKeys::leftButtonModifier, from, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, to, 0, from, true));

        REQUIRE(fixture.errors.size() == errors_before + 1);
        CHECK(fixture.errors.back().find(code + ":") == 0);
        CHECK(fixture.errors.back().find(message) != std::string::npos);
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.controller.dirty() == dirty_before);
        CHECK(fixture.controller.results_stale() == stale_before);
        CHECK(fixture.controller.history().undo_size() == undo_before);
        CHECK(fixture.controller.history().redo_size() == redo_before);
        CHECK(fixture.layout_changes == layouts_before);
    };

    expect_rejection_without_edit(*source, *wrong_direction_target,
        "connection_target_port_direction", "Connection target must use an input port");
    expect_rejection_without_edit(*control_output, *gain_input,
        "connection_type_mismatch", "Port types do not match");

    const auto before_cycle_seed = fixture.controller.project();
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *cycle_a_output,
        juce::ModifierKeys::leftButtonModifier, *cycle_a_output));
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *cycle_b_input,
        juce::ModifierKeys::leftButtonModifier, *cycle_a_output, true));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *cycle_b_input, 0, *cycle_a_output, true));
    REQUIRE(fixture.controller.project().connections.size() == 3);
    REQUIRE(fixture.controller.project().revision == before_cycle_seed.revision + 1);
    expect_rejection_without_edit(*cycle_b_output, *cycle_a_input,
        "graph_cycle", "Graph must be acyclic");
}

TEST_CASE("NCE-06.b Expanded socket hit starts the right gesture and drag threshold is six logical pixels",
          "[NCE-06.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;

    SECTION("Port connection gesture crosses only after 6 logical pixels") {
        for (const auto distance : {5.0f, 6.0f, 7.0f}) {
            audioalgo::editor::PortConnectionGesture gesture;
            REQUIRE(gesture.begin("file", "audio", {120.0f, 80.0f}));
            REQUIRE(gesture.update_pointer({120.0f + distance, 80.0f}));
            gesture.mark_drag_started();
            CHECK(gesture.is_dragging() == (distance > 6.0f));
        }
    }

    SECTION("Expanded socket hit wins over selected source handle and the cable behind it") {
        CanvasInteractionFixture fixture;
        auto project = fixture.controller.project();
        project.nodes.push_back({"mixer", "builtin.mixer", {}, 340.0, 320.0});
        fixture.controller.adopt_project(std::move(project));

        const auto source_node = std::find_if(fixture.controller.project().nodes.begin(),
                                              fixture.controller.project().nodes.end(),
            [](const auto& node) { return node.id == "file"; });
        REQUIRE(source_node != fixture.controller.project().nodes.end());
        const auto source_geometry = audioalgo::editor::make_node_geometry(
            *source_node, fixture.registry.find(source_node->type_id), fixture.canvas.coordinate_transform());
        const auto source_port = std::find_if(source_geometry.ports.begin(), source_geometry.ports.end(),
            [](const auto& port) { return port.id == "audio" &&
                                         port.direction == audioalgo::PortDirection::output; });
        REQUIRE(source_port != source_geometry.ports.end());
        const auto source_socket = juce::Point<float>{source_port->anchor.x, source_port->anchor.y};
        const auto expanded_socket_hit = source_socket.translated(11.0f, 0.0f);
        const auto expanded_hit = audioalgo::editor::find_topmost_port_at(
            fixture.controller.project().nodes, fixture.registry, {expanded_socket_hit.x, expanded_socket_hit.y},
            fixture.canvas.coordinate_transform());
        REQUIRE(expanded_hit.has_value());
        CHECK(expanded_hit->node_id == "file");
        CHECK(expanded_hit->port_id == "audio");

        const auto edge_geometry = audioalgo::editor::make_connection_geometry(
            fixture.controller.project(), fixture.registry, fixture.controller.project().connections.front(),
            fixture.canvas.coordinate_transform());
        REQUIRE(edge_geometry.has_value());
        const auto select_edge = juce::Point<float>{
            0.125f * edge_geometry->start.x + 0.375f * edge_geometry->control1.x +
                0.375f * edge_geometry->control2.x + 0.125f * edge_geometry->finish.x,
            0.125f * edge_geometry->start.y + 0.375f * edge_geometry->control1.y +
                0.375f * edge_geometry->control2.y + 0.125f * edge_geometry->finish.y};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, select_edge,
            juce::ModifierKeys::leftButtonModifier, select_edge));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, select_edge, 0, select_edge));

        const auto before = fixture.controller.project();
        REQUIRE(before.connections.size() == 2);
        REQUIRE(fixture.controller.history().undo_size() == 0);
        const auto mixer_node = std::find_if(before.nodes.begin(), before.nodes.end(),
            [](const auto& node) { return node.id == "mixer"; });
        REQUIRE(mixer_node != before.nodes.end());
        const auto mixer_geometry = audioalgo::editor::make_node_geometry(
            *mixer_node, fixture.registry.find(mixer_node->type_id), fixture.canvas.coordinate_transform());
        const auto mixer_input = std::find_if(mixer_geometry.ports.begin(), mixer_geometry.ports.end(),
            [](const auto& port) { return port.id == "primary" &&
                                         port.direction == audioalgo::PortDirection::input; });
        REQUIRE(mixer_input != mixer_geometry.ports.end());
        const auto mixer_input_point = juce::Point<float>{mixer_input->anchor.x, mixer_input->anchor.y};

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, expanded_socket_hit,
            juce::ModifierKeys::leftButtonModifier, expanded_socket_hit));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, mixer_input_point,
            juce::ModifierKeys::leftButtonModifier, expanded_socket_hit, true));
        CHECK(fixture.controller.project().connections.size() == before.connections.size());
        CHECK(fixture.controller.project().revision == before.revision);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, mixer_input_point, 0, expanded_socket_hit, true));

        const auto& after = fixture.controller.project();
        REQUIRE(after.connections.size() == before.connections.size() + 1);
        CHECK(after.connections[0].from_node == before.connections[0].from_node);
        CHECK(after.connections[0].from_port == before.connections[0].from_port);
        CHECK(after.connections[0].to_node == before.connections[0].to_node);
        CHECK(after.connections[0].to_port == before.connections[0].to_port);
        CHECK(after.connections[1].from_node == before.connections[1].from_node);
        CHECK(after.connections[1].to_node == before.connections[1].to_node);
        CHECK(after.connections.back().from_node == "file");
        CHECK(after.connections.back().from_port == "audio");
        CHECK(after.connections.back().to_node == "mixer");
        CHECK(after.connections.back().to_port == "primary");
        CHECK(after.revision == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("NCE-06.c Port and selected-handle geometry stays aligned through zoom and pan",
          "[NCE-06.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;

    for (const auto scale : {0.5f, 1.0f, 2.0f}) {
        INFO("zoom=" << scale);
        CanvasInteractionFixture fixture;
        auto project = fixture.controller.project();
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("first.wav")}}, -120, -80},
                         {"gain", "builtin.gain", {{"gain_db", 0.0}}, 120, -80},
                         {"output", "builtin.output", {}, 350, -80},
                         {"file-2", "builtin.file_source", {{"path", std::string("second.wav")}}, -120, 40},
                         {"mixer", "builtin.mixer", {}, 180, 80}};
        project.connections = {{"file", "audio", "gain", "audio_in"},
                               {"gain", "audio_out", "output", "audio"}};
        project.selected_output_node_id = "output";
        fixture.controller.adopt_project(std::move(project));

        juce::MouseWheelDetails wheel{};
        wheel.deltaY = std::log(scale) / std::log(1.25f);
        fixture.canvas.mouseWheelMove(canvas_mouse(fixture.canvas, {0.0f, 0.0f}, 0, {0.0f, 0.0f}), wheel);
        fixture.drag({0.0f, 0.0f}, {400.0f, 250.0f}, juce::ModifierKeys::middleButtonModifier);
        CHECK(fixture.canvas.coordinate_transform().zoom_scale() == Catch::Approx(scale).margin(0.0001f));

        const auto view = [&](float x, float y) {
            const auto point = fixture.canvas.coordinate_transform().model_to_canvas({x, y});
            return juce::Point<float>{point.x, point.y};
        };
        const auto original = fixture.controller.project();
        const auto edge_geometry = audioalgo::editor::make_connection_geometry(
            original, fixture.registry, original.connections.front(), fixture.canvas.coordinate_transform());
        REQUIRE(edge_geometry.has_value());
        const auto selected_edge_point = juce::Point<float>{
            0.125f * edge_geometry->start.x + 0.375f * edge_geometry->control1.x +
                0.375f * edge_geometry->control2.x + 0.125f * edge_geometry->finish.x,
            0.125f * edge_geometry->start.y + 0.375f * edge_geometry->control1.y +
                0.375f * edge_geometry->control2.y + 0.125f * edge_geometry->finish.y};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, selected_edge_point,
            juce::ModifierKeys::leftButtonModifier, selected_edge_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, selected_edge_point, 0, selected_edge_point));

        const auto handle_geometry = audioalgo::editor::make_source_endpoint_handle_geometry(
            fixture.controller.project(), fixture.registry, fixture.controller.project().connections.front(),
            fixture.canvas.coordinate_transform());
        REQUIRE(handle_geometry.has_value());
        const auto source_handle = juce::Point<float>{handle_geometry->center.x, handle_geometry->center.y};
        CHECK(handle_geometry->radius == Catch::Approx(6.0f * scale).margin(0.001f));
        CHECK(audioalgo::editor::source_endpoint_handle_contains(
            *handle_geometry, {source_handle.x, source_handle.y}));
        CHECK_FALSE(audioalgo::editor::source_endpoint_handle_contains(
            *handle_geometry, {source_handle.x + handle_geometry->radius + 0.01f, source_handle.y}));
        CHECK_FALSE(audioalgo::editor::find_topmost_port_at(
            fixture.controller.project().nodes, fixture.registry,
            {source_handle.x, source_handle.y}, fixture.canvas.coordinate_transform()).has_value());
        CHECK(audioalgo::editor::find_topmost_node_at(
            fixture.controller.project().nodes, fixture.registry,
            {source_handle.x, source_handle.y}, fixture.canvas.coordinate_transform()) == nullptr);
        juce::Image selected_edge_image(juce::Image::RGB, fixture.canvas.getWidth(), fixture.canvas.getHeight(), true);
        {
            juce::Graphics graphics(selected_edge_image);
            fixture.canvas.paint(graphics);
        }
        const auto handle_x = juce::roundToInt(source_handle.x);
        const auto handle_y = juce::roundToInt(source_handle.y);
        REQUIRE(handle_x >= 0);
        REQUIRE(handle_y >= 0);
        REQUIRE(handle_x < fixture.canvas.getWidth());
        REQUIRE(handle_y < fixture.canvas.getHeight());
        CHECK(selected_edge_image.getPixelAt(handle_x, handle_y) !=
              selected_edge_image.getPixelAt(juce::roundToInt(source_handle.x + handle_geometry->radius + 2.0f),
                                              handle_y));
        const auto replacement_source = view(56.0f, 104.0f);
        const auto before_reconnect = fixture.controller.project();
        const auto original_positions = before_reconnect.nodes;
        const auto reconnect_layout_changes = fixture.layout_changes;
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, source_handle,
            juce::ModifierKeys::leftButtonModifier, source_handle));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, replacement_source,
            juce::ModifierKeys::leftButtonModifier, source_handle, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, replacement_source, 0, source_handle, true));

        const auto after_reconnect = fixture.controller.project();
        CHECK(after_reconnect.connections.size() == original.connections.size());
        const bool source_handle_reconnected = !after_reconnect.connections.empty() &&
            after_reconnect.connections.front().from_node == "file-2";
        CHECK(source_handle_reconnected);
        if (source_handle_reconnected) {
            CHECK(after_reconnect.connections.front().from_port == "audio");
            CHECK(after_reconnect.connections.front().to_node == "gain");
            CHECK(after_reconnect.connections.front().to_port == "audio_in");
        }
        CHECK(after_reconnect.revision == before_reconnect.revision + (source_handle_reconnected ? 1U : 0U));
        CHECK(fixture.controller.history().undo_size() == (source_handle_reconnected ? 1U : 0U));
        CHECK(fixture.layout_changes == reconnect_layout_changes + (source_handle_reconnected ? 1 : 0));
        CHECK(fixture.errors.empty());
        REQUIRE(after_reconnect.nodes.size() == original_positions.size());
        for (std::size_t index = 0; index < after_reconnect.nodes.size(); ++index) {
            CHECK(after_reconnect.nodes[index].x == original_positions[index].x);
            CHECK(after_reconnect.nodes[index].y == original_positions[index].y);
        }

        const auto before_branch = fixture.controller.project();
        const auto source_node = std::find_if(before_branch.nodes.begin(), before_branch.nodes.end(),
            [](const auto& node) { return node.id == "file-2"; });
        const auto mixer_node = std::find_if(before_branch.nodes.begin(), before_branch.nodes.end(),
            [](const auto& node) { return node.id == "mixer"; });
        REQUIRE(source_node != before_branch.nodes.end());
        REQUIRE(mixer_node != before_branch.nodes.end());
        const auto source_layout = audioalgo::editor::make_node_geometry(
            *source_node, fixture.registry.find(source_node->type_id), fixture.canvas.coordinate_transform());
        const auto mixer_layout = audioalgo::editor::make_node_geometry(
            *mixer_node, fixture.registry.find(mixer_node->type_id), fixture.canvas.coordinate_transform());
        const auto source_port = std::find_if(source_layout.ports.begin(), source_layout.ports.end(),
            [](const auto& port) { return port.id == "audio" &&
                                          port.direction == audioalgo::PortDirection::output; });
        const auto mixer_port = std::find_if(mixer_layout.ports.begin(), mixer_layout.ports.end(),
            [](const auto& port) { return port.id == "primary" &&
                                          port.direction == audioalgo::PortDirection::input; });
        REQUIRE(source_port != source_layout.ports.end());
        REQUIRE(mixer_port != mixer_layout.ports.end());
        const auto source_anchor = juce::Point<float>{source_port->anchor.x, source_port->anchor.y};
        const auto mixer_anchor = juce::Point<float>{mixer_port->anchor.x, mixer_port->anchor.y};
        const auto branch_layout_changes = fixture.layout_changes;
        const auto history_before_branch = fixture.controller.history().undo_size();

        juce::Image before_preview(juce::Image::RGB, fixture.canvas.getWidth(), fixture.canvas.getHeight(), true);
        {
            juce::Graphics graphics(before_preview);
            fixture.canvas.paint(graphics);
        }
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, source_anchor,
            juce::ModifierKeys::leftButtonModifier, source_anchor));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, mixer_anchor,
            juce::ModifierKeys::leftButtonModifier, source_anchor, true));
        CHECK(fixture.controller.project().connections.size() == before_branch.connections.size());
        CHECK(fixture.controller.project().revision == before_branch.revision);
        CHECK(fixture.layout_changes == branch_layout_changes);
        juce::Image during_preview(juce::Image::RGB, fixture.canvas.getWidth(), fixture.canvas.getHeight(), true);
        {
            juce::Graphics graphics(during_preview);
            fixture.canvas.paint(graphics);
        }
        const auto target_x = juce::roundToInt(mixer_anchor.x);
        const auto target_y = juce::roundToInt(mixer_anchor.y);
        REQUIRE(target_x >= 0);
        REQUIRE(target_y >= 0);
        REQUIRE(target_x < fixture.canvas.getWidth());
        REQUIRE(target_y < fixture.canvas.getHeight());
        CHECK(during_preview.getPixelAt(target_x, target_y) != before_preview.getPixelAt(target_x, target_y));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, mixer_anchor, 0, source_anchor, true));

        const auto after_branch = fixture.controller.project();
        REQUIRE(after_branch.connections.size() == before_branch.connections.size() + 1);
        CHECK(after_branch.revision == before_branch.revision + 1);
        CHECK(fixture.controller.history().undo_size() == history_before_branch + 1);
        CHECK(fixture.layout_changes == branch_layout_changes + 1);
        const auto created = std::find_if(after_branch.connections.begin(), after_branch.connections.end(),
            [](const auto& edge) { return edge.from_node == "file-2" && edge.to_node == "mixer"; });
        REQUIRE(created != after_branch.connections.end());
        const auto final_geometry = audioalgo::editor::make_connection_geometry(
            after_branch, fixture.registry, *created, fixture.canvas.coordinate_transform());
        REQUIRE(final_geometry.has_value());
        CHECK(final_geometry->start.x == Catch::Approx(source_anchor.x).margin(0.001f));
        CHECK(final_geometry->start.y == Catch::Approx(source_anchor.y).margin(0.001f));
        CHECK(final_geometry->finish.x == Catch::Approx(mixer_anchor.x).margin(0.001f));
        CHECK(final_geometry->finish.y == Catch::Approx(mixer_anchor.y).margin(0.001f));
        for (std::size_t index = 0; index < after_branch.nodes.size(); ++index) {
            CHECK(after_branch.nodes[index].x == original_positions[index].x);
            CHECK(after_branch.nodes[index].y == original_positions[index].y);
        }
        CHECK(fixture.selection.selected_node_ids().empty());
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("NCE-07.a Topology actions restore exact edge sets and preserve node data through undo and redo",
          "[NCE-07.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto prepare = [](CanvasInteractionFixture& fixture, bool add_mixer, bool add_gain_branch) {
        auto initial = fixture.controller.project();
        initial.schema_version = 2;
        initial.project_id = "nce07a-history-exactness";
        initial.name = "History data must round-trip";
        initial.revision = 140;
        initial.serialized_extensions = {{"project_extension", "{\"preserve\":true}"}};
        constexpr auto content_hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        auto managed_gain = *fixture.registry.find("builtin.gain");
        managed_gain.type_id = "fixture.managed_gain";
        managed_gain.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
            "example.package", "1.0.0", {}, {}, "python", content_hash, "python-function-v1",
            "packages/example/nce07a"};
        REQUIRE(fixture.registry.register_descriptor(std::move(managed_gain)));
        if (add_mixer)
            initial.nodes.push_back({"mixer", "builtin.mixer", {}, 340, 320});
        if (add_gain_branch)
            initial.connections.push_back({"gain", "audio_out", "mixer", "secondary"});
        for (auto& node : initial.nodes) {
            node.serialized_extensions = {{"node_extension", "\"" + node.id + "\""}};
            if (node.id == "gain") {
                node.type_id = "fixture.managed_gain";
                node.managed_version = audioalgo::NodeInstance::ManagedVersion{
                    "example.package", content_hash, "packages/example/nce07a", "python-function-v1",
                    {{"lock_extension", "\"preserve\""}}};
            }
        }
        for (std::size_t index = 0; index < initial.connections.size(); ++index)
            initial.connections[index].serialized_extensions = {{"edge_extension", "\"edge-" +
                std::to_string(index) + "\""}};
        CHECK(audioalgo::validate_project_structure(initial, fixture.registry).empty());
        fixture.controller.adopt_project(initial);
        fixture.controller.mark_saved();
        fixture.controller.mark_result_current();
        return initial;
    };
    const auto verify_edit_undo_redo = [&](audioalgo::editor::EditorController& controller,
                                           const audioalgo::ProjectModel& initial,
                                           audioalgo::ProjectModel expected_after) {
        expected_after.revision = initial.revision + 1;
        CHECK(same_project_snapshot(controller.project(), expected_after));
        CHECK(controller.dirty());
        CHECK(controller.results_stale());
        CHECK(controller.history().undo_size() == 1);
        CHECK(controller.history().redo_size() == 0);

        REQUIRE(controller.undo());
        auto expected_undo = initial;
        expected_undo.revision = initial.revision + 2;
        CHECK(same_project_snapshot(controller.project(), expected_undo));
        CHECK(controller.dirty());
        CHECK(controller.results_stale());
        CHECK(controller.history().undo_size() == 0);
        CHECK(controller.history().redo_size() == 1);

        REQUIRE(controller.redo());
        expected_after.revision = initial.revision + 3;
        CHECK(same_project_snapshot(controller.project(), expected_after));
        CHECK(controller.dirty());
        CHECK(controller.results_stale());
        CHECK(controller.history().undo_size() == 1);
        CHECK(controller.history().redo_size() == 0);
    };

    SECTION("Deleting one selected edge preserves its value identity through undo and redo") {
        CanvasInteractionFixture fixture;
        const auto initial = prepare(fixture, false, false);
        const auto geometry = audioalgo::editor::make_connection_geometry(
            initial, fixture.registry, initial.connections.front(), fixture.canvas.coordinate_transform());
        REQUIRE(geometry.has_value());
        const juce::Point<float> edge_point{
            0.125f * geometry->start.x + 0.375f * geometry->control1.x +
                0.375f * geometry->control2.x + 0.125f * geometry->finish.x,
            0.125f * geometry->start.y + 0.375f * geometry->control1.y +
                0.375f * geometry->control2.y + 0.125f * geometry->finish.y};
        std::function<void()> disconnect_action;
        fixture.canvas.set_connection_context_menu_handler(
            [&](std::string label, std::function<void()> action) {
                CHECK(label == "Disconnect connection");
                disconnect_action = std::move(action);
            });
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, edge_point,
            juce::ModifierKeys::rightButtonModifier, edge_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, edge_point, 0, edge_point));
        REQUIRE(disconnect_action);
        disconnect_action();

        auto expected_after = initial;
        expected_after.connections.erase(expected_after.connections.begin());
        verify_edit_undo_redo(fixture.controller, initial, std::move(expected_after));
    }

    SECTION("Disconnecting one branch from a connected port restores the exact branch set") {
        CanvasInteractionFixture fixture;
        const auto initial = prepare(fixture, true, true);
        std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> menu_items;
        fixture.canvas.set_port_connection_context_menu_handler(
            [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
                menu_items = std::move(items);
            });
        const auto gain = std::find_if(initial.nodes.begin(), initial.nodes.end(),
            [](const auto& node) { return node.id == "gain"; });
        REQUIRE(gain != initial.nodes.end());
        const auto geometry = audioalgo::editor::make_node_geometry(
            *gain, fixture.registry.find(gain->type_id), fixture.canvas.coordinate_transform());
        const auto output_port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
            [](const auto& port) { return port.id == "audio_out"; });
        REQUIRE(output_port != geometry.ports.end());
        const juce::Point<float> output_point{output_port->anchor.x, output_port->anchor.y};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, output_point,
            juce::ModifierKeys::rightButtonModifier, output_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, output_point, 0, output_point));
        const auto selected_branch = std::find_if(menu_items.begin(), menu_items.end(), [](const auto& item) {
            return item.label == "gain:audio_out -> output:audio";
        });
        REQUIRE(selected_branch != menu_items.end());
        selected_branch->action();

        auto expected_after = initial;
        std::erase_if(expected_after.connections, [](const auto& edge) {
            return edge.from_node == "gain" && edge.from_port == "audio_out" &&
                   edge.to_node == "output" && edge.to_port == "audio";
        });
        verify_edit_undo_redo(fixture.controller, initial, std::move(expected_after));
    }

    SECTION("Disconnecting a node preserves that node and restores all incident edges") {
        CanvasInteractionFixture fixture;
        const auto initial = prepare(fixture, false, false);
        fixture.inspector.setSize(300, 500);
        fixture.canvas.select("gain");
        fixture.inspector.show_node("gain");
        auto* disconnect_button = descendant<juce::TextButton>(
            fixture.inspector, "inspector.disconnect-all-connections");
        REQUIRE(disconnect_button != nullptr);
        REQUIRE(disconnect_button->isEnabled());
        disconnect_button->onClick();

        auto expected_after = initial;
        std::erase_if(expected_after.connections, [](const auto& edge) {
            return edge.from_node == "gain" || edge.to_node == "gain";
        });
        verify_edit_undo_redo(fixture.controller, initial, std::move(expected_after));
    }

    SECTION("Reconnect preserves the exact old edge through undo and the new edge through redo") {
        CanvasInteractionFixture fixture;
        const auto initial = prepare(fixture, true, false);
        const auto gain = std::find_if(initial.nodes.begin(), initial.nodes.end(),
            [](const auto& node) { return node.id == "gain"; });
        const auto mixer = std::find_if(initial.nodes.begin(), initial.nodes.end(),
            [](const auto& node) { return node.id == "mixer"; });
        REQUIRE(gain != initial.nodes.end());
        REQUIRE(mixer != initial.nodes.end());
        const auto anchor = [&](const audioalgo::NodeInstance& node, const std::string& port_id) {
            const auto geometry = audioalgo::editor::make_node_geometry(
                node, fixture.registry.find(node.type_id), fixture.canvas.coordinate_transform());
            const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(),
                [&](const auto& candidate) { return candidate.id == port_id; });
            if (port == geometry.ports.end())
                return std::optional<juce::Point<float>>{};
            return std::optional<juce::Point<float>>{{port->anchor.x, port->anchor.y}};
        };
        const auto connected_input = anchor(*gain, "audio_in");
        const auto idle_input = anchor(*mixer, "primary");
        REQUIRE(connected_input.has_value());
        REQUIRE(idle_input.has_value());
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, *connected_input,
            juce::ModifierKeys::leftButtonModifier, *connected_input));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, *idle_input,
            juce::ModifierKeys::leftButtonModifier, *connected_input, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, *idle_input, 0, *connected_input, true));

        auto expected_after = initial;
        expected_after.connections.front().to_node = "mixer";
        expected_after.connections.front().to_port = "primary";
        verify_edit_undo_redo(fixture.controller, initial, std::move(expected_after));
    }
}

TEST_CASE("NCE-07.b Interleaved parameter layout and topology history preserves redo semantics",
          "[NCE-07.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;

    SECTION("parameter layout and topology entries undo and redo in order without swallowing one another") {
        CanvasInteractionFixture fixture;
        auto initial = fixture.controller.project();
        initial.project_id = "nce07b-history-interleaving";
        initial.name = "Parameter layout topology history";
        initial.revision = 60;
        initial.serialized_extensions = {{"project_extension", "{\"preserve\":true}"}};
        for (auto& node : initial.nodes)
            node.serialized_extensions = {{"node_extension", "\"" + node.id + "\""}};
        for (std::size_t index = 0; index < initial.connections.size(); ++index)
            initial.connections[index].serialized_extensions = {{"edge_extension", "\"edge-" +
                std::to_string(index) + "\""}};
        fixture.controller.adopt_project(initial);
        fixture.controller.mark_saved();
        fixture.controller.mark_result_current();

        REQUIRE(fixture.controller.set_parameter("gain", "gain_db", 4.0).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        auto after_parameter = initial;
        after_parameter.nodes[1].parameters["gain_db"] = 4.0;
        after_parameter.revision = initial.revision + 1;
        CHECK(same_project_snapshot(fixture.controller.project(), after_parameter));
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.history().redo_size() == 0);

        REQUIRE(fixture.controller.move_node_layout("gain", 380.0, 145.0).status ==
                audioalgo::editor::ProjectEditStatus::applied);
        auto after_layout = after_parameter;
        after_layout.nodes[1].x = 380.0;
        after_layout.nodes[1].y = 145.0;
        CHECK(same_project_snapshot(fixture.controller.project(), after_layout));
        CHECK(fixture.controller.history().undo_size() == 2);
        CHECK(fixture.controller.history().redo_size() == 0);

        const auto first_edge = initial.connections.front();
        REQUIRE(fixture.controller.disconnect(first_edge).status == audioalgo::editor::ProjectEditStatus::applied);
        auto after_topology = after_layout;
        after_topology.connections.erase(after_topology.connections.begin());
        after_topology.revision = initial.revision + 2;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 3);
        CHECK(fixture.controller.history().redo_size() == 0);
        CHECK(fixture.controller.results_stale());

        REQUIRE(fixture.controller.undo());
        after_topology.connections = initial.connections;
        after_topology.revision = initial.revision + 3;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 2);
        CHECK(fixture.controller.history().redo_size() == 1);

        REQUIRE(fixture.controller.undo());
        after_topology.nodes[1].x = initial.nodes[1].x;
        after_topology.nodes[1].y = initial.nodes[1].y;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.history().redo_size() == 2);

        REQUIRE(fixture.controller.undo());
        after_topology.nodes[1].parameters = initial.nodes[1].parameters;
        after_topology.revision = initial.revision + 4;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 0);
        CHECK(fixture.controller.history().redo_size() == 3);

        REQUIRE(fixture.controller.redo());
        after_topology.nodes[1].parameters["gain_db"] = 4.0;
        after_topology.revision = initial.revision + 5;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.history().redo_size() == 2);

        REQUIRE(fixture.controller.redo());
        after_topology.nodes[1].x = 380.0;
        after_topology.nodes[1].y = 145.0;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 2);
        CHECK(fixture.controller.history().redo_size() == 1);

        REQUIRE(fixture.controller.redo());
        after_topology.connections.erase(after_topology.connections.begin());
        after_topology.revision = initial.revision + 6;
        CHECK(same_project_snapshot(fixture.controller.project(), after_topology));
        CHECK(fixture.controller.history().undo_size() == 3);
        CHECK(fixture.controller.history().redo_size() == 0);
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
    }

    SECTION("rejected unchanged and cancelled edits preserve redo until a successful component edit") {
        CanvasInteractionFixture fixture;
        fixture.controller.mark_saved();
        fixture.controller.mark_result_current();
        const auto initial = fixture.controller.project();
        REQUIRE(fixture.controller.set_parameter("gain", "gain_db", 3.0).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        REQUIRE(fixture.controller.undo());
        REQUIRE(fixture.controller.history().can_redo());

        const auto assert_unchanged_and_redo = [&](const audioalgo::ProjectModel& expected,
                                                   bool dirty, bool stale, std::size_t undo_size,
                                                   std::size_t redo_size, std::size_t error_count,
                                                   int layout_changes) {
            CHECK(same_project_snapshot(fixture.controller.project(), expected));
            CHECK(fixture.controller.dirty() == dirty);
            CHECK(fixture.controller.results_stale() == stale);
            CHECK(fixture.controller.history().undo_size() == undo_size);
            CHECK(fixture.controller.history().redo_size() == redo_size);
            CHECK(fixture.controller.history().can_redo());
            CHECK(fixture.errors.size() == error_count);
            CHECK(fixture.layout_changes == layout_changes);
        };
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo_size = fixture.controller.history().undo_size();
        const auto redo_size = fixture.controller.history().redo_size();
        const auto error_count = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        REQUIRE(redo_size == 1);

        const auto original = initial.connections.front();
        const auto rejected = fixture.controller.reconnect_connection(
            original, {"gain", "audio_out", "gain", "audio_in"});
        REQUIRE(rejected.status == audioalgo::editor::ProjectEditStatus::rejected);
        assert_unchanged_and_redo(expected, dirty, stale, undo_size, redo_size, error_count, layout_changes);

        const auto unchanged = fixture.controller.reconnect_connection(original, original);
        REQUIRE(unchanged.status == audioalgo::editor::ProjectEditStatus::unchanged);
        CHECK(fixture.controller.move_node_layout("gain", expected.nodes[1].x, expected.nodes[1].y).status ==
              audioalgo::editor::ProjectEditStatus::unchanged);
        CHECK(fixture.controller.set_parameter("gain", "gain_db", 0.0).status ==
              audioalgo::editor::ParameterEditStatus::unchanged);
        assert_unchanged_and_redo(expected, dirty, stale, undo_size, redo_size, error_count, layout_changes);

        const auto gain = std::find_if(expected.nodes.begin(), expected.nodes.end(),
            [](const auto& node) { return node.id == "gain"; });
        REQUIRE(gain != expected.nodes.end());
        const auto gain_geometry = audioalgo::editor::make_node_geometry(
            *gain, fixture.registry.find(gain->type_id), fixture.canvas.coordinate_transform());
        const auto output_port = std::find_if(gain_geometry.ports.begin(), gain_geometry.ports.end(),
            [](const auto& port) { return port.id == "audio_out" &&
                port.direction == audioalgo::PortDirection::output; });
        REQUIRE(output_port != gain_geometry.ports.end());
        const juce::Point<float> start{output_port->anchor.x, output_port->anchor.y};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start,
            juce::ModifierKeys::leftButtonModifier, start));
        REQUIRE(fixture.canvas.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, start, 0, start));
        assert_unchanged_and_redo(expected, dirty, stale, undo_size, redo_size, error_count, layout_changes);

        std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> menu_items;
        fixture.canvas.set_port_connection_context_menu_handler(
            [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
                menu_items = std::move(items);
            });
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start,
            juce::ModifierKeys::rightButtonModifier, start));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, start, 0, start));
        const auto selected_edge = std::find_if(menu_items.begin(), menu_items.end(), [](const auto& item) {
            return item.label == "gain:audio_out -> output:audio";
        });
        REQUIRE(selected_edge != menu_items.end());
        selected_edge->action();

        auto expected_after = expected;
        std::erase_if(expected_after.connections, [](const auto& edge) {
            return edge.from_node == "gain" && edge.from_port == "audio_out" &&
                   edge.to_node == "output" && edge.to_port == "audio";
        });
        ++expected_after.revision;
        CHECK(same_project_snapshot(fixture.controller.project(), expected_after));
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
        CHECK(fixture.controller.history().undo_size() == undo_size + 1);
        CHECK(fixture.controller.history().redo_size() == 0);
    }
}

TEST_CASE("NCE-05.a Reconnect released on blank disconnects while a new line cancels",
          "[NCE-05.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto same_edge = [](const audioalgo::Connection& left, const audioalgo::Connection& right) {
        return left.from_node == right.from_node && left.from_port == right.from_port &&
               left.to_node == right.to_node && left.to_port == right.to_port &&
               left.serialized_extensions == right.serialized_extensions;
    };
    const auto same_nodes = [](const auto& actual, const auto& expected) {
        if (actual.size() != expected.size())
            return false;
        for (std::size_t index = 0; index < actual.size(); ++index) {
            const auto& left = actual[index];
            const auto& right = expected[index];
            if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
                left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
                left.serialized_extensions != right.serialized_extensions || left.managed_version != right.managed_version)
                return false;
        }
        return true;
    };

    SECTION("Controller disconnect keeps the node set and commits one undoable edit") {
        CanvasInteractionFixture fixture;
        fixture.controller.mark_result_current();
        const auto before = fixture.controller.project();
        REQUIRE(before.connections.size() == 2);
        const auto original = before.connections.front();
        const auto outcome = fixture.controller.disconnect(original);
        REQUIRE(outcome.status == audioalgo::editor::ProjectEditStatus::applied);
        CHECK(fixture.controller.project().connections.size() == 1);
        CHECK(same_edge(fixture.controller.project().connections.front(), before.connections.back()));
        CHECK(same_nodes(fixture.controller.project().nodes, before.nodes));
        CHECK(fixture.controller.revision() == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
        CHECK(fixture.controller.undo());
        CHECK(fixture.controller.project().connections.size() == before.connections.size());
        CHECK(fixture.controller.redo());
        CHECK(fixture.controller.project().connections.size() == 1);
        CHECK(same_nodes(fixture.controller.project().nodes, before.nodes));
    }

    SECTION("GraphCanvas distinguishes blank reconnect disconnect from new-line cancellation") {
        CanvasInteractionFixture fixture;
        fixture.controller.mark_result_current();
        const auto before = fixture.controller.project();
        REQUIRE(before.connections.size() == 2);
        const auto original = before.connections.front();
        const auto connected_input_node = std::find_if(before.nodes.begin(), before.nodes.end(),
            [&](const auto& node) { return node.id == original.to_node; });
        const auto source_node = std::find_if(before.nodes.begin(), before.nodes.end(),
            [&](const auto& node) { return node.id == original.from_node; });
        REQUIRE(connected_input_node != before.nodes.end());
        REQUIRE(source_node != before.nodes.end());
        const auto input_layout = audioalgo::editor::make_node_geometry(
            *connected_input_node, fixture.registry.find(connected_input_node->type_id),
            fixture.canvas.coordinate_transform());
        const auto source_layout = audioalgo::editor::make_node_geometry(
            *source_node, fixture.registry.find(source_node->type_id), fixture.canvas.coordinate_transform());
        const auto input = std::find_if(input_layout.ports.begin(), input_layout.ports.end(), [&](const auto& port) {
            return port.id == original.to_port && port.direction == audioalgo::PortDirection::input;
        });
        const auto output = std::find_if(source_layout.ports.begin(), source_layout.ports.end(), [&](const auto& port) {
            return port.id == original.from_port && port.direction == audioalgo::PortDirection::output;
        });
        REQUIRE(input != input_layout.ports.end());
        REQUIRE(output != source_layout.ports.end());
        const auto input_point = juce::Point<float>{input->anchor.x, input->anchor.y};
        const auto output_point = juce::Point<float>{output->anchor.x, output->anchor.y};
        const auto blank_point = juce::Point<float>{850.0f, 520.0f};
        CHECK(audioalgo::editor::find_topmost_node_at(before.nodes, fixture.registry,
            {blank_point.x, blank_point.y}, fixture.canvas.coordinate_transform()) == nullptr);
        CHECK_FALSE(audioalgo::editor::find_topmost_port_at(before.nodes, fixture.registry,
            {blank_point.x, blank_point.y}, fixture.canvas.coordinate_transform()).has_value());

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, input_point,
            juce::ModifierKeys::leftButtonModifier, input_point));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, blank_point,
            juce::ModifierKeys::leftButtonModifier, input_point, true));
        fixture.canvas.mouseMove(canvas_mouse(fixture.canvas, blank_point,
            juce::ModifierKeys::leftButtonModifier, input_point, true));
        const auto reconnect_feedback = fixture.canvas.getTooltip().toStdString();
        CHECK(reconnect_feedback.find("disconnect") != std::string::npos);
        CHECK(fixture.controller.project().connections.size() == before.connections.size());
        CHECK(fixture.controller.revision() == before.revision);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, blank_point, 0, input_point, true));
        const auto after_reconnect_drop = fixture.controller.project();
        CHECK(after_reconnect_drop.connections.size() == before.connections.size() - 1);
        CHECK(same_edge(after_reconnect_drop.connections.front(), before.connections.back()));
        CHECK(same_nodes(after_reconnect_drop.nodes, before.nodes));
        CHECK(after_reconnect_drop.revision == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
        CHECK(fixture.errors.empty());

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, output_point,
            juce::ModifierKeys::leftButtonModifier, output_point));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, blank_point,
            juce::ModifierKeys::leftButtonModifier, output_point, true));
        fixture.canvas.mouseMove(canvas_mouse(fixture.canvas, blank_point,
            juce::ModifierKeys::leftButtonModifier, output_point, true));
        const auto new_line_feedback = fixture.canvas.getTooltip().toStdString();
        CHECK(new_line_feedback.find("cancel") != std::string::npos);
        CHECK_FALSE(new_line_feedback.empty());
        CHECK(new_line_feedback != reconnect_feedback);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, blank_point, 0, output_point, true));
        const auto after_new_line_drop = fixture.controller.project();
        CHECK(after_new_line_drop.connections.size() == after_reconnect_drop.connections.size());
        CHECK(same_nodes(after_new_line_drop.nodes, after_reconnect_drop.nodes));
        CHECK(after_new_line_drop.revision == after_reconnect_drop.revision);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("NCE-05.b Connection cancellation clears previews without changing editor state",
          "[NCE-05.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto same_project = [](const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.revision != right.revision ||
            left.selected_output_node_id != right.selected_output_node_id ||
            left.selected_output_port_id != right.selected_output_port_id ||
            left.serialized_extensions != right.serialized_extensions || left.nodes.size() != right.nodes.size() ||
            left.connections.size() != right.connections.size())
            return false;
        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            const auto& a = left.nodes[index];
            const auto& b = right.nodes[index];
            if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters ||
                a.x != b.x || a.y != b.y || a.opaque_parameters != b.opaque_parameters ||
                a.serialized_extensions != b.serialized_extensions || a.managed_version != b.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& a = left.connections[index];
            const auto& b = right.connections[index];
            if (a.from_node != b.from_node || a.from_port != b.from_port ||
                a.to_node != b.to_node || a.to_port != b.to_port ||
                a.serialized_extensions != b.serialized_extensions)
                return false;
        }
        return true;
    };
    const auto same_image = [](const juce::Image& left, const juce::Image& right) {
        if (left.getWidth() != right.getWidth() || left.getHeight() != right.getHeight())
            return false;
        for (int y = 0; y < left.getHeight(); ++y)
            for (int x = 0; x < left.getWidth(); ++x)
                if (left.getPixelAt(x, y).getARGB() != right.getPixelAt(x, y).getARGB())
                    return false;
        return true;
    };
    const auto render = [](audioalgo::editor::GraphCanvas& canvas) {
        juce::Image image(juce::Image::ARGB, canvas.getWidth(), canvas.getHeight(), true);
        juce::Graphics graphics(image);
        canvas.paint(graphics);
        return image;
    };

    for (const bool reconnect : {false, true}) {
        for (const auto cancellation : {0, 1, 2, 3}) {
            INFO("gesture: " << (reconnect ? "reconnect" : "new connection")
                 << ", cancellation: " << (cancellation == 0 ? "Escape" : cancellation == 1 ? "canvas outside" :
                                               cancellation == 2 ? "focus loss" : "hide"));
            CanvasInteractionFixture fixture;
            auto project = fixture.controller.project();
            project.nodes.push_back({"gain-2", "builtin.gain", {{"gain_db", 0.0}}, 640, 300});
            fixture.controller.adopt_project(project);
            fixture.controller.mark_result_current();
            REQUIRE(fixture.controller.set_parameter("gain", "gain_db", 3.0).status ==
                    audioalgo::editor::ParameterEditStatus::applied);
            REQUIRE(fixture.controller.undo());
            REQUIRE(fixture.controller.history().can_redo());
            fixture.canvas.select("gain");
            fixture.canvas.setVisible(true);

            const auto node_port = [&](const std::string& node_id, const std::string& port_id,
                                       audioalgo::PortDirection direction) {
                const auto& nodes = fixture.controller.project().nodes;
                const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
                    return candidate.id == node_id;
                });
                REQUIRE(node != nodes.end());
                const auto layout = audioalgo::editor::make_node_geometry(
                    *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
                const auto port = std::find_if(layout.ports.begin(), layout.ports.end(), [&](const auto& candidate) {
                    return candidate.id == port_id && candidate.direction == direction;
                });
                REQUIRE(port != layout.ports.end());
                return juce::Point<float>{port->anchor.x, port->anchor.y};
            };
            const auto begin = reconnect
                ? node_port("gain", "audio_in", audioalgo::PortDirection::input)
                : node_port("file", "audio", audioalgo::PortDirection::output);
            const auto valid_target = node_port("gain-2", "audio_in", audioalgo::PortDirection::input);
            const auto drag_point = juce::Point<float>{500.0f, 520.0f};
            const auto outside_point = juce::Point<float>{-20.0f, 620.0f};
            const auto baseline = render(fixture.canvas);
            const auto before = fixture.controller.project();
            const auto dirty = fixture.controller.dirty();
            const auto stale = fixture.controller.results_stale();
            const auto undo_size = fixture.controller.history().undo_size();
            const auto redo_size = fixture.controller.history().redo_size();
            const auto selected_nodes = fixture.selection.selected_node_ids();
            const auto errors = fixture.errors.size();
            const auto layout_changes = fixture.layout_changes;

            fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, begin,
                juce::ModifierKeys::leftButtonModifier, begin));
            fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, drag_point,
                juce::ModifierKeys::leftButtonModifier, begin, true));
            REQUIRE_FALSE(same_image(baseline, render(fixture.canvas)));

            if (cancellation == 0) {
                REQUIRE(fixture.canvas.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
                fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, valid_target, 0, begin, true));
            } else if (cancellation == 1) {
                fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, outside_point, 0, begin, true));
                fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, valid_target, 0, begin, true));
            } else if (cancellation == 2) {
                fixture.canvas.focusLost(juce::Component::focusChangedDirectly);
                fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, valid_target, 0, begin, true));
            } else {
                fixture.canvas.setVisible(false);
                fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, valid_target, 0, begin, true));
            }

            CHECK(same_image(baseline, render(fixture.canvas)));
            CHECK(same_project(fixture.controller.project(), before));
            CHECK(fixture.controller.revision() == before.revision);
            CHECK(fixture.controller.dirty() == dirty);
            CHECK(fixture.controller.results_stale() == stale);
            CHECK(fixture.controller.history().undo_size() == undo_size);
            CHECK(fixture.controller.history().redo_size() == redo_size);
            CHECK(fixture.controller.history().can_redo() == (redo_size != 0));
            CHECK(fixture.selection.selected_node_ids() == selected_nodes);
            CHECK(fixture.errors.size() == errors);
            CHECK(fixture.layout_changes == layout_changes);
        }
    }
}

TEST_CASE("NCE-05.c Project changes invalidate active connection intent and deferred menu actions",
          "[NCE-05.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto same_project = [](const audioalgo::ProjectModel& left, const audioalgo::ProjectModel& right) {
        if (left.schema_version != right.schema_version || left.project_id != right.project_id ||
            left.name != right.name || left.revision != right.revision ||
            left.selected_output_node_id != right.selected_output_node_id ||
            left.selected_output_port_id != right.selected_output_port_id ||
            left.serialized_extensions != right.serialized_extensions || left.nodes.size() != right.nodes.size() ||
            left.connections.size() != right.connections.size())
            return false;
        for (std::size_t index = 0; index < left.nodes.size(); ++index) {
            const auto& a = left.nodes[index];
            const auto& b = right.nodes[index];
            if (a.id != b.id || a.type_id != b.type_id || a.parameters != b.parameters ||
                a.x != b.x || a.y != b.y || a.opaque_parameters != b.opaque_parameters ||
                a.serialized_extensions != b.serialized_extensions || a.managed_version != b.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < left.connections.size(); ++index) {
            const auto& a = left.connections[index];
            const auto& b = right.connections[index];
            if (a.from_node != b.from_node || a.from_port != b.from_port ||
                a.to_node != b.to_node || a.to_port != b.to_port ||
                a.serialized_extensions != b.serialized_extensions)
                return false;
        }
        return true;
    };
    const auto prepare = [](CanvasInteractionFixture& fixture) {
        auto project = fixture.controller.project();
        project.nodes.push_back({"gain-2", "builtin.gain", {{"gain_db", 0.0}}, 640, 300});
        fixture.controller.adopt_project(project);
        fixture.controller.mark_result_current();
        fixture.canvas.setVisible(true);
        fixture.canvas.select("gain");
    };
    const auto port_point = [](CanvasInteractionFixture& fixture, const std::string& node_id,
                               const std::string& port_id, audioalgo::PortDirection direction) {
        const auto& nodes = fixture.controller.project().nodes;
        const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
            return candidate.id == node_id;
        });
        REQUIRE(node != nodes.end());
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(), [&](const auto& candidate) {
            return candidate.id == port_id && candidate.direction == direction;
        });
        REQUIRE(port != geometry.ports.end());
        return juce::Point<float>{port->anchor.x, port->anchor.y};
    };
    const auto begin_drag = [](CanvasInteractionFixture& fixture, juce::Point<float> start) {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start,
            juce::ModifierKeys::leftButtonModifier, start));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, {500.0f, 520.0f},
            juce::ModifierKeys::leftButtonModifier, start, true));
    };
    const auto state_unchanged = [&](CanvasInteractionFixture& fixture,
                                     const audioalgo::ProjectModel& expected,
                                     bool dirty, bool stale, std::size_t undo_size, std::size_t redo_size,
                                     const std::vector<std::string>& selection,
                                     std::size_t errors, int layout_changes) {
        return same_project(fixture.controller.project(), expected) &&
               fixture.controller.revision() == expected.revision && fixture.controller.dirty() == dirty &&
               fixture.controller.results_stale() == stale &&
               fixture.controller.history().undo_size() == undo_size &&
               fixture.controller.history().redo_size() == redo_size &&
               fixture.controller.history().can_redo() == (redo_size != 0) &&
               fixture.selection.selected_node_ids() == selection && fixture.errors.size() == errors &&
               fixture.layout_changes == layout_changes;
    };

    SECTION("new connection intent is cancelled after an unrelated revision change") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        const auto start = port_point(fixture, "file", "audio", audioalgo::PortDirection::output);
        const auto target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        begin_drag(fixture, start);
        REQUIRE(fixture.controller.set_parameter("gain", "gain_db", 2.0).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
    }

    SECTION("reconnect intent is cancelled after an unrelated revision change") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        const auto start = port_point(fixture, "gain", "audio_in", audioalgo::PortDirection::input);
        const auto target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        begin_drag(fixture, start);
        REQUIRE(fixture.controller.set_parameter("gain-2", "gain_db", 2.5).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
    }

    SECTION("deleting the original reconnect target cancels instead of submitting a dangling edge") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        const auto start = port_point(fixture, "gain", "audio_in", audioalgo::PortDirection::input);
        const auto remaining_target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        begin_drag(fixture, start);
        REQUIRE(fixture.controller.delete_nodes({"gain"}).status == audioalgo::editor::ProjectEditStatus::applied);
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, remaining_target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
    }

    SECTION("deleting a new-line target leaves the changed graph untouched") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        const auto start = port_point(fixture, "file", "audio", audioalgo::PortDirection::output);
        const auto deleted_target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        begin_drag(fixture, start);
        REQUIRE(fixture.controller.delete_nodes({"gain-2"}).status == audioalgo::editor::ProjectEditStatus::applied);
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, deleted_target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
    }

    SECTION("New/Open project epoch cancels the preview without restoring the old project") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        const auto start = port_point(fixture, "file", "audio", audioalgo::PortDirection::output);
        begin_drag(fixture, start);
        auto replacement = fixture.controller.project();
        replacement.project_id = "replacement-during-connection-gesture";
        replacement.name = "Replacement project";
        fixture.controller.adopt_project(replacement);
        fixture.canvas.set_project_epoch(2);
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        const auto replacement_target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, replacement_target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
        CHECK(fixture.controller.project().project_id == "replacement-during-connection-gesture");
    }

    SECTION("a delayed connection menu and stale gesture cannot edit after the revision changes") {
        CanvasInteractionFixture fixture;
        prepare(fixture);
        std::function<void()> delayed_menu_action;
        fixture.canvas.set_connection_context_menu_handler(
            [&](std::string, std::function<void()> action) { delayed_menu_action = std::move(action); });
        const juce::Point<float> edge_point{278.0f, 164.0f};
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, edge_point,
            juce::ModifierKeys::rightButtonModifier, edge_point));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, edge_point, 0, edge_point));
        REQUIRE(delayed_menu_action);

        const auto start = port_point(fixture, "file", "audio", audioalgo::PortDirection::output);
        const auto target = port_point(fixture, "gain-2", "audio_in", audioalgo::PortDirection::input);
        begin_drag(fixture, start);
        REQUIRE(fixture.controller.set_parameter("gain", "gain_db", 4.0).status ==
                audioalgo::editor::ParameterEditStatus::applied);
        delayed_menu_action();
        REQUIRE_FALSE(fixture.errors.empty());
        const auto expected = fixture.controller.project();
        const auto dirty = fixture.controller.dirty();
        const auto stale = fixture.controller.results_stale();
        const auto undo = fixture.controller.history().undo_size();
        const auto redo = fixture.controller.history().redo_size();
        const auto selection = fixture.selection.selected_node_ids();
        const auto errors = fixture.errors.size();
        const auto layout_changes = fixture.layout_changes;
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, target, 0, start, true));
        CHECK(state_unchanged(fixture, expected, dirty, stale, undo, redo, selection, errors, layout_changes));
    }

    SECTION("a deferred menu action is safe after its GraphCanvas has been destroyed") {
        CanvasInteractionFixture fixture;
        std::function<void()> delayed_menu_action;
        auto canvas = std::make_unique<audioalgo::editor::GraphCanvas>(
            fixture.controller.project(), fixture.registry, fixture.selection, fixture.controller,
            [](std::string) {}, [] {}, [&](std::string message) { fixture.errors.push_back(std::move(message)); },
            [](std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
               std::optional<audioalgo::Connection>) {},
            [](juce::Component*) -> std::optional<std::uint64_t> { return std::nullopt; });
        canvas->setSize(1000, 600);
        canvas->setVisible(true);
        canvas->set_connection_context_menu_handler(
            [&](std::string, std::function<void()> action) { delayed_menu_action = std::move(action); });
        const juce::Point<float> edge_point{278.0f, 164.0f};
        canvas->mouseDown(canvas_mouse(*canvas, edge_point,
            juce::ModifierKeys::rightButtonModifier, edge_point));
        canvas->mouseUp(canvas_mouse(*canvas, edge_point, 0, edge_point));
        REQUIRE(delayed_menu_action);
        const auto before = fixture.controller.project();
        canvas.reset();
        delayed_menu_action();
        CHECK(same_project(fixture.controller.project(), before));
        CHECK(fixture.controller.revision() == before.revision);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK_FALSE(fixture.controller.results_stale());
        CHECK_FALSE(fixture.controller.history().can_undo());
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("Canvas marquee keeps multiple nodes after the production inspector callback",
          "[node_interaction][canvas_regression][NED-06.e]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    fixture.drag({20, 90}, {540, 220}, juce::ModifierKeys::leftButtonModifier);
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
    CHECK(fixture.selection.selected_node_id() == "file");
    fixture.inspector.show_node("file");
    CHECK(fixture.selection.selected_node_ids().size() == 2);
    fixture.inspector.show_node("missing");
    CHECK(fixture.selection.selected_node_ids().size() == 2);
    CHECK(fixture.controller.revision() == 0);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK(fixture.layout_changes == 0);
    fixture.canvas.delete_selected();
    REQUIRE(fixture.controller.project().nodes.size() == 1);
    CHECK(fixture.controller.project().nodes.front().id == "output");
    REQUIRE(fixture.controller.undo());
    CHECK(fixture.controller.project().nodes.size() == 3);
    CHECK(fixture.controller.project().connections.size() == 2);
}

TEST_CASE("Canvas right drag selects before node socket and connection gestures",
          "[node_interaction][canvas_regression][NED-06.e]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    juce::Point<float> from{20, 90};
    juce::Point<float> to{540, 220};
    std::vector<std::string> expected{"file", "gain"};
    SECTION("blank") {}
    SECTION("node title") { from = {80, 115}; }
    SECTION("output socket") { from = {216, 164}; expected = {"gain"}; }
    SECTION("input socket") { from = {340, 164}; to = {20, 220}; expected = {"file"}; }
    SECTION("connection") { from = {278, 164}; to = {20, 90}; expected = {"file"}; }
    SECTION("reverse both axes") { from = {540, 220}; to = {20, 90}; }
    SECTION("reverse horizontal") { from = {540, 90}; to = {20, 220}; }
    SECTION("reverse vertical") { from = {20, 220}; to = {540, 90}; }
    const auto before = fixture.controller.project();
    fixture.drag(from, to, juce::ModifierKeys::rightButtonModifier);
    CHECK(fixture.selection.selected_node_ids() == expected);
    CHECK(fixture.controller.revision() == 0);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK(fixture.controller.project().connections.size() == before.connections.size());
    for (std::size_t index = 0; index < before.nodes.size(); ++index) {
        CHECK(fixture.controller.project().nodes[index].x == before.nodes[index].x);
        CHECK(fixture.controller.project().nodes[index].y == before.nodes[index].y);
    }
    CHECK(fixture.layout_changes == 0);
    CHECK(fixture.errors.empty());
}

TEST_CASE("Canvas right marquee threshold escape and project switch preserve edit state",
          "[node_interaction][canvas_regression][NED-06.e]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    fixture.canvas.select("gain");
    const juce::Point<float> start{20, 90};
    const auto right = juce::ModifierKeys::rightButtonModifier;
    SECTION("right click keeps selection") { fixture.drag(start, {23, 92}, right); }
    SECTION("escape cancels") {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start, right, start));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, {540, 220}, right, start, true));
        CHECK(fixture.canvas.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {540, 220}, 0, start, true));
    }
    SECTION("project epoch cancels") {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start, right, start));
        fixture.canvas.set_project_epoch(2);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {540, 220}, 0, start, true));
    }
    SECTION("focus loss cancels") {
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start, right, start));
        fixture.canvas.focusLost(juce::Component::focusChangedDirectly);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {540, 220}, 0, start, true));
    }
    SECTION("hiding cancels") {
        fixture.canvas.setVisible(true);
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start, right, start));
        fixture.canvas.setVisible(false);
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {540, 220}, 0, start, true));
    }
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"gain"});
    CHECK(fixture.controller.revision() == 0);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK(fixture.layout_changes == 0);
}

TEST_CASE("Canvas zoomed selection left editing and middle pan share the displayed geometry",
          "[node_interaction][canvas_regression][NED-06.e][NED-08.a]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    for (const auto scale : {0.4f, 1.0f, 1.25f, 2.5f}) {
        INFO(scale);
        CanvasInteractionFixture fixture;
        fixture.canvas.setSize(2200, 700);
        juce::MouseWheelDetails wheel{};
        wheel.deltaY = std::log(scale) / std::log(1.25f);
        fixture.canvas.mouseWheelMove(canvas_mouse(fixture.canvas, {0, 0}, 0, {0, 0}), wheel);
        fixture.drag({10, 10}, {40, 30}, juce::ModifierKeys::middleButtonModifier);
        const auto view = [&](float x, float y) {
            const auto point = fixture.canvas.coordinate_transform().model_to_canvas({x, y});
            return juce::Point<float>{point.x, point.y};
        };
        fixture.drag(view(20, 90), view(540, 220), juce::ModifierKeys::rightButtonModifier);
        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
        CHECK(fixture.controller.revision() == 0);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK(fixture.layout_changes == 0);

        // Verify the real renderer along the entire visible gap, including the
        // region previously hidden inside the incorrectly sized source node.
        juce::Image image(juce::Image::RGB, 2200, 700, true);
        {
            // Native drawing finishes when the Graphics context is released.
            juce::Graphics graphics(image);
            fixture.canvas.paint(graphics);
        }
        const auto first = view(228, 164);
        const auto last = view(328, 164);
        for (int x = static_cast<int>(std::ceil(first.x)); x <= static_cast<int>(last.x); ++x)
            CHECK(image.getPixelAt(x, juce::roundToInt(first.y)).getGreen() > 90);
        if (const auto* artifact_root = std::getenv("AUDIOALGO_CANVAS_ARTIFACT_OUTPUT")) {
            const auto file = juce::File(audioalgo::editor::utf8_string(artifact_root))
                .getChildFile("canvas-" + juce::String(scale, 2) + ".png");
            REQUIRE(file.getParentDirectory().createDirectory().wasOk());
            juce::FileOutputStream stream(file);
            REQUIRE(stream.openedOk());
            REQUIRE(juce::PNGImageFormat().writeImageToStream(image, stream));
        }

        fixture.drag(view(350, 110), view(370, 310), juce::ModifierKeys::leftButtonModifier);
        CHECK(std::abs(fixture.controller.project().nodes[1].x - 360.0) < 0.001);
        CHECK(std::abs(fixture.controller.project().nodes[1].y - 300.0) < 0.001);
        CHECK(fixture.controller.revision() == 0);
        REQUIRE(fixture.controller.disconnect({"file", "audio", "gain", "audio_in"}).status ==
                audioalgo::editor::ProjectEditStatus::applied);
        fixture.drag(view(216, 164), view(360, 364), juce::ModifierKeys::leftButtonModifier);
        CHECK(fixture.controller.project().connections.size() == 2);
        CHECK(fixture.controller.revision() == 2);
        CHECK(fixture.errors.empty());
    }
}

TEST_CASE("Canvas node drag crosses the old top left limits after zoom and pan",
          "[node_interaction][canvas_regression][canvas_unbounded_drag][NED-06.d][NED-06.e]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    for (const auto scale : {0.4f, 1.0f, 1.25f, 2.5f}) {
        for (const auto target : {juce::Point<float>{4, 20}, juce::Point<float>{0, 0},
                                  juce::Point<float>{-80, -60}}) {
            INFO("scale=" << scale << ", target=" << target.x << "," << target.y);
            CanvasInteractionFixture fixture;
            fixture.canvas.setSize(2200, 1200);
            juce::MouseWheelDetails wheel{};
            wheel.deltaY = std::log(scale) / std::log(1.25f);
            fixture.canvas.mouseWheelMove(canvas_mouse(fixture.canvas, {0, 0}, 0, {0, 0}), wheel);
            fixture.drag({10, 10}, {410, 410}, juce::ModifierKeys::middleButtonModifier);
            const auto view = [&](float x, float y) {
                const auto point = fixture.canvas.coordinate_transform().model_to_canvas({x, y});
                return juce::Point<float>{point.x, point.y};
            };
            const auto results_stale = fixture.controller.results_stale();
            fixture.drag(view(50, 110), view(target.x + 10, target.y + 10),
                         juce::ModifierKeys::leftButtonModifier);
            CHECK(std::abs(fixture.controller.project().nodes[0].x - target.x) < 0.001);
            CHECK(std::abs(fixture.controller.project().nodes[0].y - target.y) < 0.001);
            CHECK(fixture.controller.revision() == 0);
            CHECK(fixture.controller.results_stale() == results_stale);
            CHECK(fixture.controller.dirty());
            CHECK(fixture.layout_changes == 1);
            CHECK(fixture.controller.project().connections.size() == 2);
            const auto connection = audioalgo::editor::make_connection_geometry(
                fixture.controller.project(), fixture.registry, fixture.controller.project().connections[0],
                fixture.canvas.coordinate_transform());
            REQUIRE(connection.has_value());
            const auto output = view(target.x + 176, target.y + 64);
            CHECK(std::abs(connection->start.x - output.x) < 0.001);
            CHECK(std::abs(connection->start.y - output.y) < 0.001);
            REQUIRE(fixture.controller.undo());
            CHECK(fixture.controller.project().nodes[0].x == 40);
            CHECK(fixture.controller.project().nodes[0].y == 100);
            REQUIRE(fixture.controller.redo());
            CHECK(std::abs(fixture.controller.project().nodes[0].x - target.x) < 0.001);
            CHECK(std::abs(fixture.controller.project().nodes[0].y - target.y) < 0.001);
            CHECK(fixture.controller.revision() == 0);
            CHECK(fixture.errors.empty());
        }
    }
}

TEST_CASE("AR-10.c numeric text rejects malformed and out of range input then recovers",
          "[AR-10.c][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto descriptor = parameter_binding_descriptor();
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::ProjectModel project;
    project.project_id = "validation-project";
    project.revision = 4;
    project.nodes = {{"node-a", descriptor.type_id, {{"level", 3.0}}, 0.0, 0.0},
                     {"node-b", descriptor.type_id, {{"level", 7.0}}, 0.0, 0.0}};
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    int notifications = 0;
    audioalgo::editor::NodeInspector inspector(controller, selection, [&] { ++notifications; });
    inspector.set_descriptor_registry(registry);
    inspector.show_node("node-a");
    auto* slider = descendant<juce::Slider>(inspector, "parameter.level");
    REQUIRE(slider != nullptr);
    juce::Label* value_box = nullptr;
    for (auto* child : slider->getChildren())
        if (auto* label = dynamic_cast<juce::Label*>(child))
            value_box = label;
    REQUIRE(value_box != nullptr);

    for (const auto* invalid : {"banana", "11", "NaN", "2.5junk", "-1"}) {
        INFO(invalid);
        value_box->setText(invalid, juce::sendNotificationSync);
        CHECK(parameter_value(controller.project(), "node-a") == 3.0);
        CHECK(parameter_value(controller.project(), "node-b") == 7.0);
        CHECK(controller.revision() == 4);
        CHECK_FALSE(controller.history().can_undo());
        CHECK(notifications == 0);
        auto* error = descendant<juce::Label>(inspector, "parameter.error");
        REQUIRE(error != nullptr);
        CHECK(error->getText().contains("level"));
        CHECK_FALSE(error->getText().isEmpty());
    }

    value_box->setText("4.5 dB", juce::sendNotificationSync);
    CHECK(parameter_value(controller.project(), "node-a") == 4.5);
    CHECK(controller.revision() == 5);
    CHECK(notifications == 1);
    auto* error = descendant<juce::Label>(inspector, "parameter.error");
    REQUIRE(error != nullptr);
    CHECK(error->getText().isEmpty());
    REQUIRE(controller.undo());
    CHECK(parameter_value(controller.project(), "node-a") == 3.0);
}

TEST_CASE("AR-10.b parameter controls stay bound to the selected node across panel rebuilds",
          "[AR-10.b][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto descriptor = parameter_binding_descriptor();
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor(descriptor));

    audioalgo::ProjectModel project;
    project.project_id = "parameter-binding-project";
    project.name = "Parameter binding project";
    project.revision = 4;
    project.nodes = {
        {"node-a", descriptor.type_id, {{"level", 2.0}}, 10.0, 20.0},
        {"node-b", descriptor.type_id, {{"level", 7.0}}, 210.0, 20.0},
    };

    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    const auto node_a = controller.node_snapshot("node-a");
    const auto node_b = controller.node_snapshot("node-b");
    REQUIRE(node_a.has_value());
    REQUIRE(node_b.has_value());

    const audioalgo::editor::EditorNodeParameterBinding binding_a(controller, *node_a);
    audioalgo::editor::ParameterPanel panel;
    std::string selected_node_id = "node-a";
    std::optional<audioalgo::editor::ParameterEditOutcome> last_outcome;
    std::optional<std::string> active_gesture_parameter;
    audioalgo::editor::ParameterPanel::ParameterGestureChanged stale_node_a_gesture;

    const auto show = [&](const audioalgo::editor::NodeViewSnapshot& snapshot,
                          const audioalgo::editor::EditorNodeParameterBinding& binding) {
        auto on_gesture = [&, binding](std::string parameter_id,
                                      audioalgo::editor::ParameterGesturePhase phase,
                                      audioalgo::ParameterValue value) {
            if (!binding.matches(controller.node_snapshot(selected_node_id)))
                return;
            if (phase == audioalgo::editor::ParameterGesturePhase::started) {
                const auto outcome = controller.begin_parameter_gesture(
                    binding.session(), binding.node_id(), parameter_id);
                last_outcome = outcome;
                if (outcome.status == audioalgo::editor::ParameterEditStatus::started)
                    active_gesture_parameter = std::move(parameter_id);
            } else if (phase == audioalgo::editor::ParameterGesturePhase::updated &&
                       active_gesture_parameter && *active_gesture_parameter == parameter_id) {
                last_outcome = controller.update_parameter_gesture(binding.session(), std::move(value));
            } else if (phase == audioalgo::editor::ParameterGesturePhase::committed &&
                       active_gesture_parameter && *active_gesture_parameter == parameter_id) {
                last_outcome = controller.commit_parameter_gesture(binding.session());
                active_gesture_parameter.reset();
            } else if (phase == audioalgo::editor::ParameterGesturePhase::updated) {
                last_outcome = binding.set_parameter_if_current(
                    controller.node_snapshot(selected_node_id), std::move(parameter_id), std::move(value));
            }
        };
        if (snapshot.node.id == "node-a")
            stale_node_a_gesture = on_gesture;
        panel.show_parameters(descriptor, snapshot.node.parameters,
            [&, binding](std::string parameter_id, audioalgo::ParameterValue value) {
                last_outcome = binding.set_parameter_if_current(
                    controller.node_snapshot(selected_node_id), std::move(parameter_id), std::move(value));
            }, std::move(on_gesture));
    };

    show(*node_a, binding_a);
    auto* node_a_level = find_control<juce::Slider>(panel, "parameter.level");
    REQUIRE(node_a_level != nullptr);
    CHECK(node_a_level->getValue() == 2.0);
    node_a_level->onDragStart();
    node_a_level->setValue(3.5, juce::sendNotificationSync);
    node_a_level->onDragEnd();
    REQUIRE(last_outcome.has_value());
    CHECK(last_outcome->status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(parameter_value(controller.project(), "node-a") == 3.5);
    CHECK(parameter_value(controller.project(), "node-b") == 7.0);

    selected_node_id = "node-b";
    const auto current_node_b = controller.node_snapshot("node-b");
    REQUIRE(current_node_b.has_value());
    const audioalgo::editor::EditorNodeParameterBinding binding_b(controller, *current_node_b);
    last_outcome.reset();
    show(*current_node_b, binding_b);

    auto* rebuilt_node_b_level = find_control<juce::Slider>(panel, "parameter.level");
    REQUIRE(rebuilt_node_b_level != nullptr);
    CHECK(rebuilt_node_b_level->getValue() == 7.0);
    CHECK_FALSE(last_outcome.has_value());
    rebuilt_node_b_level->onDragStart();
    rebuilt_node_b_level->setValue(8.5, juce::sendNotificationSync);
    rebuilt_node_b_level->onDragEnd();
    REQUIRE(last_outcome.has_value());
    CHECK(last_outcome->status == audioalgo::editor::ParameterEditStatus::applied);
    CHECK(parameter_value(controller.project(), "node-a") == 3.5);
    CHECK(parameter_value(controller.project(), "node-b") == 8.5);

    const auto revision_before_stale_callback = controller.revision();
    REQUIRE(stale_node_a_gesture);
    stale_node_a_gesture("level", audioalgo::editor::ParameterGesturePhase::updated, 9.5);
    const auto stale_edit = binding_a.set_parameter_if_current(
        controller.node_snapshot("node-b"), "level", 9.5);
    CHECK_FALSE(stale_edit.has_value());
    CHECK(controller.revision() == revision_before_stale_callback);
    CHECK(parameter_value(controller.project(), "node-a") == 3.5);
    CHECK(parameter_value(controller.project(), "node-b") == 8.5);
}

TEST_CASE("AR-10.d reset defaults uses controller history and refreshes typed controls",
          "[AR-10.d][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    auto descriptor = parameter_binding_descriptor();
    descriptor.parameters.push_back({"enabled", audioalgo::ParameterKind::boolean, false, {}, {}, {}});
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::ProjectModel project;
    project.project_id = "reset-project";
    project.revision = 4;
    project.nodes = {{"node-a", descriptor.type_id, {{"level", 3.0}, {"enabled", true}}, 0.0, 0.0},
                     {"node-b", descriptor.type_id, {{"level", 7.0}, {"enabled", true}}, 0.0, 0.0}};
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    int notifications = 0;
    audioalgo::editor::NodeInspector inspector(controller, selection, [&] { ++notifications; });
    inspector.set_descriptor_registry(registry);
    inspector.show_node("node-a");
    auto* reset = descendant<juce::TextButton>(inspector, "reset.level");
    REQUIRE(reset != nullptr);
    reset->onClick();
    CHECK(parameter_value(controller.project(), "node-a") == 0.0);
    CHECK(parameter_value(controller.project(), "node-b") == 7.0);
    auto* level = descendant<juce::Slider>(inspector, "parameter.level");
    REQUIRE(level != nullptr);
    CHECK(level->getValue() == 0.0);
    CHECK(level->getTextValueSuffix() == " dB");
    CHECK(controller.revision() == 5);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(notifications == 1);
    reset->onClick();
    CHECK(controller.revision() == 5);
    CHECK(notifications == 1);
    REQUIRE(controller.undo());
    inspector.show_node("node-a");
    CHECK(descendant<juce::Slider>(inspector, "parameter.level")->getValue() == 3.0);
    REQUIRE(controller.redo());
    inspector.show_node("node-a");
    CHECK(descendant<juce::Slider>(inspector, "parameter.level")->getValue() == 0.0);
    CHECK(controller.revision() == 7);

    auto* reset_enabled = descendant<juce::TextButton>(inspector, "reset.enabled");
    REQUIRE(reset_enabled != nullptr);
    reset_enabled->onClick();
    CHECK_FALSE(std::get<bool>(controller.project().nodes.front().parameters.at("enabled")));
    CHECK_FALSE(descendant<juce::ToggleButton>(inspector, "parameter.enabled")->getToggleState());
    REQUIRE(controller.undo());
    inspector.show_node("node-a");
    CHECK(descendant<juce::ToggleButton>(inspector, "parameter.enabled")->getToggleState());
    CHECK(controller.revision() == 9);
}

TEST_CASE("AR-10.g six production controls drive native audio and the recorded run snapshot",
          "[AR-10.g][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto root = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        ("parameter-probe-" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{root};
    const auto path_string = [](const std::filesystem::path& p) {
        const auto bytes = p.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    { std::ofstream stream(root / "quarter.txt", std::ios::binary); stream << "0.25\n"; }
    { std::ofstream stream(root / "half.txt", std::ios::binary); stream << "0.5\n"; }
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t i = 0; i < input.frame_count(); ++i) {
        input.mutable_channel(0)[i] = static_cast<float>(0.2 * std::sin(i * 0.03));
        input.mutable_channel(1)[i] = static_cast<float>(-0.15 + 0.0003 * i);
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    const auto descriptor = audioalgo::tests::parameter_probe_descriptor();
    REQUIRE(registry.register_node(descriptor, [] { return std::make_unique<audioalgo::tests::ParameterProbeNode>(); }).registered);
    audioalgo::NodeParameters parameters;
    for (const auto& p : descriptor.parameters)
        parameters[p.id] = p.default_value;
    parameters["coefficient"] = path_string(root / "quarter.txt");
    audioalgo::ProjectModel project;
    project.project_id = "six-control-project";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 0, 0},
                     {"probe", descriptor.type_id, parameters, 200, 0},
                     {"output", "builtin.output", {}, 400, 0}};
    project.connections = {{"file", "audio", "probe", "in"}, {"probe", "out", "output", "audio"}};
    project.selected_output_node_id = "output";
    const auto project_file = root / "probe.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file, project)));
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller, selection, [] {});
    inspector.set_descriptor_registry(registry);
    inspector.show_node("probe");
    audioalgo::RunService service(registry);
    std::string last_output_hash;
    const auto verify_run = [&](const std::string& name, double expected_factor, const std::string& expected_tag) {
        INFO(name);
        const auto result = service.run({controller.project(), project_file, root / name, 127});
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&result))
            INFO(error->code + ": " + error->message);
        const auto* success = std::get_if<audioalgo::RunSuccess>(&result);
        REQUIRE(success != nullptr);
        const auto read = audioalgo::read_wav_file(success->output_file);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
        const auto& output = std::get<audioalgo::AudioBuffer>(read);
        CHECK(output.channel_count() == 2);
        REQUIRE(output.frame_count() == 513);
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t frame = 0; frame < 513; ++frame) {
                const double expected = static_cast<double>(input.channel(channel)[frame]) * expected_factor;
                REQUIRE(std::abs(output.channel(channel)[frame] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
            }
        const auto snapshot = audioalgo::load_project_file(root / name / "project.aaproj");
        REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(snapshot));
        const auto& frozen = std::get<audioalgo::ProjectModel>(snapshot);
        CHECK(frozen.nodes[1].parameters == controller.project().nodes[1].parameters);
        CHECK(std::get<std::string>(frozen.nodes[1].parameters.at("tag")) == expected_tag);
        const auto manifest = juce::JSON::parse(juce::File(audioalgo::editor::utf8_string(
            path_string(success->manifest_file))).loadFileAsString());
        CHECK(manifest["project_snapshot"].toString() == "project.aaproj");
        const auto* resources = manifest["resources"].getArray();
        REQUIRE(resources != nullptr);
        bool found_coefficient = false;
        for (const auto& resource : *resources) {
            if (resource["node_id"].toString() == "probe" && resource["parameter_id"].toString() == "coefficient") {
                found_coefficient = true;
                const auto expected_sha = name == "coefficient"
                    ? "8d5c1b5a87c51f970807fc0c2057b3ab3aaf11638ab667dc5956edc8f5bcf138"
                    : "7747240b40ef7064f499d4ddd256767cba251ce7c2cc4b26faf542d9a2c5c104";
                CHECK(resource["sha256"].toString() == expected_sha);
            }
        }
        CHECK(found_coefficient);
        if (name == "tag")
            CHECK(success->output_sha256 == last_output_hash);
        last_output_hash = success->output_sha256;
    };
    verify_run("initial", 1.0, "initial");
    auto* enabled = descendant<juce::ToggleButton>(inspector, "parameter.enabled");
    REQUIRE(enabled != nullptr);
    enabled->setToggleState(true, juce::dontSendNotification);
    enabled->onClick();
    verify_run("enabled", -0.375, "initial");
    descendant<juce::Slider>(inspector, "parameter.repeat")->setValue(4, juce::sendNotificationSync);
    verify_run("repeat", -0.5, "initial");
    descendant<juce::Slider>(inspector, "parameter.scale")->setValue(1.5, juce::sendNotificationSync);
    verify_run("scale", -1.5, "initial");
    descendant<juce::ComboBox>(inspector, "parameter.polarity")->setSelectedId(2, juce::sendNotificationSync);
    verify_run("polarity", 1.5, "initial");
    auto* tag = descendant<juce::TextEditor>(inspector, "parameter.tag");
    tag->setText("report only", false);
    tag->onReturnKey();
    verify_run("tag", 1.5, "report only");
    auto* coefficient = descendant<juce::TextEditor>(inspector, "parameter.coefficient");
    coefficient->setText(audioalgo::editor::utf8_string(path_string(root / "half.txt")), false);
    coefficient->onReturnKey();
    verify_run("coefficient", 3.0, "report only");
    if (const auto* target = std::getenv("AUDIOALGO_PARAMETER_ARTIFACT_OUTPUT"))
        std::filesystem::copy(root, std::filesystem::u8path(target), std::filesystem::copy_options::recursive);
}

TEST_CASE("AR-10.c backend bypass rejects invalid types enums and missing resources with context",
          "[AR-10.c][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto root = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        ("parameter-errors-" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{root};
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 1);
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    const auto descriptor = audioalgo::tests::parameter_probe_descriptor();
    REQUIRE(registry.register_node(descriptor, [] { return std::make_unique<audioalgo::tests::ParameterProbeNode>(); }).registered);
    audioalgo::NodeParameters parameters;
    for (const auto& p : descriptor.parameters)
        parameters[p.id] = p.default_value;
    audioalgo::ProjectModel project;
    project.project_id = "parameter-errors";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 0, 0},
                     {"probe", descriptor.type_id, parameters, 200, 0},
                     {"output", "builtin.output", {}, 400, 0}};
    project.connections = {{"file", "audio", "probe", "in"}, {"probe", "out", "output", "audio"}};
    project.selected_output_node_id = "output";
    const auto project_file = root / "probe.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file, project)));
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller, selection, [] {});
    inspector.set_descriptor_registry(registry);
    inspector.show_node("probe");
    inspector.setEnabled(false);
    audioalgo::RunService service(registry);
    struct Invalid { std::string id; audioalgo::ParameterValue value; std::string code; };
    const std::vector<Invalid> invalids = {
        {"repeat", std::int64_t{0}, "parameter_out_of_range"},
        {"repeat", std::string("banana"), "parameter_type"},
        {"scale", std::numeric_limits<double>::infinity(), "parameter_non_finite"},
        {"scale", -1.0, "parameter_out_of_range"},
        {"polarity", std::string("unknown"), "parameter_enum_unknown"},
        {"tag", 1.0, "parameter_type"}, {"coefficient", false, "parameter_type"}};
    int index = 0;
    for (const auto& invalid : invalids) {
        INFO(invalid.id);
        const auto edit = controller.set_parameter("probe", invalid.id, invalid.value);
        REQUIRE(edit.status == audioalgo::editor::ParameterEditStatus::rejected);
        REQUIRE(edit.diagnostic.has_value());
        CHECK(edit.diagnostic->code == invalid.code);
        CHECK(edit.diagnostic->node_id == "probe");
        CHECK(edit.diagnostic->port_id == invalid.id);
        CHECK(controller.project().nodes[1].parameters == parameters);
        CHECK(controller.revision() == 0);
        CHECK_FALSE(controller.history().can_undo());
        auto bypass = project;
        bypass.nodes[1].parameters[invalid.id] = invalid.value;
        const auto directory = root / ("invalid-" + std::to_string(++index));
        const auto result = service.run({bypass, project_file, directory, 127});
        const auto* error = std::get_if<audioalgo::Diagnostic>(&result);
        REQUIRE(error != nullptr);
        CHECK(error->code == invalid.code);
        CHECK(error->node_id == "probe");
        CHECK(error->port_id == invalid.id);
        CHECK_FALSE(std::filesystem::exists(directory / "complete.json"));
    }
    for (const auto& node_id : {std::string("file"), std::string("probe")}) {
        const auto node_index = node_id == "file" ? 0u : 1u;
        const auto parameter = node_id == "file" ? "path" : "coefficient";
        for (const auto& reference : {std::string{}, std::string("missing.txt")}) {
            auto missing = project;
            missing.nodes[node_index].parameters[parameter] = reference;
            const auto directory = root / ("missing-" + std::to_string(++index));
            const auto result = service.run({missing, project_file, directory, 127});
            const auto* error = std::get_if<audioalgo::Diagnostic>(&result);
            REQUIRE(error != nullptr);
            CHECK(error->node_id == node_id);
            CHECK(error->port_id == parameter);
            CHECK_FALSE(error->message.empty());
            CHECK_FALSE(std::filesystem::exists(directory / "complete.json"));
        }
    }
}

TEST_CASE("AR-10.e declarative conditions refresh controls and opaque structures remain read only",
          "[AR-10.e][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    auto descriptor = parameter_binding_descriptor();
    descriptor.parameters[0].enabled_if = audioalgo::ParameterCondition{"enabled", true};
    descriptor.parameters.push_back({"enabled", audioalgo::ParameterKind::boolean, false, {}, {}, {}});
    audioalgo::ParameterDescriptor detail{"detail", audioalgo::ParameterKind::string, std::string("retained"), {}, {}, {}};
    detail.visible_if = audioalgo::ParameterCondition{"enabled", true};
    descriptor.parameters.push_back(detail);
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::ProjectModel project;
    project.project_id = "conditions-and-opaque";
    project.nodes = {{"node-a", descriptor.type_id, {{"level", 3.0}, {"enabled", false}, {"detail", std::string("retained")}}, 0, 0}};
    const auto opaque_json = std::string("{\"bands\":[1,2],\"note\":\"keep\"}");
    project.nodes.front().opaque_parameters.push_back({"settings", opaque_json});
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller, selection, [] {});
    inspector.set_descriptor_registry(registry);
    inspector.show_node("node-a");
    auto* level = descendant<juce::Slider>(inspector, "parameter.level");
    auto* detail_control = descendant<juce::TextEditor>(inspector, "parameter.detail");
    REQUIRE(level != nullptr);
    REQUIRE(detail_control != nullptr);
    CHECK_FALSE(level->isEnabled());
    CHECK_FALSE(detail_control->isVisible());
    auto* opaque = descendant<juce::TextEditor>(inspector, "opaque.settings");
    REQUIRE(opaque != nullptr);
    CHECK(opaque->isReadOnly());
    CHECK(opaque->getText().toStdString() == opaque_json);
    auto* explanation = descendant<juce::Label>(inspector, "opaque-label.settings");
    REQUIRE(explanation != nullptr);
    CHECK(explanation->getText().contains("read-only"));
    auto* enabled = descendant<juce::ToggleButton>(inspector, "parameter.enabled");
    REQUIRE(enabled != nullptr);
    enabled->setToggleState(true, juce::dontSendNotification);
    enabled->onClick();
    CHECK(level->isEnabled());
    CHECK(detail_control->isVisible());
    CHECK(level->getValue() == 3.0);
    CHECK(controller.project().nodes.front().opaque_parameters == project.nodes.front().opaque_parameters);
    REQUIRE(controller.undo());
    inspector.show_node("node-a");
    CHECK_FALSE(descendant<juce::Slider>(inspector, "parameter.level")->isEnabled());
    CHECK_FALSE(descendant<juce::TextEditor>(inspector, "parameter.detail")->isVisible());
    CHECK(controller.project().nodes.front().opaque_parameters == project.nodes.front().opaque_parameters);

    auto invalid = descriptor;
    invalid.type_id = "test.invalid_condition_reference";
    invalid.parameters.front().enabled_if->parameter_id = "missing";
    const auto rejected = registry.register_node(invalid, [] { return std::make_unique<audioalgo::tests::ParameterProbeNode>(); });
    CHECK_FALSE(rejected.registered);
    REQUIRE(rejected.diagnostic.has_value());
    CHECK(rejected.diagnostic->field.find("enabled_if") != std::string::npos);
    CHECK(registry.find(invalid.type_id) == nullptr);
    invalid.type_id = "test.invalid_condition_value";
    invalid.parameters.front().enabled_if = audioalgo::ParameterCondition{"enabled", 1.0};
    const auto wrong_type = registry.register_node(invalid, [] { return std::make_unique<audioalgo::tests::ParameterProbeNode>(); });
    CHECK_FALSE(wrong_type.registered);
    REQUIRE(wrong_type.diagnostic.has_value());
    CHECK(wrong_type.diagnostic->field.find("enabled_if") != std::string::npos);

    const auto directory = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        ("opaque-save-" + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto file = directory / "opaque.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(file, controller.project())));
    const auto loaded = audioalgo::load_project_file(file);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    const auto& fields = std::get<audioalgo::ProjectModel>(loaded).nodes.front().opaque_parameters;
    REQUIRE(fields.size() == 1);
    CHECK(fields.front().name == "settings");
    const auto structure = juce::JSON::parse(audioalgo::editor::utf8_string(fields.front().value_json));
    CHECK(structure["note"].toString() == "keep");
    REQUIRE(structure["bands"].isArray());
    CHECK(structure["bands"].getArray()->size() == 2);
    CHECK(static_cast<int>((*structure["bands"].getArray())[0]) == 1);
    CHECK(static_cast<int>((*structure["bands"].getArray())[1]) == 2);
}

TEST_CASE("AR-11.a separated view components retain controller intents and presentation state",
          "[AR-11.a][R3]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    audioalgo::NodeRegistry registry;
    const auto descriptor = parameter_binding_descriptor();
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::editor::EditorController controller(registry);
    audioalgo::ProjectModel project;
    project.project_id = "separated-components";
    controller.adopt_project(project);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller, selection, [] {});
    inspector.set_descriptor_registry(registry);
    std::optional<audioalgo::editor::ProjectEditOutcome> add;
    audioalgo::editor::NodeLibrary library(registry, [&](std::string type) {
        add = controller.add_node(type, "library-created", 24, 36);
    }, [] { return std::uint64_t{1}; });
    library.setSize(164, 500);
    auto* button = descendant<audioalgo::editor::NodeLibraryButton>(library, "library." +
        audioalgo::editor::utf8_string(descriptor.type_id));
    REQUIRE(button != nullptr);
    CHECK(button->getButtonText().toStdString() == descriptor.display_name);
    button->onClick();
    REQUIRE(add.has_value());
    REQUIRE(add->status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.project().nodes.size() == 1);
    CHECK(controller.project().nodes.front().parameters.at("level") == audioalgo::ParameterValue{0.0});
    audioalgo::editor::GraphCanvas canvas(controller.project(), registry, selection, controller,
        [&](std::string id) { inspector.show_node(id); }, [] {}, [](std::string) {},
        [](std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
           std::optional<audioalgo::Connection>) {},
        [](juce::Component*) -> std::optional<std::uint64_t> { return std::nullopt; });
    canvas.setSize(640, 360);
    canvas.select("library-created");
    CHECK(selection.selected_node_id() == "library-created");
    auto* level = descendant<juce::Slider>(inspector, "parameter.level");
    REQUIRE(level != nullptr);
    level->setValue(3.5, juce::sendNotificationSync);
    CHECK(parameter_value(controller.project(), "library-created") == 3.5);
    CHECK(controller.revision() == 2);
    REQUIRE(controller.undo());
    canvas.select("library-created");
    CHECK(descendant<juce::Slider>(inspector, "parameter.level")->getValue() == 0.0);
    juce::Image canvas_image(juce::Image::RGB, 640, 360, true);
    juce::Graphics canvas_graphics(canvas_image);
    canvas.paint(canvas_graphics);

    audioalgo::AudioBuffer audio(48000, {"left"}, 5);
    const std::vector<float> samples{0.0f, -1.0f, 2.0f, -0.5f, 0.1f};
    std::copy(samples.begin(), samples.end(), audio.mutable_channel(0).begin());
    const auto ranges = audioalgo::editor::make_waveform(audio, 2);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0].size() == 2);
    CHECK(ranges[0][0].minimum == -1.0f);
    CHECK(ranges[0][0].maximum == 0.0f);
    CHECK(ranges[0][1].minimum == -0.5f);
    CHECK(ranges[0][1].maximum == 2.0f);
    audioalgo::editor::WaveformView waveform;
    waveform.setSize(320, 160);
    waveform.set_waveform(ranges);
    juce::Image waveform_image(juce::Image::RGB, 320, 160, true);
    juce::Graphics waveform_graphics(waveform_image);
    waveform.paint(waveform_graphics);
    audioalgo::editor::RunStatusPanel status;
    status.set_status("Missing input", true);
    CHECK(status.status_text() == "Missing input");
    status.set_status("Rendered current request", false);
    CHECK(status.status_text() == "Rendered current request");
}

TEST_CASE("NI0017 waveform wheel reveals detail and double click restores overview", "[NI0017][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::AudioBuffer audio(48000, {"left", "right"}, 8192);
    for (std::size_t n = 0; n < audio.frame_count(); ++n) {
        audio.mutable_channel(0)[n] = .8f * std::sin(static_cast<float>(n) * .17f);
        audio.mutable_channel(1)[n] = n == 2301 ? -1.f : 0.f;
    }
    audioalgo::editor::WaveformView view;
    view.setSize(640, 240);
    const auto ranges = audioalgo::editor::make_waveform(audio);
    REQUIRE(ranges.detail);
    CHECK(ranges.detail->frames == 8192);
    CHECK(ranges.detail->sample_rate_hz == 48000);
    CHECK(ranges.detail->finest_bucket_frames == 1);
    CHECK(audioalgo::editor::waveform_visible_range(ranges, 1, 2301, 2302, 1).minimum == -1.f);
    CHECK(audioalgo::editor::waveform_visible_range(ranges, 1, 2302, 2303, 1).minimum == 0.f);
    view.set_waveform(ranges);
    const auto image_hash = [&] {
        juce::MemoryOutputStream stream;
        juce::PNGImageFormat{}.writeImageToStream(view.createComponentSnapshot(view.getLocalBounds()), stream);
        return juce::SHA256(stream.getData(), stream.getDataSize()).toHexString();
    };
    const auto preview_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NI0017-waveform-zoom/artifacts" /
                              juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(preview_root);
    std::cout << "NI0017_PREVIEW_ROOT=" << preview_root.generic_string() << '\n';
    const auto save_preview = [&](const char* name) {
        juce::FileOutputStream stream(juce::File((preview_root / name).generic_string()));
        REQUIRE(stream.openedOk());
        CHECK(juce::PNGImageFormat{}.writeImageToStream(view.createComponentSnapshot(view.getLocalBounds()), stream));
    };
    const auto before = image_hash();
    save_preview("overview.png");
    CHECK(view.visible_frames() == std::make_pair(0.0, 8192.0));
    juce::MouseWheelDetails wheel{}; wheel.deltaY = 1.f;
    view.mouseWheelMove(canvas_mouse(view, {180, 100}, 0, {180, 100}), wheel);
    const auto zoomed = image_hash();
    save_preview("zoomed.png");
    CHECK(zoomed != before);
    const auto zoom_frames = view.visible_frames();
    CHECK(zoom_frames.second - zoom_frames.first == 1024.0);
    const auto anchor = (180.0 - 14) / (640.0 - 28);
    CHECK(zoom_frames.first + anchor * 1024.0 == Catch::Approx(anchor * 8192.0));
    view.mouseDown(canvas_mouse(view, {320, 100}, juce::ModifierKeys::leftButtonModifier, {320, 100}));
    view.mouseDrag(canvas_mouse(view, {240, 100}, juce::ModifierKeys::leftButtonModifier, {320, 100}, true));
    view.mouseUp(canvas_mouse(view, {240, 100}, 0, {320, 100}, true));
    CHECK(image_hash() != zoomed);
    save_preview("panned.png");
    CHECK(view.visible_frames().first - zoom_frames.first == Catch::Approx(80.0 * 1024.0 / 612.0));
    view.mouseDoubleClick(canvas_mouse(view, {180, 100}, juce::ModifierKeys::leftButtonModifier, {180, 100}));
    CHECK(image_hash() == before);
    CHECK(view.visible_frames() == std::make_pair(0.0, 8192.0));
    wheel.deltaY = 100.f;
    view.mouseWheelMove(canvas_mouse(view, {320, 100}, 0, {320, 100}), wheel);
    CHECK(view.visible_frames().second - view.visible_frames().first == 8.0);
    wheel.deltaY = -100.f;
    view.mouseWheelMove(canvas_mouse(view, {320, 100}, 0, {320, 100}), wheel);
    CHECK(view.visible_frames() == std::make_pair(0.0, 8192.0));
    view.set_waveform({});
    CHECK(view.visible_frames() == std::make_pair(0.0, 0.0));
}

TEST_CASE("NI0017 bounded zoom cache preserves isolated peaks across levels", "[NI0017][component]") {
    audioalgo::AudioBuffer audio(48000, {"L", "R"}, 300000);
    audio.mutable_channel(0)[199999] = .75f;
    audio.mutable_channel(1)[0] = -1.f;
    auto ranges = audioalgo::editor::make_waveform(audio);
    REQUIRE(ranges.detail);
    CHECK(ranges.detail->finest_bucket_frames == 2);
    CHECK(ranges.detail->frames == 300000);
    std::size_t cached_ranges = 0;
    std::size_t bucket_frames = ranges.detail->finest_bucket_frames;
    for (const auto& level : ranges.detail->levels) {
        for (const auto& channel : level) cached_ranges += channel.size();
        CHECK(level[0][199999 / bucket_frames].maximum == .75f);
        bucket_frames *= 2;
    }
    CHECK(cached_ranges <= 1'048'576);
    CHECK(audioalgo::editor::waveform_visible_range(ranges, 1, 0, 2, 1).minimum == -1.f);
    CHECK(audioalgo::editor::waveform_visible_range(ranges, 0, 199999, 200000, 1).maximum == .75f);
}

TEST_CASE("AR-12.d ChannelSwap library controls native rendering and CLI follow the stereo oracle",
          "[AR-12.d][R4][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto root = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        std::filesystem::u8path("Channel Swap 中文 " + juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{root};
    const auto source = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "examples/channel_swap_demo/input.wav";
    std::filesystem::copy_file(source, root / "input.wav");
    const auto input_result = audioalgo::read_wav_file(root / "input.wav");
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(input_result));
    const auto& input = std::get<audioalgo::AudioBuffer>(input_result);
    REQUIRE(input.channel_count() == 2);
    REQUIRE(input.frame_count() == 513);
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    audioalgo::ProjectModel project;
    project.project_id = "component-channel-swap";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 60, 150},
                     {"output", "builtin.output", {}, 620, 150}};
    project.connections = {{"file", "audio", "output", "audio"}};
    project.selected_output_node_id = "output";
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    std::optional<audioalgo::editor::ProjectEditOutcome> created;
    audioalgo::editor::NodeLibrary library(registry, [&](std::string type) {
        created = controller.add_node(type, "swap", 340, 150);
    }, [] { return std::uint64_t{1}; });
    library.setSize(164, 500);
    auto* button = descendant<audioalgo::editor::NodeLibraryButton>(library, "library.example.cpp_channel_swap");
    REQUIRE(button != nullptr);
    button->onClick();
    REQUIRE(created.has_value());
    REQUIRE(created->status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.disconnect(project.connections.front()).status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.connect({"file", "audio", "swap", "audio_in"}).status == audioalgo::editor::ProjectEditStatus::applied);
    REQUIRE(controller.connect({"swap", "audio_out", "output", "audio"}).status == audioalgo::editor::ProjectEditStatus::applied);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller, selection, [] {});
    inspector.set_descriptor_registry(registry);
    inspector.show_node("swap");
    auto* enabled = descendant<juce::ToggleButton>(inspector, "parameter.enabled");
    REQUIRE(enabled != nullptr);
    CHECK(enabled->getButtonText() == "Swap channels");
    CHECK(enabled->getToggleState());
    audioalgo::RunService service(registry);
    const auto verify_audio = [&](const std::filesystem::path& file, bool swap) {
        const auto loaded = audioalgo::read_wav_file(file);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(loaded));
        const auto& output = std::get<audioalgo::AudioBuffer>(loaded);
        REQUIRE(output.frame_count() == input.frame_count());
        REQUIRE(output.channel_count() == 2);
        CHECK(output.channel_ids() == input.channel_ids());
        CHECK(output.valid_interval() == input.valid_interval());
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
                const double expected = input.channel(swap ? 1 - c : c)[frame];
                CHECK(std::abs(output.channel(c)[frame] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
            }
    };
    const auto run = [&](const std::string& name, bool swap) {
        const auto project_file = root / (name + ".aaproj");
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file, controller.project())));
        const auto result = service.run({controller.project(), project_file, root / ("component-" + name), 127});
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&result))
            INFO(error->code + ": " + error->message);
        REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(result));
        verify_audio(std::get<audioalgo::RunSuccess>(result).output_file, swap);
        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
        const auto path_text = [](const std::filesystem::path& path) {
            const auto bytes = path.u8string();
            return juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
        };
        juce::StringArray arguments{executable.getFullPathName(), "run", path_text(project_file), path_text(root / ("cli-" + name)), "127"};
        juce::ChildProcess child;
        REQUIRE(child.start(arguments));
        REQUIRE(child.waitForProcessToFinish(30000));
        INFO(child.readAllProcessOutput().toStdString());
        REQUIRE(child.getExitCode() == 0);
        verify_audio(root / ("cli-" + name) / "output.wav", swap);
    };
    run("enabled", true);
    enabled->setToggleState(false, juce::dontSendNotification);
    enabled->onClick();
    REQUIRE(controller.node_snapshot("swap").has_value());
    CHECK(std::get<bool>(controller.node_snapshot("swap")->node.parameters.at("enabled")) == false);
    run("disabled", false);
    REQUIRE(controller.undo());
    inspector.show_node("swap");
    CHECK(descendant<juce::ToggleButton>(inspector, "parameter.enabled")->getToggleState());
    run("undo-enabled", true);
    auto invalid = controller.project();
    for (auto& node : invalid.nodes)
        if (node.id == "swap")
            node.parameters["enabled"] = std::string("wrong type");
    const auto error_result = service.run({invalid, root / "enabled.aaproj", root / "invalid-parameter", 127});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(error_result));
    CHECK(std::get<audioalgo::Diagnostic>(error_result).code == "parameter_type");
    CHECK(std::get<audioalgo::Diagnostic>(error_result).node_id == "swap");
    CHECK(std::get<audioalgo::Diagnostic>(error_result).port_id == "enabled");
    CHECK_FALSE(std::filesystem::exists(root / "invalid-parameter/complete.json"));
    for (const auto channels : {1u, 4u}) {
        std::vector<std::string> ids;
        for (std::size_t i = 0; i < channels; ++i)
            ids.push_back(std::to_string(i));
        audioalgo::AudioBuffer unsupported(48000, ids, 3);
        const auto input_file = root / ("unsupported-" + std::to_string(channels) + ".wav");
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
            audioalgo::write_wav_file(input_file, unsupported, audioalgo::WavEncoding::float32)));
        auto unsupported_project = controller.project();
        for (auto& node : unsupported_project.nodes)
            if (node.id == "file")
                node.parameters["path"] = input_file.filename().string();
        const auto result = service.run({unsupported_project, root / "enabled.aaproj", root / ("unsupported-run-" + std::to_string(channels)), 127});
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
        CHECK(std::get<audioalgo::Diagnostic>(result).node_id == "swap");
    }
    if (const auto* target = std::getenv("AUDIOALGO_SWAP_ARTIFACT_OUTPUT"))
        std::filesystem::copy(root, std::filesystem::u8path(target), std::filesystem::copy_options::recursive);
}

TEST_CASE("NCE-09.a Right-click threshold opens hit menus and longer drags stay marquee-only",
          "[NCE-09.a][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto install_menu_observers = [](CanvasInteractionFixture& fixture,
                                           std::vector<std::string>& connection_labels,
                                           std::vector<std::vector<std::string>>& port_labels,
                                           std::vector<std::size_t>& port_action_counts) {
        fixture.canvas.set_connection_context_menu_handler(
            [&](std::string label, std::function<void()>) { connection_labels.push_back(std::move(label)); });
        fixture.canvas.set_port_connection_context_menu_handler(
            [&](std::vector<audioalgo::editor::GraphCanvas::PortConnectionMenuItem> items) {
                std::vector<std::string> labels;
                for (const auto& item : items)
                    labels.push_back(item.label);
                port_action_counts.push_back(items.size());
                port_labels.push_back(std::move(labels));
            });
    };
    const auto port_anchor = [](CanvasInteractionFixture& fixture, const std::string& node_id,
                                const std::string& port_id, audioalgo::PortDirection direction) {
        const auto& nodes = fixture.controller.project().nodes;
        const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
            return candidate.id == node_id;
        });
        REQUIRE(node != nodes.end());
        const auto geometry = audioalgo::editor::make_node_geometry(
            *node, fixture.registry.find(node->type_id), fixture.canvas.coordinate_transform());
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(), [&](const auto& candidate) {
            return candidate.id == port_id && candidate.direction == direction;
        });
        REQUIRE(port != geometry.ports.end());
        return juce::Point<float>{port->anchor.x, port->anchor.y};
    };
    const auto right_gesture = [](CanvasInteractionFixture& fixture, juce::Point<float> from,
                                  juce::Point<float> to) {
        const auto right = juce::ModifierKeys::rightButtonModifier;
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, from, right, from));
        fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, to, right, from, true));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, to, 0, from, true));
    };
    const auto verify_marquee_only_drag = [&](juce::Point<float> from, juce::Point<float> to,
                                              std::vector<std::string> expected_selection) {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        const auto before = fixture.controller.project();
        (void)fixture.selection.select_many(before, {"output"});
        std::vector<std::string> connection_labels;
        std::vector<std::vector<std::string>> port_labels;
        std::vector<std::size_t> port_action_counts;
        install_menu_observers(fixture, connection_labels, port_labels, port_action_counts);
        right_gesture(fixture, from, to);
        CHECK(fixture.selection.selected_node_ids() == expected_selection);
        CHECK(fixture.selection_callback_count == 1);
        CHECK(fixture.selection_callback_ids == std::vector<std::string>{expected_selection.front()});
        CHECK(connection_labels.empty());
        CHECK(port_labels.empty());
        CHECK(port_action_counts.empty());
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.layout_changes == 0);
        CHECK(fixture.errors.empty());
    };

    SECTION("a connection click moved five logical pixels opens its own menu") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        (void)fixture.selection.select_many(fixture.controller.project(), {"file", "gain"});
        fixture.inspector.show_node("file");
        std::vector<std::string> connection_labels;
        std::vector<std::vector<std::string>> port_labels;
        std::vector<std::size_t> port_action_counts;
        install_menu_observers(fixture, connection_labels, port_labels, port_action_counts);
        const auto before = fixture.controller.project();
        const auto selection_before = fixture.selection.selected_node_ids();
        const auto callbacks_before = fixture.selection_callback_count;
        right_gesture(fixture, {278.0f, 164.0f}, {283.0f, 164.0f});
        CHECK(connection_labels == std::vector<std::string>{"Disconnect connection"});
        CHECK(port_labels.empty());
        CHECK(port_action_counts.empty());
        CHECK(fixture.selection.selected_node_ids() == selection_before);
        CHECK(fixture.selection_callback_count == callbacks_before);
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.controller.revision() == before.revision);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK_FALSE(fixture.controller.results_stale());
        CHECK(fixture.layout_changes == 0);
        CHECK(fixture.errors.empty());
    }

    SECTION("a port click moved exactly six logical pixels opens only that port menu") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        (void)fixture.selection.select_many(fixture.controller.project(), {"file", "gain"});
        fixture.inspector.show_node("file");
        std::vector<std::string> connection_labels;
        std::vector<std::vector<std::string>> port_labels;
        std::vector<std::size_t> port_action_counts;
        install_menu_observers(fixture, connection_labels, port_labels, port_action_counts);
        const auto before = fixture.controller.project();
        const auto selection_before = fixture.selection.selected_node_ids();
        const auto callbacks_before = fixture.selection_callback_count;
        const auto output = port_anchor(fixture, "gain", "audio_out", audioalgo::PortDirection::output);
        right_gesture(fixture, output, output + juce::Point<float>{6.0f, 0.0f});
        CHECK(connection_labels.empty());
        REQUIRE(port_labels.size() == 1);
        CHECK(port_labels.front() == std::vector<std::string>{"gain:audio_out -> output:audio"});
        CHECK(port_action_counts == std::vector<std::size_t>{1});
        CHECK(fixture.selection.selected_node_ids() == selection_before);
        CHECK(fixture.selection_callback_count == callbacks_before);
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.controller.revision() == before.revision);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK_FALSE(fixture.controller.results_stale());
        CHECK(fixture.layout_changes == 0);
        CHECK(fixture.errors.empty());
    }

    SECTION("drags over blank start the existing marquee and never open a menu") {
        verify_marquee_only_drag({20.0f, 90.0f}, {540.0f, 220.0f}, {"file", "gain"});
    }

    SECTION("drags over a node title start the existing marquee and never open a menu") {
        verify_marquee_only_drag({80.0f, 115.0f}, {540.0f, 220.0f}, {"file", "gain"});
    }

    SECTION("drags over an output socket start the existing marquee and never open a menu") {
        verify_marquee_only_drag({216.0f, 164.0f}, {540.0f, 220.0f}, {"gain"});
    }

    SECTION("drags over an input socket start the existing marquee and never open a menu") {
        verify_marquee_only_drag({340.0f, 164.0f}, {20.0f, 220.0f}, {"file"});
    }

    SECTION("drags over a connection start the existing marquee and never open a menu") {
        verify_marquee_only_drag({278.0f, 164.0f}, {20.0f, 90.0f}, {"file"});
    }
}

TEST_CASE("NCE-09.b Right-clicking nodes preserves selected groups and targets an outside node",
          "[NCE-09.b][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    const auto right = juce::ModifierKeys::rightButtonModifier;

    SECTION("right-clicking a node in the selected set keeps the full selection") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        REQUIRE(fixture.selection.select_many(fixture.controller.project(), {"file", "gain"}));
        fixture.inspector.show_node("file");
        std::vector<std::vector<std::string>> menu_targets;
        std::vector<std::function<void()>> menu_actions;
        fixture.canvas.set_node_connection_context_menu_handler(
            [&](std::vector<std::string> node_ids, std::function<void()> action) {
                menu_targets.push_back(std::move(node_ids));
                menu_actions.push_back(std::move(action));
            });
        const auto before = fixture.controller.project();

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, right, {80.0f, 115.0f}));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, 0, {80.0f, 115.0f}));

        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
        CHECK(fixture.selection_callback_count == 0);
        CHECK(menu_targets == std::vector<std::vector<std::string>>{{"file", "gain"}});
        REQUIRE(menu_actions.size() == 1);
        CHECK(static_cast<bool>(menu_actions.front()));
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.controller.revision() == before.revision);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK_FALSE(fixture.controller.results_stale());
        CHECK(fixture.layout_changes == 0);
        CHECK(fixture.errors.empty());

        menu_actions.front()();
        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
        CHECK(fixture.controller.project().connections.empty());
        CHECK(fixture.controller.project().revision == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.project().nodes.size() == before.nodes.size());
        CHECK(fixture.layout_changes == 1);
        CHECK(fixture.errors.empty());
    }

    SECTION("right-clicking outside the selected set explicitly targets that node") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        REQUIRE(fixture.selection.select_many(fixture.controller.project(), {"file", "gain"}));
        fixture.inspector.show_node("file");
        std::vector<std::vector<std::string>> menu_targets;
        std::function<void()> menu_action;
        fixture.canvas.set_node_connection_context_menu_handler(
            [&](std::vector<std::string> node_ids, std::function<void()> action) {
                menu_targets.push_back(std::move(node_ids));
                menu_action = std::move(action);
            });
        const auto before = fixture.controller.project();

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {680.0f, 115.0f}, right, {680.0f, 115.0f}));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {680.0f, 115.0f}, 0, {680.0f, 115.0f}));

        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"output"});
        CHECK(fixture.selection_callback_count == 1);
        CHECK(fixture.selection_callback_ids == std::vector<std::string>{"output"});
        CHECK(menu_targets == std::vector<std::vector<std::string>>{{"output"}});
        REQUIRE(menu_action);
        menu_action();
        REQUIRE(fixture.controller.project().connections.size() == 1);
        CHECK(fixture.controller.project().connections.front().from_node == "file");
        CHECK(fixture.controller.project().connections.front().to_node == "gain");
        CHECK(fixture.controller.project().revision == before.revision + 1);
        CHECK(fixture.controller.history().undo_size() == 1);
        CHECK(fixture.controller.project().nodes.size() == before.nodes.size());
        auto expected = before;
        std::erase_if(expected.connections, [](const auto& edge) { return edge.to_node == "output"; });
        expected.revision = before.revision + 1;
        CHECK(same_project_snapshot(fixture.controller.project(), expected));
        CHECK(fixture.controller.revision() == before.revision + 1);
        CHECK(fixture.controller.dirty());
        CHECK(fixture.controller.results_stale());
        CHECK(fixture.layout_changes == 1);
        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"output"});
        CHECK(fixture.errors.empty());
    }

    SECTION("right-clicking blank space preserves selection and does not edit") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        REQUIRE(fixture.selection.select_many(fixture.controller.project(), {"file", "gain"}));
        std::vector<std::vector<std::string>> menu_targets;
        fixture.canvas.set_node_connection_context_menu_handler(
            [&](std::vector<std::string> node_ids, std::function<void()>) {
                menu_targets.push_back(std::move(node_ids));
            });
        const auto before = fixture.controller.project();

        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {900.0f, 500.0f}, right, {900.0f, 500.0f}));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {900.0f, 500.0f}, 0, {900.0f, 500.0f}));

        CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
        CHECK(fixture.selection_callback_count == 0);
        CHECK(menu_targets.empty());
        CHECK(same_project_snapshot(fixture.controller.project(), before));
        CHECK(fixture.controller.revision() == before.revision);
        CHECK_FALSE(fixture.controller.dirty());
        CHECK_FALSE(fixture.controller.results_stale());
        CHECK(fixture.layout_changes == 0);
        CHECK(fixture.errors.empty());
    }

    SECTION("a delayed node menu action cannot edit a replacement project") {
        CanvasInteractionFixture fixture;
        fixture.canvas.setVisible(true);
        REQUIRE(fixture.selection.select_many(fixture.controller.project(), {"file", "gain"}));
        std::function<void()> delayed_action;
        fixture.canvas.set_node_connection_context_menu_handler(
            [&](std::vector<std::string> node_ids, std::function<void()> action) {
                CHECK(node_ids == std::vector<std::string>{"file", "gain"});
                delayed_action = std::move(action);
            });
        fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, right, {80.0f, 115.0f}));
        fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, 0, {80.0f, 115.0f}));
        REQUIRE(delayed_action);

        auto replacement = fixture.controller.project();
        replacement.project_id = "replacement-during-node-menu";
        replacement.name = "Replacement project";
        fixture.controller.adopt_project(replacement);
        fixture.canvas.set_project_epoch(2);
        const auto replacement_before_action = fixture.controller.project();
        const auto undo_before_action = fixture.controller.history().undo_size();
        const auto redo_before_action = fixture.controller.history().redo_size();
        delayed_action();

        CHECK(fixture.controller.project().project_id == "replacement-during-node-menu");
        CHECK(same_project_snapshot(fixture.controller.project(), replacement_before_action));
        CHECK(fixture.controller.revision() == replacement_before_action.revision);
        CHECK(fixture.controller.history().undo_size() == undo_before_action);
        CHECK(fixture.controller.history().redo_size() == redo_before_action);
        CHECK(fixture.errors.size() == 1);
        CHECK(fixture.layout_changes == 0);
    }
}

TEST_CASE("NCE-09.c Left selection and Delete remain isolated after a node context click",
          "[NCE-09.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    const auto initial_project = fixture.controller.project();
    const auto right = juce::ModifierKeys::rightButtonModifier;
    const auto left = juce::ModifierKeys::leftButtonModifier;

    REQUIRE(fixture.selection.select_many(fixture.controller.project(), {"file", "gain"}));
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, right, {80.0f, 115.0f}));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {80.0f, 115.0f}, 0, {80.0f, 115.0f}));
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
    CHECK(fixture.controller.revision() == 0);

    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {680.0f, 115.0f}, left, {680.0f, 115.0f}));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {680.0f, 115.0f}, 0, {680.0f, 115.0f}));
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"output"});
    CHECK(fixture.selection_callback_ids.back() == "output");
    CHECK(fixture.controller.revision() == 0);
    CHECK_FALSE(fixture.controller.dirty());

    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, {20.0f, 90.0f}, left, {20.0f, 90.0f}));
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, {540.0f, 220.0f}, left, {20.0f, 90.0f}, true));
    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, {540.0f, 220.0f}, 0, {20.0f, 90.0f}, true));
    CHECK(fixture.selection.selected_node_ids() == std::vector<std::string>{"file", "gain"});
    CHECK(fixture.selection.selected_node_id() == "file");
    CHECK(fixture.controller.revision() == 0);

    REQUIRE(fixture.canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(fixture.controller.project().nodes.size() == 1);
    CHECK(fixture.controller.project().nodes.front().id == "output");
    CHECK(fixture.controller.project().connections.empty());
    CHECK(fixture.controller.project().selected_output_node_id == "output");
    CHECK(fixture.selection.selected_node_ids().empty());
    CHECK(fixture.controller.revision() == 1);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.layout_changes == 1);
    CHECK(fixture.errors.empty());

    REQUIRE(fixture.controller.undo());
    auto expected_after_undo = initial_project;
    expected_after_undo.revision = initial_project.revision + 2;
    CHECK(same_project_snapshot(fixture.controller.project(), expected_after_undo));
}

TEST_CASE("NCE-09.c Canvas drag swaps adjacent processing nodes atomically",
          "[NCE-09.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    auto project = fixture.controller.project();
    project.revision = 90;
    project.nodes[1].x = 210.0;
    project.nodes[1].y = 90.0;
    project.nodes.push_back({"gain-b", "builtin.gain", {{"gain_db", -6.0}}, 430.0, 180.0});
    project.connections = {{"file", "audio", "gain", "audio_in"},
                           {"gain", "audio_out", "gain-b", "audio_in"},
                           {"gain-b", "audio_out", "output", "audio"}};
    fixture.controller.adopt_project(project);
    fixture.canvas.set_project_epoch(2);
    const auto before = fixture.controller.project();

    const juce::Point<float> start{220.0f, 110.0f};
    const juce::Point<float> target{440.0f, 200.0f};
    fixture.canvas.mouseDown(canvas_mouse(fixture.canvas, start,
        juce::ModifierKeys::leftButtonModifier, start));
    fixture.canvas.mouseDrag(canvas_mouse(fixture.canvas, target,
        juce::ModifierKeys::leftButtonModifier, start, true));
    CHECK(same_project_snapshot(fixture.controller.project(), before));
    CHECK(fixture.controller.revision() == 90);
    CHECK(fixture.controller.history().undo_size() == 0);
    CHECK(fixture.layout_changes == 0);

    fixture.canvas.mouseUp(canvas_mouse(fixture.canvas, target, 0, start, true));
    CHECK(fixture.controller.revision() == 91);
    CHECK(fixture.controller.history().undo_size() == 1);
    CHECK(fixture.selection.selected_node_id() == "gain");
    CHECK(fixture.layout_changes == 1);
    CHECK(fixture.errors.empty());

    const auto find_node = [&](const std::string& node_id) -> const audioalgo::NodeInstance& {
        const auto found = std::find_if(fixture.controller.project().nodes.begin(),
            fixture.controller.project().nodes.end(), [&](const auto& node) { return node.id == node_id; });
        REQUIRE(found != fixture.controller.project().nodes.end());
        return *found;
    };
    CHECK(find_node("gain").x == 430.0);
    CHECK(find_node("gain").y == 180.0);
    CHECK(find_node("gain").parameters.at("gain_db") == audioalgo::ParameterValue{0.0});
    CHECK(find_node("gain-b").x == 210.0);
    CHECK(find_node("gain-b").y == 90.0);
    CHECK(find_node("gain-b").parameters.at("gain_db") == audioalgo::ParameterValue{-6.0});
    auto expected_after_swap = before;
    expected_after_swap.revision = 91;
    for (auto& node : expected_after_swap.nodes) {
        if (node.id == "gain") {
            node.x = 430.0;
            node.y = 180.0;
        } else if (node.id == "gain-b") {
            node.x = 210.0;
            node.y = 90.0;
        }
    }
    expected_after_swap.connections = {{"file", "audio", "gain-b", "audio_in"},
                                       {"gain-b", "audio_out", "gain", "audio_in"},
                                       {"gain", "audio_out", "output", "audio"}};
    CHECK(same_project_snapshot(fixture.controller.project(), expected_after_swap));

    REQUIRE(fixture.controller.undo());
    auto expected_after_undo = before;
    expected_after_undo.revision = 92;
    CHECK(same_project_snapshot(fixture.controller.project(), expected_after_undo));
}

TEST_CASE("NCE-09.c Middle pan and wheel zoom keep their exact transform semantics",
          "[NCE-09.c][CNL0001][component]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    CanvasInteractionFixture fixture;
    const auto before = fixture.controller.project();
    const juce::Point<float> anchor{320.0f, 200.0f};
    const auto model_anchor_before = fixture.canvas.coordinate_transform().canvas_to_model({anchor.x, anchor.y});
    const auto scale_before = fixture.canvas.coordinate_transform().zoom_scale();
    juce::MouseWheelDetails wheel{};
    wheel.deltaY = 1.0f;
    fixture.canvas.mouseWheelMove(canvas_mouse(fixture.canvas, anchor, 0, anchor), wheel);
    CHECK(fixture.canvas.coordinate_transform().zoom_scale() == Catch::Approx(scale_before * 1.25f));
    const auto model_anchor_after = fixture.canvas.coordinate_transform().canvas_to_model({anchor.x, anchor.y});
    CHECK(model_anchor_after.x == Catch::Approx(model_anchor_before.x));
    CHECK(model_anchor_after.y == Catch::Approx(model_anchor_before.y));

    const auto pan_before = fixture.canvas.coordinate_transform().pan_offset();
    const juce::Point<float> pan_start{15.0f, 20.0f};
    const juce::Point<float> pan_finish{55.0f, 47.0f};
    fixture.drag(pan_start, pan_finish, juce::ModifierKeys::middleButtonModifier);
    const auto pan_after = fixture.canvas.coordinate_transform().pan_offset();
    CHECK(pan_after.x == Catch::Approx(pan_before.x + pan_finish.x - pan_start.x));
    CHECK(pan_after.y == Catch::Approx(pan_before.y + pan_finish.y - pan_start.y));
    CHECK(fixture.canvas.coordinate_transform().zoom_scale() == Catch::Approx(scale_before * 1.25f));
    CHECK(same_project_snapshot(fixture.controller.project(), before));
    CHECK(fixture.controller.revision() == before.revision);
    CHECK_FALSE(fixture.controller.dirty());
    CHECK(fixture.layout_changes == 0);
    CHECK(fixture.errors.empty());
}

TEST_CASE("NCE-10.a Production document service saves and reopens disconnected and rewired graphs exactly",
          "[NCE-10.a][CNL0001][integration]") {
    juce::ScopedJuceInitialiser_GUI juce_initialiser;
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    constexpr auto package_content_hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr auto package_path = "node_packages/user.nce10a.fixture/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    auto managed_gain_descriptor = *registry.find("builtin.gain");
    managed_gain_descriptor.type_id = "user.nce10a.gain";
    managed_gain_descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
        "user.nce10a.fixture", "1.0.0", {}, {}, "python", package_content_hash,
        "python-function-v1", package_path};
    REQUIRE(registry.register_descriptor(std::move(managed_gain_descriptor)));
    audioalgo::editor::EditorController controller(registry);
    audioalgo::editor::ProjectFileDocumentService documents;

    audioalgo::ProjectModel original;
    original.schema_version = 2;
    original.project_id = "nce10a-document-project";
    original.name = "NCE-10.a round trip 中文";
    original.revision = 40;
    original.selected_output_node_id = "gain";
    original.selected_output_port_id = "audio_out";
    original.serialized_extensions = {{"vendor_project", "{\"stable\":true}"}};
    original.nodes = {
        {"source-a", "builtin.file_source", {{"path", std::string("resources/音频 input/source & A.wav")}}, -120.25, 48.5},
        {"source-b", "builtin.file_source", {{"path", std::string("resources/backup source B.wav")}}, 62.0, 340.0},
        {"gain", "user.nce10a.gain", {{"gain_db", -3.5}}, 240.0, 118.25},
        {"output", "builtin.output", {}, 520.0, 118.25},
    };
    original.nodes[0].serialized_extensions = {{"vendor_resource", "\"relative-path-retained\""}};
    original.nodes[2].serialized_extensions = {{"vendor_node", "{\"retain\":3}"}};
    original.nodes[2].managed_version = audioalgo::NodeInstance::ManagedVersion{
        "user.nce10a.fixture", package_content_hash, package_path, "python-function-v1",
        {{"vendor_lock", "\"exact-lock-extension\""}}};
    original.connections = {
        {"source-a", "audio", "gain", "audio_in", {{"vendor_edge", "\"source-a-route\""}}},
        {"gain", "audio_out", "output", "audio", {{"vendor_edge", "\"output-route\""}}},
    };

    const auto artifact_environment = std::getenv("AUDIOALGO_NCE10A_ARTIFACT_DIR");
    const bool keep_artifacts = artifact_environment != nullptr && *artifact_environment != '\0';
    const auto root = keep_artifacts
        ? std::filesystem::u8path(artifact_environment)
        : std::filesystem::temp_directory_path() / "AudioAlgo NCE10a" /
              ("case-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto run_directory = root /
        ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(run_directory));
    const auto original_path = run_directory / "original.aaproj";
    const auto disconnected_path = run_directory / "disconnected.aaproj";
    const auto rewired_path = run_directory / "rewired.aaproj";
    const auto original_before_path = run_directory / "original-before.aaproj.bytes";

    controller.adopt_project(original);
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::save(controller, documents, original_path)));
    const auto original_bytes = read_project_bytes(original_path);
    REQUIRE_FALSE(original_bytes.empty());
    std::ofstream(original_before_path, std::ios::binary).write(original_bytes.data(),
        static_cast<std::streamsize>(original_bytes.size()));
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::open(controller, documents, original_path)));
    CHECK(same_persisted_project(controller.project(), original));
    CHECK_FALSE(controller.dirty());

    auto expected_disconnected = original;
    expected_disconnected.connections.pop_back();
    expected_disconnected.revision = 41;
    REQUIRE(controller.disconnect(original.connections.back()).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    CHECK(same_persisted_project(controller.project(), expected_disconnected));
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::save(controller, documents, disconnected_path)));
    CHECK_FALSE(controller.dirty());
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::open(controller, documents, disconnected_path)));
    CHECK(same_persisted_project(controller.project(), expected_disconnected));
    CHECK(controller.project().nodes[0].parameters.at("path") ==
          audioalgo::ParameterValue{std::string("resources/音频 input/source & A.wav")});
    REQUIRE(controller.project().nodes[2].managed_version.has_value());
    CHECK(controller.project().nodes[2].managed_version == original.nodes[2].managed_version);

    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::open(controller, documents, original_path)));
    const audioalgo::Connection old_source{"source-a", "audio", "gain", "audio_in",
                                           {{"vendor_edge", "\"source-a-route\""}}};
    const audioalgo::Connection new_source{"source-b", "audio", "gain", "audio_in",
                                           {{"vendor_edge", "\"source-b-route\""}}};
    auto expected_rewired = original;
    expected_rewired.connections.front().from_node = new_source.from_node;
    expected_rewired.revision = 41;
    const auto reconnect = controller.reconnect_connection(old_source, new_source);
    INFO("reconnect status=" << static_cast<int>(reconnect.status)
         << (reconnect.diagnostics.empty() ? "" : " diagnostic=" + reconnect.diagnostics.front().code + ":" + reconnect.diagnostics.front().message));
    REQUIRE(reconnect.status == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(same_persisted_project(controller.project(), expected_rewired));
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::save(controller, documents, rewired_path)));
    REQUIRE(std::holds_alternative<std::monostate>(
        audioalgo::EditorProjectDocuments::open(controller, documents, rewired_path)));
    CHECK(same_persisted_project(controller.project(), expected_rewired));
    CHECK(controller.project().nodes[1].parameters.at("path") ==
          audioalgo::ParameterValue{std::string("resources/backup source B.wav")});
    CHECK(controller.project().nodes[2].managed_version == original.nodes[2].managed_version);
    CHECK(read_project_bytes(original_path) == original_bytes);
    CHECK(read_project_bytes(original_path) == read_project_bytes(original_before_path));
    CHECK(controller.project().selected_output_node_id == "gain");
    CHECK(controller.project().selected_output_port_id == "audio_out");

    std::ofstream(run_directory / "run-path.txt", std::ios::binary) << run_directory.generic_string();
    std::ofstream(root / "latest-run.txt", std::ios::binary) << run_directory.generic_string();
    if (!keep_artifacts)
        std::filesystem::remove_all(root);
}
