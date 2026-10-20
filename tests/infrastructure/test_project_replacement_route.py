"""Production route guards for replacing the editor's current project."""
from __future__ import annotations

import unittest
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]
MAIN = editor_route_source(ROOT)


def function_body(signature: str) -> str:
    start = MAIN.index(signature)
    opening = MAIN.index("{", start)
    depth = 0
    for index in range(opening, len(MAIN)):
        if MAIN[index] == "{":
            depth += 1
        elif MAIN[index] == "}":
            depth -= 1
            if depth == 0:
                return MAIN[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


class ProjectReplacementRouteTests(unittest.TestCase):
    def test_open_refreshes_history_controls_after_adopting_the_loaded_project(self):
        body = function_body("void open_project()")
        opening = body.index("EditorProjectDocuments::complete_open(")
        refresh = body.find("update_history_controls()", opening)
        self.assertGreaterEqual(
            refresh,
            0,
            "Open must refresh Undo/Redo from the newly adopted project's cleared history",
        )

    def test_new_and_open_revoke_old_project_actions_before_adoption(self):
        new = function_body("void new_project()")
        self.assertLess(new.index("advance_project_epoch()"), new.index("controller_.adopt_project("))
        open_project = function_body("void open_project()")
        self.assertIn("EditorProjectDocuments::complete_open(", open_project)
        self.assertIn("[&target] { target.advance_project_epoch(); }", open_project)
        self.assertIn("controller_.document_generation() != expected_document_generation", open_project)

    def test_epoch_change_clears_chooser_drag_and_deferred_run_state(self):
        body = function_body("void advance_project_epoch()")
        for operation in (
            "active_file_chooser_.reset()",
            "file_chooser_completion_.invalidate()",
            "run_after_save_ = false",
            "canvas_.set_project_epoch(project_epoch_)",
            "canvas_.select({})",
            "waveform_.set_waveform({})",
            "output_file_.clear()",
            "stop_playback_resources()",
            'play_button_.setButtonText("Play output")',
        ):
            with self.subTest(operation=operation):
                self.assertIn(operation, body)
        canvas = function_body("void set_project_epoch(std::uint64_t epoch)")
        self.assertIn("library_drop_session_.cancel()", canvas)


if __name__ == "__main__":
    unittest.main(verbosity=2)
