"""AR-08.c guard for generic numeric slider transaction routing."""
from __future__ import annotations

import unittest
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "apps" / "editor" / "Main.cpp"
PANEL = ROOT / "apps" / "editor" / "EditorParameterPanel.cpp"


class ParameterSliderGestureRouteTests(unittest.TestCase):
    def test_any_numeric_slider_drag_stages_updates_and_commits_one_controller_gesture(self):
        source = editor_route_source(ROOT)
        panel = PANEL.read_text(encoding="utf-8")
        inspector = (ROOT / "apps/editor/EditorNodeInspector.h").read_text(encoding="utf-8")
        self.assertIn('#include "EditorNodeInspector.h"', source)
        required_route = (
            "editor->onDragStart = [this, id, kind, control = editor.get()]",
            "editor->onValueChange = [this, id, kind, control = editor.get()]",
            "editor->onDragEnd = [this, id, kind, control = editor.get()]",
        )
        missing = [route for route in required_route if route not in panel]
        required_controller_route = (
            "controller_.begin_parameter_gesture(binding.session(), binding.node_id(), parameter_id)",
            "controller_.update_parameter_gesture(binding.session(), std::move(value))",
            "controller_.commit_parameter_gesture(binding.session())",
            "if (!binding.matches(snapshot_))",
            "std::optional<std::string> active_gesture_parameter_;",
        )
        missing.extend(route for route in required_controller_route if route not in inspector)
        self.assertEqual(missing, [], "Gain drag is not one session-scoped controller gesture: " + repr(missing))


if __name__ == "__main__":
    unittest.main(verbosity=2)
