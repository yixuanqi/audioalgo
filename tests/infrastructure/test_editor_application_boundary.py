"""AR-09.a boundary guard: the application layer stays headless and view-free.

The controller must be usable without JUCE, without the view and without file dialogs, so the
same commands can be exercised by headless tests and (later) by the GUI.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APPLICATION = ROOT / "libs" / "audio_editor_application"
FORBIDDEN_INCLUDES = ("juce_", "apps/", "audio_juce_backend", "audio_io", "audio_project_file_io",
                      "audio_run_service")


def application_sources() -> list[Path]:
    return sorted(path for path in APPLICATION.rglob("*") if path.suffix in {".h", ".cpp"})


class EditorApplicationBoundaryTests(unittest.TestCase):
    def test_application_layer_has_no_juce_view_or_io_dependency(self):
        sources = application_sources()
        self.assertTrue(sources, "the application layer has no sources")
        offenders = []
        for path in sources:
            text = path.read_text(encoding="utf-8")
            for include in re.findall(r'#include\s+"([^"]+)"', text):
                if any(token in include for token in FORBIDDEN_INCLUDES):
                    offenders.append(f"{path.relative_to(ROOT).as_posix()} -> {include}")
        self.assertEqual(offenders, [], "application layer must stay headless: " + "; ".join(offenders))

    def test_controller_owns_the_parameter_command_and_history(self):
        header = (APPLICATION / "EditorController.h").read_text(encoding="utf-8")
        self.assertIn("set_parameter", header)
        self.assertIn("ParameterEditOutcome", header)
        self.assertIn("EditorTopologyHistory", header)
        implementation = (APPLICATION / "EditorController.cpp").read_text(encoding="utf-8")
        # The command must validate through the shared SDK schema, not a private copy of the rules.
        self.assertIn("validate_parameter_value", implementation)
        self.assertIn("history_.apply", implementation)
        self.assertNotIn("revision++", implementation)
        self.assertNotIn("++project_.revision", implementation)

    def test_cmake_builds_the_application_target_without_juce(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        block = re.search(r"add_library\(audio_editor_application STATIC[^\n]*\n(?P<body>(?:.|\n)*?)target_compile_features",
                          cmake)
        self.assertIsNotNone(block, "audio_editor_application target is missing")
        body = block.group("body")
        self.assertIn("audio_project", body)
        self.assertNotIn("juce::", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
