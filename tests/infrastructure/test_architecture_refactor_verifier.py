from __future__ import annotations

import copy
import importlib.util
import json
import hashlib
import os
import sys
import xml.etree.ElementTree as ET
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("architecture_verifier", ROOT / "scripts" / "verify_architecture_refactor.py")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)

GATE_SPEC = importlib.util.spec_from_file_location("legacy_gate_verifier", ROOT / "scripts" / "verify_gate.py")
GATE = importlib.util.module_from_spec(GATE_SPEC)
GATE_SPEC.loader.exec_module(GATE)
RECORD_SPEC = importlib.util.spec_from_file_location("record_command", ROOT / "scripts" / "record_command.py")
RECORD = importlib.util.module_from_spec(RECORD_SPEC)
RECORD_SPEC.loader.exec_module(RECORD)


class ArchitectureVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = (ROOT / "doc" / "testing_architecture_refactor.md").read_text(encoding="utf-8")
        cls.manifest = json.loads((ROOT / "tests" / "architecture_refactor_manifest.json").read_text(encoding="utf-8"))

    def test_manifest_exact_110_case_coverage_and_hash(self):
        self.assertEqual(len(VERIFY.acceptance_ids(self.document)), 110)
        self.assertEqual(VERIFY.validate_manifest(self.manifest, self.document), [])

    def test_source_provenance_excludes_ephemeral_test_scratch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "src" / "input.cpp").write_text("source", encoding="utf-8")
            scratch = root / ".audioalgo-test-tmp" / "leftover-run"
            scratch.mkdir(parents=True)
            (scratch / "output.wav").write_bytes(b"generated audio")
            for module in (RECORD, VERIFY):
                with self.subTest(module=module.__name__), patch.object(module, "ROOT", root):
                    self.assertEqual(set(module.source_manifest()), {"src/input.cpp"})

    def test_source_scan_prunes_ignored_trees_and_preserves_digest_contracts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kept = {"src/input.cpp": "source", "doc/nested/readme.md": "document", ".pytest_cache/probe": "cache"}
            ignored = {"build", ".git", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
            for name, text in kept.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            for name in ignored - {".pytest_cache"}:
                path = root / "src" / name / "deep/ignored.cpp"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("excluded", encoding="utf-8")
            expected = {name: hashlib.sha256(text.encode()).hexdigest()
                        for name, text in kept.items() if not ignored.intersection(Path(name).parts)}
            real_scandir = os.scandir
            def guarded_scandir(path):
                relative = Path(path).relative_to(root)
                self.assertFalse(ignored.intersection(relative.parts), f"walked excluded tree: {relative}")
                return real_scandir(path)
            modules = [(RECORD, "manifest"), (VERIFY, "manifest"), (GATE, "json_hash")]
            for relative, mode in (("scripts/verify_editor_interaction.py", "editor_hash"),
                                   ("tests/infrastructure/test_architecture_baseline.py", "baseline"),
                                   ("tests/infrastructure/test_ar04_consumer.py", "consumer")):
                spec = importlib.util.spec_from_file_location("scan_" + mode, ROOT / relative)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                modules.append((module, mode))
            for module, mode in modules:
                exclusions = ignored - {".pytest_cache"} if mode == "editor_hash" else ignored
                def guarded(path):
                    relative = Path(path).relative_to(root)
                    self.assertFalse(exclusions.intersection(relative.parts), f"walked excluded tree: {relative}")
                    return real_scandir(path)
                with self.subTest(module=module.__name__), patch.object(module, "ROOT", root), patch("os.scandir", guarded), patch("glob._StringGlobber.scandir", staticmethod(guarded)):
                    if mode == "json_hash":
                        digest = hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
                        self.assertEqual(module.source_tree_hash(), digest)
                    elif mode == "editor_hash":
                        digest = hashlib.sha256()
                        for name in sorted(kept):
                            encoded = name.encode("utf-8")
                            digest.update(len(encoded).to_bytes(4, "big"))
                            digest.update(encoded)
                            digest.update(hashlib.sha256(kept[name].encode()).digest())
                        self.assertEqual(module.source_tree_hash(), digest.hexdigest())
                    elif mode == "baseline":
                        self.assertEqual(module.current_source_manifest(), expected)
                    elif mode == "consumer":
                        self.assertEqual(module.source_manifest(root), expected)
                    else:
                        self.assertEqual(module.source_manifest(), expected)

    def test_missing_duplicate_and_document_hash_fail_closed(self):
        for mutation in ("missing", "duplicate", "hash"):
            manifest = copy.deepcopy(self.manifest)
            if mutation == "missing":
                manifest["requirements"].pop()
            elif mutation == "duplicate":
                manifest["requirements"].append(copy.deepcopy(manifest["requirements"][0]))
            else:
                manifest["acceptance_document_sha256"] = "0" * 64
            with self.subTest(mutation=mutation):
                self.assertTrue(VERIFY.validate_manifest(manifest, self.document))

    def test_shared_facts_require_exact_owners_and_preserve_each_case_evidence(self):
        self.assertEqual(VERIFY.validate_manifest(self.manifest, self.document), [])
        self.assertTrue(self.manifest["shared_test_facts"])
        for mutation in ("undeclared", "owner", "reason", "stale", "duplicate", "within_row", "malformed"):
            broken = copy.deepcopy(self.manifest)
            if mutation == "undeclared":
                broken.pop("shared_test_facts")
            elif mutation == "owner":
                broken["shared_test_facts"][0]["requirement_ids"].pop()
            elif mutation == "reason":
                broken["shared_test_facts"][0]["fact"] = ""
            elif mutation == "stale":
                broken["shared_test_facts"][0]["test_name"] = "not-a-mapped-test"
            elif mutation == "duplicate":
                broken["shared_test_facts"].append(copy.deepcopy(broken["shared_test_facts"][0]))
            elif mutation == "within_row":
                broken["requirements"][0]["test_names"] *= 2
            else:
                broken["shared_test_facts"] = {}
            with self.subTest(mutation=mutation):
                self.assertTrue(VERIFY.validate_manifest(broken, self.document))
        rows = [{"id": case_id, "stage": "R0", "required": True, "runner": "ctest",
                 "test_names": ["shared-render"], "desktop_required": desktop}
                for case_id, desktop in (("AR-03.a", False), ("AR-03.b", True))]
        manifest = {"stages": [f"R{i}" for i in range(7)], "requirements": rows}
        result = VERIFY.evaluate_cases(manifest, {"shared-render"}, {"shared-render": "passed"}, "R0")
        self.assertEqual([row["status"] for row in result], ["passed", "not_verified"])
        for state in ("failed", "skipped", "timeout"):
            result = VERIFY.evaluate_cases(manifest, {"shared-render"}, {"shared-render": state}, "R0")
            self.assertEqual([row["status"] for row in result], [state, state])

    def test_zero_tests_missing_registration_and_unexecuted_are_not_passed(self):
        case = {"id": "AR-03.a", "stage": "R0", "required": True, "layer": "pure", "runner": "ctest",
                "test_names": ["AR-03.a"], "evidence_requirements": []}
        manifest = {"stages": [f"R{i}" for i in range(7)], "requirements": [case]}
        self.assertEqual(VERIFY.evaluate_cases(manifest, set(), {}, "R0")[0]["status"], "not_run")
        self.assertEqual(VERIFY.evaluate_cases(manifest, {"AR-03.a"}, {}, "R0")[0]["status"], "not_run")

    def test_wrong_configuration_binary_and_crashed_process_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_dir = root / "build" / "windows-debug"
            build_dir.mkdir(parents=True)
            binary_paths = [build_dir / name for name in (
                "audio_cli.exe", "audioalgo_tests.exe", "audio_application_tests.exe", "AudioAlgo Editor.exe")]
            for path in binary_paths:
                path.write_bytes(path.name.encode())
            junit = root / "tests.xml"
            junit.write_text("<testsuite><testcase name='AR-03.a'/></testsuite>", encoding="utf-8")
            ctest_log = root / "ctest.log"
            ctest_log.write_text("all passed", encoding="utf-8")
            build_log = root / "build.log"
            build_log.write_text("build passed", encoding="utf-8")
            source = {"src.cpp": "a" * 64}
            source_hash = hashlib.sha256(json.dumps(source, sort_keys=True).encode()).hexdigest()
            ctest_record = {
                "command": ["ctest", "--test-dir", str(build_dir), "-C", "Debug", "--output-junit", str(junit)],
                "cwd": str(ROOT), "started_utc": "now", "finished_utc": "now", "exit_code": 0,
                "log_path": str(ctest_log), "log_sha256": hashlib.sha256(ctest_log.read_bytes()).hexdigest(),
                "sources": source, "source_tree_hash": source_hash,
                "artifacts": [{"path": str(junit), "sha256": hashlib.sha256(junit.read_bytes()).hexdigest()}],
            }
            build_record = {
                "command": ["cmake", "--build", "--preset", "windows-debug"],
                "cwd": str(ROOT), "started_utc": "now", "finished_utc": "now", "exit_code": 0,
                "log_path": str(build_log), "log_sha256": hashlib.sha256(build_log.read_bytes()).hexdigest(),
                "sources": source, "source_tree_hash": source_hash,
                "artifacts": [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in binary_paths],
            }
            ctest_path, build_path = root / "ctest.json", root / "build.json"
            ctest_path.write_text(json.dumps(ctest_record), encoding="utf-8")
            build_path.write_text(json.dumps(build_record), encoding="utf-8")
            inventory = [{"name": "AR-03.a", "command": [str(binary_paths[1])]}]
            errors, _ = VERIFY.validate_run_records(ctest_path, build_path, build_dir, "Debug", junit, inventory, source)
            self.assertEqual(errors, [])

            wrong_configuration = copy.deepcopy(ctest_record)
            wrong_configuration["command"][wrong_configuration["command"].index("-C") + 1] = "Release"
            ctest_path.write_text(json.dumps(wrong_configuration), encoding="utf-8")
            errors, _ = VERIFY.validate_run_records(ctest_path, build_path, build_dir, "Debug", junit, inventory, source)
            self.assertTrue(any("wrong build directory or configuration" in error for error in errors))

            ctest_path.write_text(json.dumps(ctest_record), encoding="utf-8")
            wrong_binary = copy.deepcopy(build_record)
            wrong_binary["artifacts"] = [item for item in wrong_binary["artifacts"] if not item["path"].endswith("audioalgo_tests.exe")]
            build_path.write_text(json.dumps(wrong_binary), encoding="utf-8")
            errors, _ = VERIFY.validate_run_records(ctest_path, build_path, build_dir, "Debug", junit, inventory, source)
            self.assertTrue(any("audioalgo_tests.exe" in error for error in errors))

            crashed = root / "crash.xml"
            crashed.write_text("<testsuite><testcase name='AR-03.a'><error message='process crashed'/></testcase></testsuite>", encoding="utf-8")
            self.assertEqual(VERIFY.junit_results(crashed)["AR-03.a"], "failed")

    def test_failure_and_skip_are_not_passed(self):
        case = {"id": "AR-03.a", "stage": "R0", "required": True, "layer": "pure", "runner": "ctest",
                "test_names": ["AR-03.a"], "evidence_requirements": []}
        manifest = {"stages": [f"R{i}" for i in range(7)], "requirements": [case]}
        for result in ("failed", "skipped", "timeout"):
            mapped = {"AR-03.a": "failed" if result == "timeout" else result}
            with self.subTest(result=result):
                self.assertNotEqual(VERIFY.evaluate_cases(manifest, {"AR-03.a"}, mapped, "R0")[0]["status"], "passed")

    def test_later_stage_does_not_pass_when_required_case_is_missing(self):
        report = VERIFY.evaluate_cases(self.manifest, {"AR-03.a"}, {"AR-03.a": "passed"}, "R6")
        self.assertEqual(len(report), 110)
        self.assertGreater(sum(row["status"] == "not_run" for row in report), 0)

    def test_timeout_failure_is_distinct_and_not_passed(self):
        with tempfile.TemporaryDirectory() as directory:
            junit = Path(directory) / "timeout.xml"
            junit.write_text('<testsuite><testcase name="AR-03.a"><failure message="timeout">Timed out</failure></testcase></testsuite>', encoding="utf-8")
            parsed = VERIFY.junit_results(junit)
        self.assertEqual(parsed["AR-03.a"], "timeout")

    def test_missing_expired_wrong_hash_and_forged_evidence_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            good = root / "capture.bin"
            good.write_bytes(b"current evidence")
            digest = hashlib.sha256(good.read_bytes()).hexdigest()
            self.assertIsNone(VERIFY.verify_evidence_record({"path": "capture.bin", "sha256": digest}, root))
            self.assertIn("missing", VERIFY.verify_evidence_record(None, root))
            self.assertIn("missing or expired", VERIFY.verify_evidence_record({"path": "gone.png", "sha256": digest}, root))
            self.assertIn("hash mismatch", VERIFY.verify_evidence_record({"path": "capture.bin", "sha256": "0" * 64}, root))
            self.assertIn("fields missing", VERIFY.verify_evidence_record({"status": "passed"}, root))
            self.assertIn("stale", VERIFY.verify_evidence_record(
                {"path": "capture.bin", "sha256": digest, "source_tree_hash": "old"}, root,
                source_tree_hash="current"))

    def test_main_gate_inventory_keeps_quality_build_and_loop_requirements(self):
        self.assertTrue({"BLD-01", "BLD-02", "BLD-03", "BLD-04", "LOOP-01"}.issubset(GATE.required_test_ids("G0")))
        self.assertTrue({"UI-01", "UI-02"}.issubset(GATE.required_test_ids("G1")))
        self.assertTrue({"QUAL-01", "QUAL-02", "QUAL-03"}.issubset(GATE.required_test_ids("G4")))
        self.assertTrue({"UI-04", "UI-07"}.issubset(GATE.required_test_ids("G5")))
        required = GATE.required_test_ids("G6")
        self.assertTrue({"QUAL-01", "QUAL-02", "QUAL-03", "BLD-01", "BLD-02", "BLD-03", "LOOP-01", "LOOP-02"}.issubset(required))

    def test_report_shape_requires_timestamp_source_commands_and_case_reasons(self):
        report = {"schema_version": 1, "stage": "R0", "timestamp_utc": "2026-10-18T00:00:00Z",
                  "source_tree_hash": "abc", "commands": [{"argv": ["ctest"], "cwd": "workspace",
                  "started_utc": "2026-10-18T00:00:00Z", "ended_utc": "2026-10-18T00:00:01Z",
                  "exit_code": 0, "log_path": "ctest.log", "log_sha256": "0" * 64}], "artifacts": [],
                  "cases": [{"id": "AR-03.a", "status": "passed", "reason": "JUnit case executed"}]}
        self.assertEqual(VERIFY.validate_report(report), [])
        self.assertTrue(VERIFY.validate_report({"status": "passed"}))
        report["commands"][0].pop("log_sha256")
        self.assertTrue(VERIFY.validate_report(report))

    def test_failure_probe_proof_requires_red_gate_and_clean_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failed = root / "gate-failed.json"
            clean = root / "gate-clean.json"
            failed.write_text('{"status":"failed"}', encoding="utf-8")
            clean.write_text('{"status":"passed"}', encoding="utf-8")
            proof = {
                "status": "red_observed", "gate_status": "failed", "exit_code": 8,
                "gate_exit_code": 1, "gate_ctest_exit_code": 8,
                "probe_named_in_gate_failure": True, "expected_nonzero": True,
                "configure_off_exit_code": 0, "probe_absent_after_restore": True,
                "clean_gate_exit_code": 0, "clean_gate_status": "passed",
                "gate_report_path": str(failed), "gate_report_sha256": hashlib.sha256(failed.read_bytes()).hexdigest(),
                "clean_gate_report_path": str(clean), "clean_gate_report_sha256": hashlib.sha256(clean.read_bytes()).hexdigest(),
            }
            artifact = root / "proof.json"
            artifact.write_text(json.dumps(proof), encoding="utf-8")
            record = {"path": "proof.json", "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
                      "source_tree_hash": "current"}
            self.assertIsNone(VERIFY.verify_gate_probe_record(record, root, "current"))
            proof["probe_absent_after_restore"] = False
            artifact.write_text(json.dumps(proof), encoding="utf-8")
            record["sha256"] = hashlib.sha256(artifact.read_bytes()).hexdigest()
            self.assertIn("restored", VERIFY.verify_gate_probe_record(record, root, "current"))


CASE_METHODS = {
    "a": ("test_manifest_exact_110_case_coverage_and_hash", "test_missing_duplicate_and_document_hash_fail_closed",
          "test_shared_facts_require_exact_owners_and_preserve_each_case_evidence",
          "test_source_scan_prunes_ignored_trees_and_preserves_digest_contracts"),
    "b": ("test_zero_tests_missing_registration_and_unexecuted_are_not_passed", "test_wrong_configuration_binary_and_crashed_process_are_rejected",
          "test_later_stage_does_not_pass_when_required_case_is_missing"),
    "c": ("test_failure_and_skip_are_not_passed", "test_timeout_failure_is_distinct_and_not_passed"),
    "d": ("test_missing_expired_wrong_hash_and_forged_evidence_are_rejected",),
    "e": ("test_main_gate_inventory_keeps_quality_build_and_loop_requirements",),
    "f": ("test_report_shape_requires_timestamp_source_commands_and_case_reasons",),
    "g": ("test_failure_probe_proof_requires_red_gate_and_clean_restore",),
    "h": ("test_source_provenance_excludes_ephemeral_test_scratch",),
}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=sorted(CASE_METHODS))
    args = parser.parse_args()
    if args.case:
        suite = unittest.TestSuite()
        for method in CASE_METHODS[args.case]:
            suite.addTest(ArchitectureVerifierTests(method))
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        sys.exit(0 if result.wasSuccessful() else 1)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
