"""Adversarial contract tests for the CNL fail-closed acceptance checker."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "audioalgo_verify_connection_library", ROOT / "scripts" / "verify_connection_library.py"
)
assert SPEC is not None and SPEC.loader is not None
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)

ACCEPTANCE_TEXT = (ROOT / verify.ACCEPTANCE_PATH).read_text(encoding="utf-8")
ACCEPTANCE_SHA256 = verify.sha256((ROOT / verify.ACCEPTANCE_PATH).read_bytes())
TEST_NAME = "NCE-01.b Inspector disconnects every incident edge in one undoable edit while preserving the node"


def manifest_template() -> dict:
    return {
        "schema_version": 1,
        "acceptance_document": verify.ACCEPTANCE_PATH,
        "acceptance_sha256": ACCEPTANCE_SHA256,
        "items": [{"id": item_id, "status": "not_run", "test_names": [], "verified_layers": []}
                  for item_id in verify.expected_ids()],
    }


def record_pass(root: Path, manifest: dict, item_id: str = "NCE-01.b", test_name: str = TEST_NAME,
                layers: list[str] | None = None) -> dict:
    item = next(value for value in manifest["items"] if value["id"] == item_id)
    item["status"] = "passed"
    item["test_names"] = [test_name]
    item["verified_layers"] = layers or sorted(verify.parse_acceptance(ACCEPTANCE_TEXT)[item_id])
    red_path = root / "red.log"
    green_path = root / "green.log"
    regression_path = root / "regression.log"
    source_path = root / "test_source.cpp"
    binary_path = root / "build" / "test_binary.exe"
    binary_path.parent.mkdir(parents=True, exist_ok=True)
    source_path.write_text("// synthetic verifier fixture\n", encoding="utf-8")
    binary_path.write_bytes(b"synthetic test binary")
    source_files = [{"path": "test_source.cpp", "sha256": verify.sha256(source_path.read_bytes())}]
    binary_artifacts = [{"path": "build/test_binary.exe", "sha256": verify.sha256(binary_path.read_bytes())}]
    command = ["ctest", "--test-dir", "build", "-R", f"^{test_name}$"]
    red_path.write_text(f"{test_name}\nFAILED: REQUIRE(button != nullptr)\n", encoding="utf-8")
    green_path.write_text(f"{test_name}\nAll tests passed (43 assertions in 1 test case)\n100% tests passed\n", encoding="utf-8")
    regression_path.write_text(f"{test_name}\n100% tests passed\n", encoding="utf-8")
    item["source_files"] = source_files
    item["source_snapshot_sha256"] = verify.source_snapshot_sha256(source_files)
    item["binary_artifacts"] = binary_artifacts
    item["direct_regression"] = {
        "test_names": [test_name], "argv": command, "cwd": str(root), "exit_code": 0,
        "log_path": "regression.log", "log_sha256": verify.sha256(regression_path.read_bytes()),
    }
    item["tdd"] = {
        "red": {"kind": "behavior_red", "exit_code": 8, "argv": command, "cwd": str(root), "log_path": "red.log",
                "log_sha256": verify.sha256(red_path.read_bytes()), "source_files": source_files,
                "source_snapshot_sha256": verify.source_snapshot_sha256(source_files),
                "binary_artifacts": binary_artifacts},
        "green": {"kind": "behavior_green", "exit_code": 0, "argv": command, "cwd": str(root), "log_path": "green.log",
                  "log_sha256": verify.sha256(green_path.read_bytes())},
    }
    return item


def issue_codes(issues: list[dict[str, str]]) -> set[str]:
    return {issue["code"] for issue in issues}


class ConnectionLibraryVerifierContract(unittest.TestCase):
    def test_a_fixed_contract_has_exactly_66_independent_ids(self):
        expected = verify.expected_ids()
        acceptance = verify.parse_acceptance(ACCEPTANCE_TEXT)
        self.assertEqual(len(expected), 66)
        self.assertEqual(set(expected), set(acceptance))
        self.assertEqual(len(acceptance), 66)

    def test_b_missing_and_duplicate_ids_are_rejected(self):
        manifest = manifest_template()
        manifest["items"].pop()
        manifest["items"].append(manifest["items"][0])
        errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, ["one test"], ROOT, ACCEPTANCE_SHA256)
        self.assertIn("duplicate_id", issue_codes(errors))
        self.assertIn("manifest_id_coverage", issue_codes(errors))

    def test_c_zero_test_collection_cannot_support_a_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            record_pass(root, manifest)
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [], root, ACCEPTANCE_SHA256)
            self.assertIn("zero_test_collection", issue_codes(errors))
            self.assertIn("passed_test_unregistered", issue_codes(errors))

    def test_d_passed_item_requires_hashed_red_green_and_exact_registration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            record_pass(root, manifest)
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertEqual(errors, [])
            item = next(value for value in manifest["items"] if value["id"] == "NCE-01.b")
            item["tdd"]["green"]["exit_code"] = 8
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("green_command_nonzero", issue_codes(errors))
            item["tdd"]["green"]["exit_code"] = 0
            item["tdd"]["green"]["argv"] = ["ctest", "--test-dir", "build", "-R", "some-other-test"]
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("command_test_filter_mismatch", issue_codes(errors))
            item["tdd"]["green"]["argv"] = ["ctest", "--test-dir", "build", "-R", f"^{TEST_NAME}$"]
            item["tdd"]["green"]["cwd"] = str(root / "wrong-directory")
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("command_cwd_mismatch", issue_codes(errors))
            item["tdd"]["green"]["cwd"] = str(root)
            item["source_files"][0]["sha256"] = "0" * 64
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("source_hash_mismatch", issue_codes(errors))
            item["source_files"][0]["sha256"] = verify.sha256((root / "test_source.cpp").read_bytes())
            item["source_snapshot_sha256"] = verify.source_snapshot_sha256(item["source_files"])
            item["tdd"]["red"]["source_files"][0]["sha256"] = "0" * 64
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("source_hash_mismatch", issue_codes(errors))
            item["tdd"]["red"]["source_files"][0]["sha256"] = verify.sha256((root / "test_source.cpp").read_bytes())
            (root / "build" / "test_binary.exe").write_bytes(b"changed synthetic binary")
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("binary_hash_mismatch", issue_codes(errors))
            item["source_files"] = []
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("source_identity_missing", issue_codes(errors))
            item["binary_artifacts"] = []
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("binary_identity_missing", issue_codes(errors))

    def test_e_missing_or_changed_acceptance_binding_is_rejected(self):
        manifest = manifest_template()
        manifest["acceptance_sha256"] = "0" * 64
        errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], ROOT, ACCEPTANCE_SHA256)
        self.assertIn("acceptance_hash_mismatch", issue_codes(errors))

    def test_f_component_evidence_cannot_pass_a_desktop_required_item(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            item = record_pass(root, manifest, "NCE-01.a", "synthetic desktop case", ["P", "C", "D"])
            item["desktop_evidence"] = {"type": "component_png", "artifacts": []}
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, ["synthetic desktop case"], root,
                                              ACCEPTANCE_SHA256)
            self.assertIn("desktop_evidence_not_real", issue_codes(errors))

    def test_g_skipped_and_not_run_states_never_count_as_passed(self):
        manifest = manifest_template()
        manifest["items"][0]["status"] = "skipped"
        manifest["items"][1]["status"] = "not_run"
        report = verify.evaluate(manifest, ACCEPTANCE_TEXT, [TEST_NAME], ROOT, ACCEPTANCE_SHA256)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["passed_count"], 0)
        self.assertIn("required_item_not_passed", issue_codes(report["issues"]))

    def test_h_success_marker_cannot_hide_failure_skips_or_zero_collection(self):
        for phase, field in (("green", "tdd"), ("regression", "direct_regression")):
            for terminal in (
                "fixture.cpp(12): FAILED:\n100% tests passed, 0 tests failed out of 1\n",
                "1/1 Test #1: fixture ...***Skipped\n100% tests passed, 0 tests failed out of 1\n",
                "No tests were found!!!\n100% tests passed, 0 tests failed out of 0\n",
                "FAILED (failures=1)\n100% tests passed, 0 tests failed out of 1\n",
                "assertions: 7 | 6 passed | 1 failed\n100% tests passed, 0 tests failed out of 1\n",
                "50% tests passed, 1 tests failed out of 2\n100% tests passed, 0 tests failed out of 1\n",
                "1/1 Test #1: fixture ...***Timeout\n100% tests passed, 0 tests failed out of 1\n",
            ):
                with self.subTest(phase=phase, terminal=terminal), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    manifest = manifest_template()
                    item = record_pass(root, manifest)
                    record = item["tdd"]["green"] if field == "tdd" else item[field]
                    path = root / record["log_path"]
                    path.write_text(TEST_NAME + "\n" + terminal, encoding="utf-8")
                    record["log_sha256"] = verify.sha256(path.read_bytes())
                    errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
                    self.assertIn(phase + "_log_not_passed", issue_codes(errors))

    def test_i_missing_evidence_and_duplicate_registration_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            record_pass(root, manifest)
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME, TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("passed_test_unregistered", issue_codes(errors))
            (root / "green.log").unlink()
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("evidence_missing", issue_codes(errors))

    def test_j_failure_words_in_ordinary_diagnostics_are_not_terminal_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            item = record_pass(root, manifest)
            path = root / "green.log"
            path.write_text(TEST_NAME + "\nuser diagnostic mentions FAILED: inside a sentence\n"
                            "All tests passed (43 assertions in 1 test case)\n"
                            "100% tests passed, 0 tests failed out of 1\n", encoding="utf-8")
            item["tdd"]["green"]["log_sha256"] = verify.sha256(path.read_bytes())
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertEqual(errors, [])

    def test_k_missing_desktop_layer_or_artifacts_never_pass(self):
        for layers, desktop, expected in (
            (["P", "C"], None, "required_layer_missing"),
            (["P", "C", "D"], None, "desktop_evidence_not_real"),
            (["P", "C", "D"], {"type": "real_os_window", "artifacts": []}, "desktop_evidence_missing"),
        ):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                manifest = manifest_template()
                item = record_pass(root, manifest, "NCE-01.a", "synthetic desktop case", layers)
                if desktop is not None:
                    item["desktop_evidence"] = desktop
                errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, ["synthetic desktop case"], root,
                                                  ACCEPTANCE_SHA256)
                self.assertIn(expected, issue_codes(errors))

    def test_l_changed_evidence_hash_cannot_support_a_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = manifest_template()
            item = record_pass(root, manifest)
            item["tdd"]["green"]["log_sha256"] = "0" * 64
            errors = verify.validate_manifest(manifest, ACCEPTANCE_TEXT, [TEST_NAME], root, ACCEPTANCE_SHA256)
            self.assertIn("evidence_hash_mismatch", issue_codes(errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
