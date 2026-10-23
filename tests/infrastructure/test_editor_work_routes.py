"""AR-09.e guards that editor file work is scheduled off the message thread."""
from __future__ import annotations

import unittest
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]
MAIN = editor_route_source(ROOT)
WORK_JOB = (ROOT / "apps" / "editor" / "EditorWorkJob.h").read_text(encoding="utf-8") if (
    ROOT / "apps" / "editor" / "EditorWorkJob.h").exists() else ""
RUN_QUEUE = (ROOT / "apps/editor/EditorRunQueue.h").read_text(encoding="utf-8")


def function_body(signature: str, source: str = MAIN) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


class EditorWorkRouteTests(unittest.TestCase):
    def test_document_resource_and_demo_file_work_is_submitted_to_worker_pool(self):
        for signature in (
            "bool save_to(",
            "void open_project()",
            "void load_staged_demo()",
            "void add_source_node(",
        ):
            with self.subTest(route=signature):
                self.assertIn("submit_work<", function_body(signature),
                              f"{signature} must schedule its file operation off the message thread")

    def test_run_route_does_not_probe_source_files_on_the_message_thread(self):
        run = function_body("void run_project()")
        self.assertNotIn("ensure_source_resources_available", run)
        self.assertNotIn("project_resources_->resolve", run)
        self.assertIn("EditorRunQueue<EditorComponent>", run)
        self.assertIn("run_queue_->submit", run)
        self.assertNotIn(".runJob()", run)
        self.assertIn("EditorRunJob<ComponentType> one", RUN_QUEUE)
        self.assertIn("one.runJob()", RUN_QUEUE)
        self.assertIn("pool_.add_job", RUN_QUEUE)

    def test_work_job_runs_io_before_dispatching_completion_to_the_live_component(self):
        self.assertTrue(WORK_JOB, "the editor worker job adapter is missing")
        self.assertIn("work_()", WORK_JOB)
        self.assertIn("dispatch_to_live_component", WORK_JOB)


if __name__ == "__main__":
    unittest.main(verbosity=2)
