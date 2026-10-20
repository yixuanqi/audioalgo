"""AR-13.e guard for the persisted model boundary and migration policy."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ProjectFormatPolicyTests(unittest.TestCase):
    def test_persisted_model_is_value_only_and_audio_project_build_is_pure(self):
        model = (ROOT / "libs/audio_project/ProjectModel.h").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotRegex(model, r"\bjuce::|AudioProcessor|Component\s*\*|AudioProcessor\s*\*")
        self.assertIn("add_library(audio_project STATIC libs/audio_project/ProjectModel.cpp libs/audio_project/ProjectResourceService.cpp)", cmake)
        project_target = cmake.split("add_library(audio_project STATIC", 1)[1].split("# Headless application layer", 1)[0]
        self.assertNotIn("juce::", project_target)
        self.assertIn("target_link_libraries(audio_project PUBLIC audio_node_sdk)", project_target)

    def test_version_and_backup_policy_match_current_schema_fixture_behavior(self):
        policy_path = ROOT / "doc" / "project_format_compatibility.md"
        self.assertTrue(policy_path.is_file(), "document the current persisted schema and backup policy")
        policy = policy_path.read_text(encoding="utf-8")
        required = (
            "`schema_version` is `1`",
            "project_schema_unsupported",
            "does not migrate or rewrite",
            "does not create `.bak`",
            "bytes remain unchanged",
            "atomically replaces",
            "schema-N.bak",
        )
        missing = [text for text in required if text not in policy]
        self.assertEqual(missing, [], "project format policy is incomplete: " + repr(missing))


if __name__ == "__main__":
    unittest.main(verbosity=2)
