"""F5 regression: the main Gate must never report ``passed`` while a required test skipped.

These are controlled CTest fixtures, not platform acceptance. Each fixture registers exactly
the G0 required IDs and then varies the execution status of one required test.
"""
from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE_SPEC = importlib.util.spec_from_file_location("legacy_gate_verifier", ROOT / "scripts" / "verify_gate.py")
GATE = importlib.util.module_from_spec(GATE_SPEC)
GATE_SPEC.loader.exec_module(GATE)

FIXTURE = """cmake_minimum_required(VERSION 3.24)
project(AudioAlgoGateStatusProbeFixture NONE)
enable_testing()
set(MODE "pass" CACHE STRING "probe status mode")
foreach(test_id IN ITEMS BLD-01 BLD-02 BLD-03 BLD-04)
  add_test(NAME "${test_id}-fixture" COMMAND "${CMAKE_COMMAND}" -E true)
endforeach()
if(MODE STREQUAL "skip")
  add_test(NAME "LOOP-01-skipped" COMMAND "${CMAKE_COMMAND}" -E echo "CONTROLLED_SKIP")
  set_tests_properties("LOOP-01-skipped" PROPERTIES SKIP_REGULAR_EXPRESSION "CONTROLLED_SKIP")
elseif(MODE STREQUAL "disabled")
  add_test(NAME "LOOP-01-disabled" COMMAND "${CMAKE_COMMAND}" -E true)
  set_tests_properties("LOOP-01-disabled" PROPERTIES DISABLED TRUE)
elseif(MODE STREQUAL "fail")
  add_test(NAME "LOOP-01-failing" COMMAND "${CMAKE_COMMAND}" -E false)
else()
  add_test(NAME "LOOP-01-fixture" COMMAND "${CMAKE_COMMAND}" -E true)
endif()
"""


def configure(root: Path, mode: str) -> Path:
    source = root / "source"
    source.mkdir(parents=True, exist_ok=True)
    (source / "CMakeLists.txt").write_text(FIXTURE, encoding="utf-8")
    build = root / f"build-{mode}"
    result = subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                             "-G", "Visual Studio 18 2026", "-A", "x64", f"-DMODE={mode}"],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    if result.returncode != 0:
        raise AssertionError(f"probe fixture configure failed: {result.stdout}\n{result.stderr}")
    return build


class GateStatusProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="audioalgo-gate-probe-"))
        cls.builds = {mode: configure(cls.root, mode) for mode in ("pass", "skip", "disabled", "fail")}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def gate(self, mode: str, probe: str) -> tuple[int, dict]:
        exit_code = GATE.run_gate("G0", self.builds[mode], "Debug", probe=probe)
        report_path = GATE.last_report_path()
        report = json.loads(Path(report_path).read_text(encoding="utf-8"))
        self.assertEqual(report.get("probe"), probe)
        return exit_code, report

    def test_all_required_tests_passing_still_passes(self):
        exit_code, report = self.gate("pass", "controlled_pass")
        self.assertEqual(report["status"], "passed")
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["test_counts"]["skipped"], 0)

    def test_required_skipped_test_blocks_the_gate(self):
        exit_code, report = self.gate("skip", "controlled_skip")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["test_counts"]["skipped"], 1)
        self.assertEqual(report["test_counts"]["passed"], 4,
                         "a skipped required test must not be counted as passed")
        self.assertTrue(any("LOOP-01" in item for item in report["status_failures"]))

    def test_required_disabled_test_blocks_the_gate(self):
        exit_code, report = self.gate("disabled", "controlled_disabled")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(exit_code, 1)
        self.assertTrue(report["test_counts"]["not_run"] >= 1)
        self.assertTrue(any("LOOP-01" in item for item in report["status_failures"]))

    def test_required_failing_test_blocks_the_gate(self):
        exit_code, report = self.gate("fail", "controlled_failure")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(exit_code, 1)
        self.assertTrue(report["test_counts"]["failed"] >= 1)

    def test_desktop_evidence_is_required_for_gui_acceptance(self):
        self.assertTrue({"UI-01", "UI-02", "UI-04", "UI-07"}.issubset(GATE.DESKTOP_EVIDENCE_IDS))
        for gate in ("G1", "G5"):
            required = GATE.required_test_ids(gate)
            self.assertTrue(GATE.desktop_evidence_ids(gate).issubset(required),
                            f"{gate} must keep its desktop evidence IDs inside the required set")


if __name__ == "__main__":
    unittest.main(verbosity=2)
