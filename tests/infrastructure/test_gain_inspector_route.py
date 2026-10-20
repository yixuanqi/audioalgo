"""Guard the production descriptor-driven parameter panel's route to controller intent."""
from __future__ import annotations

import re
import unittest
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "apps" / "editor" / "Main.cpp"


def class_body(source: str, declaration: str, next_declaration: str) -> str:
    start = source.index(declaration)
    end = source.index(next_declaration, start)
    return source[start:end]


class ParameterInspectorRouteTests(unittest.TestCase):
    def test_production_parameter_panel_uses_an_instance_bound_controller_intent(self):
        source = editor_route_source(ROOT)
        inspector = (ROOT / "apps/editor/EditorNodeInspector.h").read_text(encoding="utf-8")
        self.assertIn('#include "EditorNodeInspector.h"', source)

        self.assertIn("std::optional<audioalgo::editor::NodeViewSnapshot> snapshot_;", inspector)
        self.assertNotRegex(inspector, r"(?:audioalgo::)?ProjectModel\s*[*&]")
        self.assertNotRegex(inspector, r"(?:audioalgo::)?NodeInstance\s*\*")
        self.assertIn("panel_.show_parameters(*descriptor, node.parameters", inspector)
        self.assertIn("controller_.node_snapshot(node_id)", inspector)
        self.assertIn("EditorNodeParameterBinding binding(controller_, *snapshot_);", inspector)
        self.assertIn("binding.set_parameter_if_current(snapshot_", inspector)
        self.assertIn("[this, binding]", inspector)
        self.assertIn("apply_parameter_gesture(binding,", inspector)
        self.assertIn("if (!binding.matches(snapshot_))", inspector)
        self.assertNotIn("controller_.set_parameter(snapshot_->session, node_id, parameter_id", inspector)
        self.assertNotIn('node.type_id == "builtin.gain"', inspector)

    def test_editor_owns_controller_and_canvas_receives_a_read_only_model_view(self):
        source = editor_route_source(ROOT)
        editor = class_body(source, "class EditorComponent final", "class MainWindow final")
        canvas = class_body(source, "class GraphCanvas final", "class EditorComponent final")

        self.assertIn("audioalgo::editor::EditorController controller_;", editor)
        self.assertIn("project_(controller_.project())", editor)
        self.assertIn("const audioalgo::ProjectModel& project_;", editor)
        self.assertIn("const audioalgo::ProjectModel& project", canvas)
        self.assertIn("audioalgo::editor::EditorController& controller", canvas)
        self.assertNotRegex(source, r"project_\.(?:nodes|connections)\.(?:push_back|insert|erase|clear)\s*\(")
        self.assertNotRegex(source, r"project_\.(?:name|revision)\s*=")

    def test_history_refresh_preserves_a_surviving_selected_parameter_node(self):
        source = editor_route_source(ROOT)
        for name, next_name in (("undo_topology", "redo_topology"), ("redo_topology", "refresh_history_selection")):
            body = class_body(source, "void " + name + "()", "void " + next_name + "()")
            self.assertIn("refresh_history_selection();", body)
        self.assertIn("canvas_.select(controller_.node_snapshot(node_id) ? node_id : std::string{});", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
