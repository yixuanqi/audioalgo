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

#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

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
    std::vector<std::string> errors;
    audioalgo::editor::GraphCanvas canvas{controller.project(), registry, selection, controller,
        [this](std::string id) { inspector.show_node(id); }, [this] { ++layout_changes; },
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
