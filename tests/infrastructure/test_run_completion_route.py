"""Bind stale-run protection to the production View completion route (AR-09.c)."""
from __future__ import annotations

import unittest
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]
MAIN = editor_route_source(ROOT)
ROUTE = (ROOT / "apps" / "editor" / "EditorRunCompletion.h").read_text(encoding="utf-8")


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


class RunCompletionRouteTests(unittest.TestCase):
    def test_run_reception_rejects_stale_identity_before_view_state_is_touched(self):
        body = function_body("void run_finished(")
        gate = body.index("apply_if_current_run(")
        callback = body.index("[&] {", gate)
        for view_update in (
            "set_status(",
            "output_file_ =",
            "waveform_.set_waveform(",
            "play_button_.setButtonText(",
        ):
            with self.subTest(view_update=view_update):
                self.assertGreater(body.index(view_update), callback,
                                   f"{view_update} must be inside the production identity-gated completion")

    def test_shared_production_gate_checks_identity_before_invoking_view_completion(self):
        gate = ROUTE.index("if (!reception.accepts(")
        invoke = ROUTE.index("std::forward<Completion>(completion)()")
        self.assertLess(gate, invoke)
        self.assertIn("return false;", ROUTE[gate:invoke])

    def test_project_epoch_invalidates_runs_before_clearing_presentation_state(self):
        body = function_body("void advance_project_epoch()")
        self.assertLess(body.index("run_reception_.invalidate()"), body.index("output_file_.clear()"))
        self.assertLess(body.index("run_reception_.invalidate()"), body.index("waveform_.set_waveform({})"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
