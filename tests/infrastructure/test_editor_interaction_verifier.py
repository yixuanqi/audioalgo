"""Contract tests for the fail-closed NI verifier."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "audioalgo_verify_editor_interaction", ROOT / "scripts" / "verify_editor_interaction.py"
)
assert SPEC is not None and SPEC.loader is not None
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)

MANIFEST_PATH = ROOT / "tests" / "editor_interaction_manifest.json"
EXPECTED_SUBCASES = {
    "NED-01": {"a", "b", "c"},
    "NED-02": {"a", "b", "c", "d"},
    "NED-03": {"a", "b", "c", "d", "e"},
    "NED-04": {"a", "b", "c"},
    "NED-05": {"a", "b", "c", "d", "e"},
    "NED-06": {"a", "b", "c", "d", "e"},
    "NED-07": {"a", "b", "c", "d", "e", "f"},
    "NED-08": {"a", "b", "c", "d", "e"},
    "NED-09": {"a", "b", "c", "d", "e"},
    "NED-10": {"a", "b", "c", "d", "e", "f", "g", "h"},
    "NED-11": {"a", "b", "c", "d", "e", "f", "g"},
    "NED-12": {"a", "b", "c", "d"},
    "NED-13": {"a", "b", "c", "d"},
    "NED-14": {"a", "b", "c", "d", "e"},
}


def issue_codes(report: dict) -> set[str]:
    return {issue["code"] for issue in report["issues"]}


class EditorInteractionVerifierContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = verify.load_manifest(MANIFEST_PATH)
        cls.test_names = [case["test_name"] for req in cls.manifest["requirements"] for case in req["subcases"]]

    def evaluate_all_passed(self, *, manifest=None, inventory=None, results=None, evidence=None):
        return verify.evaluate(
            self.manifest if manifest is None else manifest,
            self.test_names if inventory is None else inventory,
            {name: {"status": "passed", "exit_code": 0} for name in self.test_names} if results is None else results,
            {"schema_version": 1, "records": []} if evidence is None else evidence,
            current_source_tree_hash="contract-test-source-hash",
            command_records=[{"argv": ["ctest"], "exit_code": 0, "log_path": "baseline.log", "log_sha256": "log-hash"}],
        )

    def test_a_manifest_enumerates_all_required_ids_and_subcases(self):
        requirements = {item["id"]: {case["id"] for case in item["subcases"]} for item in self.manifest["requirements"]}
        self.assertEqual(requirements, EXPECTED_SUBCASES)
        self.assertEqual(verify.validate_manifest(self.manifest), [])

    def test_b0_zero_required_test_collection_is_rejected(self):
        report = self.evaluate_all_passed(inventory=[], results={})
        self.assertEqual(report["status"], "failed")
        self.assertIn("zero_required_tests", issue_codes(report))

    def test_b1_missing_requirement_id_is_rejected(self):
        manifest = dict(self.manifest)
        manifest["requirements"] = [item for item in self.manifest["requirements"] if item["id"] != "NED-14"]
        report = self.evaluate_all_passed(manifest=manifest)
        self.assertEqual(report["status"], "failed")
        self.assertIn("manifest_id_coverage", issue_codes(report))

    def test_b2_missing_required_subcase_is_rejected(self):
        manifest = {**self.manifest, "requirements": [dict(item) for item in self.manifest["requirements"]]}
        item = next(item for item in manifest["requirements"] if item["id"] == "NED-01")
        item["subcases"] = [case for case in item["subcases"] if case["id"] != "b"]
        report = self.evaluate_all_passed(manifest=manifest)
        self.assertEqual(report["status"], "failed")
        self.assertIn("manifest_subcase_coverage", issue_codes(report))

    def test_b3_assertion_failure_is_not_passed(self):
        results = {name: {"status": "passed", "exit_code": 0} for name in self.test_names}
        results[self.test_names[0]] = {"status": "failed", "exit_code": 1}
        report = self.evaluate_all_passed(results=results)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["subcases"][0]["status"], "failed")

    def test_b4_nonzero_exit_is_not_passed(self):
        results = {name: {"status": "passed", "exit_code": 0} for name in self.test_names}
        results[self.test_names[0]] = {"status": "passed", "exit_code": 2}
        report = self.evaluate_all_passed(results=results)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["subcases"][0]["status"], "failed")

    def test_b5_timeout_is_not_passed(self):
        results = {name: {"status": "passed", "exit_code": 0} for name in self.test_names}
        results[self.test_names[0]] = {"status": "timeout", "exit_code": 124}
        report = self.evaluate_all_passed(results=results)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["subcases"][0]["status"], "timeout")

    def test_b6_skipped_not_run_and_not_verified_are_not_passed(self):
        case = next(case for req in self.manifest["requirements"] for case in req["subcases"] if not case["desktop_required"])
        for status in ("skipped", "not_run", "not_verified"):
            with self.subTest(status=status):
                results = {name: {"status": "passed", "exit_code": 0} for name in self.test_names}
                results[case["test_name"]] = {"status": status, "exit_code": 0}
                report = self.evaluate_all_passed(results=results)
                self.assertEqual(report["status"], "failed")
                checked = next(item for item in report["subcases"] if item["id"] == case["full_id"])
                self.assertEqual(checked["status"], status)

    def test_b7_desktop_status_without_artifacts_is_not_verified(self):
        desktop_case = next(case for req in self.manifest["requirements"] for case in req["subcases"] if case["desktop_required"])
        invalid_evidence_cases = (
            {"schema_version": 1, "records": []},
            {"schema_version": 1, "records": [{"subcase_id": desktop_case["full_id"], "status": "passed"}]},
        )
        for evidence in invalid_evidence_cases:
            with self.subTest(evidence=evidence):
                report = self.evaluate_all_passed(evidence=evidence)
                checked = next(item for item in report["subcases"] if item["id"] == desktop_case["full_id"])
                self.assertEqual(checked["status"], "not_verified")
                self.assertIn("desktop_evidence_missing_or_invalid", issue_codes(report))

    def test_c_failure_probe_exit_code_is_reported_as_red(self):
        probe = verify.run_controlled_failure_probe()
        self.assertNotEqual(probe["exit_code"], 0)
        self.assertIn("probe_log_sha256", probe)

    def test_d_report_records_source_hash_and_command_logs(self):
        report = self.evaluate_all_passed()
        self.assertEqual(report["source_tree_hash"], "contract-test-source-hash")
        self.assertEqual(report["commands"][0]["log_sha256"], "log-hash")
        self.assertTrue(all("test_name" in item and "status" in item for item in report["subcases"]))


TEST_GROUPS = {
    "a": ["test_a_manifest_enumerates_all_required_ids_and_subcases"],
    "b": [
        "test_b0_zero_required_test_collection_is_rejected",
        "test_b1_missing_requirement_id_is_rejected",
        "test_b2_missing_required_subcase_is_rejected",
        "test_b3_assertion_failure_is_not_passed",
        "test_b4_nonzero_exit_is_not_passed",
        "test_b5_timeout_is_not_passed",
        "test_b6_skipped_not_run_and_not_verified_are_not_passed",
        "test_b7_desktop_status_without_artifacts_is_not_verified",
    ],
    "c": ["test_c_failure_probe_exit_code_is_reported_as_red"],
    "d": ["test_d_report_records_source_hash_and_command_logs"],
}


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--group":
        group = TEST_GROUPS.get(sys.argv[2])
        if group is None:
            raise SystemExit(f"unknown test group: {sys.argv[2]}")
        suite = unittest.TestSuite(
            EditorInteractionVerifierContract(name) for name in group
        )
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main(verbosity=2)
