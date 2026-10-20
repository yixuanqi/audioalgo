"""AR-13.d production-boundary checks for project resource interpretation."""
from __future__ import annotations

import unittest
import re
from pathlib import Path
from editor_source import editor_route_source

ROOT = Path(__file__).resolve().parents[2]


class ProjectResourceServiceRouteTests(unittest.TestCase):
    def test_resource_service_is_value_only_and_run_service_uses_it(self):
        header = (ROOT / "libs/audio_project/ProjectResourceService.h").read_text(encoding="utf-8")
        implementation = (ROOT / "libs/audio_project/ProjectResourceService.cpp").read_text(encoding="utf-8")
        run_header = (ROOT / "libs/audio_run_service/RunService.h").read_text(encoding="utf-8")
        run_source = (ROOT / "libs/audio_run_service/RunService.cpp").read_text(encoding="utf-8")

        self.assertIn("ProjectResourceService", header)
        self.assertIn("make_reference", header)
        self.assertIn("relocate", header)
        self.assertNotRegex(header, re.compile(r'#\s*include\s*[<"]juce', re.IGNORECASE))
        self.assertNotRegex(header, r"\bjuce::")
        self.assertNotRegex(implementation, r"\bjuce::")
        self.assertIn("std::shared_ptr<const ProjectResourceService> resources", run_header)
        self.assertIn("resources_->resolve(utf8_path, request.project_file)", run_source)
        self.assertNotIn("std::filesystem::u8path(utf8_path)", run_source)

    def test_editor_gives_chooser_results_to_resource_service(self):
        source = editor_route_source(ROOT)
        self.assertIn("resources->make_reference(selected_file, project_path)", source)
        self.assertNotIn("std::filesystem::relative(selected_file", source)
        self.assertIn("resources->relocate(missing_reference, expected_project_path, selected_file)", source)
        self.assertIn('expected_session, node_id, "path", std::get<std::string>(replacement)', source)
        self.assertIn("std::shared_ptr<const audioalgo::ProjectResourceService> project_resources_", source)
        self.assertIn("std::make_shared<audioalgo::ProjectResourceService>()", source)
        self.assertIn('error->code == "source_resource_missing"', source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
