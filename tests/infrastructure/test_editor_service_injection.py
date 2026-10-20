"""AR-09.a guards that production editor routes use injected, headless service ports."""
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


class EditorServiceInjectionTests(unittest.TestCase):
    def test_open_save_and_demo_load_use_the_injected_document_service(self):
        save = function_body("bool save_to(")
        open_project = function_body("void open_project()")
        demo = function_body("void load_staged_demo()")
        self.assertIn("service->save(path, snapshot)", save)
        self.assertIn("EditorProjectDocuments::complete_save", save)
        self.assertNotIn("save_project_file(", save)
        self.assertIn("service->load(path)", open_project)
        self.assertIn("EditorProjectDocuments::complete_open", open_project)
        self.assertIn("submit_work<audioalgo::ProjectDocumentLoadOutcome>", open_project)
        self.assertNotIn("load_project_file(", open_project)
        self.assertIn("service->load(path)", demo)
        self.assertIn("EditorProjectDocuments::complete_open", demo)
        self.assertNotIn("load_project_file(", demo)

    def test_preview_controls_use_the_injected_preview_port(self):
        toggle = function_body("void toggle_playback()")
        stop = function_body("void stop_playback_resources()")
        self.assertIn("preview_service_->play", toggle)
        self.assertIn("preview_service_->pause", toggle)
        self.assertIn("preview_service_->stop", stop)
        self.assertNotIn("transport_", toggle + stop)

    def test_runtime_composition_supplies_all_headless_ports(self):
        self.assertIn("std::shared_ptr<const audioalgo::ProjectDocumentService>", MAIN)
        self.assertIn("std::shared_ptr<audioalgo::AudioPreviewService>", MAIN)
        self.assertIn("std::make_shared<ProjectFileDocumentService>()", MAIN)
        self.assertIn("std::make_shared<JuceAudioPreviewService>()", MAIN)
        self.assertIn("std::make_shared<audioalgo::ProjectResourceService>()", MAIN)


if __name__ == "__main__":
    unittest.main(verbosity=2)
