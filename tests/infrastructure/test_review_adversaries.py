"""Regression adversaries for the Review findings F2 (desktop evidence) and F3 (stage drift).

Every helper takes the verifier module explicitly so the same adversaries can be replayed
against the archived pre-fix snapshot as Red evidence and against the current scripts as Green.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CURRENT = ROOT / "scripts" / "verify_architecture_refactor.py"


def load_verifier(path: Path, name: str = "architecture_verifier"):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def adversary_stage_drift(verifier, manifest: dict, document: str) -> list[str]:
    """Moving a required case to a later stage must be rejected by the manifest validator."""
    changed = copy.deepcopy(manifest)
    target = next(item for item in changed["requirements"] if item["id"] == "AR-03.a")
    target["stage"] = "R6"
    return verifier.validate_manifest(changed, document)


def desktop_case(state: str, verifier, manifest: dict) -> dict:
    requirement = next(item for item in manifest["requirements"] if item["id"] == "AR-11.b")
    entry = copy.deepcopy(requirement)
    entry["test_names"] = ["desktop-test"]
    entry["desktop_required"] = True
    entry["runner"] = "ctest"
    entry["layer"] = "component"
    entry["required"] = True
    entry["stage"] = "R3"
    small = {"stages": [f"R{i}" for i in range(7)], "requirements": [entry]}
    # Use the verifier's own workspace root so the generic record really hashes cleanly;
    # only the desktop-specific artifacts are bogus, exactly like the Review's victim input.
    verifier_root = Path(getattr(verifier, "ROOT", ROOT))
    artifact = verifier_root / "tests" / "architecture_refactor_manifest.json"
    proof = {field: "placeholder" for field in verifier.desktop_fields()}
    proof.update(path=str(artifact),
                 sha256=verifier.sha(artifact),
                 source_tree_hash="adversary-source",
                 operation="add Gain from library",
                 os="Windows-10-10.0.19045-SP0",
                 display_scale_percent=150,
                 physical_client_size="1920x1200",
                 logical_client_size="1280x800",
                 timestamp_utc="2026-10-18T02:00:00Z",
                 screenshot_path="DOES_NOT_EXIST.png",
                 screenshot_sha256="a" * 64,
                 executable_path="DOES_NOT_EXIST.exe",
                 executable_sha256="b" * 64)
    return verifier.evaluate_cases(small, {"desktop-test"}, {"desktop-test": state}, "R3",
                                   False, {"AR-11.b": proof}, "adversary-source")[0]


def valid_desktop_record(root: Path, verifier, source_hash: str) -> tuple[dict, Path]:
    """Build a fully self-consistent desktop record whose artifacts really exist."""
    base = root / "desktop"
    base.mkdir(parents=True, exist_ok=True)
    screenshot = base / "capture.png"
    executable = base / "AudioAlgo Editor.exe"
    record = base / "record.json"
    screenshot.write_bytes(b"\x89PNG\r\n\x1a\ncontrolled-capture")
    executable.write_bytes(b"MZ controlled executable bytes")
    payload = {
        "operation": "add Gain from library and run",
        "os": "Windows-10-10.0.19045-SP0",
        "display_scale_percent": 150,
        "physical_client_size": "1920x1200",
        "logical_client_size": "1280x800",
        "executable_path": str(executable),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "source_tree_hash": source_hash,
        "timestamp_utc": "2026-10-18T02:00:00Z",
        "screenshot_path": str(screenshot),
        "screenshot_sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
        "path": str(record),
        "sha256": "",
    }
    record.write_text(json.dumps(payload), encoding="utf-8")
    payload["sha256"] = hashlib.sha256(record.read_bytes()).hexdigest()
    record.write_text(json.dumps(payload), encoding="utf-8")
    payload["sha256"] = hashlib.sha256(record.read_bytes()).hexdigest()
    return payload, base


class ReviewAdversaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.verifier = load_verifier(CURRENT)
        cls.document = (ROOT / "doc" / "testing_architecture_refactor.md").read_text(encoding="utf-8")
        cls.manifest = json.loads((ROOT / "tests" / "architecture_refactor_manifest.json").read_text(encoding="utf-8"))

    def test_stage_drift_is_rejected(self):
        self.assertEqual(self.verifier.validate_manifest(self.manifest, self.document), [])
        errors = adversary_stage_drift(self.verifier, self.manifest, self.document)
        self.assertTrue(errors, "moving AR-03.a to R6 must not validate")
        self.assertTrue(any("AR-03.a" in error for error in errors))

    def test_desktop_case_never_passes_on_a_skipped_or_failed_test(self):
        skipped = desktop_case("skipped", self.verifier, self.manifest)
        failed = desktop_case("failed", self.verifier, self.manifest)
        self.assertNotEqual(skipped["status"], "passed")
        self.assertEqual(skipped["status"], "skipped")
        self.assertNotEqual(failed["status"], "passed")
        self.assertEqual(failed["status"], "failed")

    def test_missing_artifacts_are_not_verified_even_when_the_test_passed(self):
        passed = desktop_case("passed", self.verifier, self.manifest)
        self.assertEqual(passed["status"], "not_verified")
        self.assertIn("screenshot", passed["reason"] + "executable")

    def test_forged_or_empty_desktop_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record, base = valid_desktop_record(root, self.verifier, "src-hash")
            self.assertIsNone(self.verifier.verify_desktop_record(record, "src-hash", root))

            def broken(mutate) -> str | None:
                candidate = copy.deepcopy(record)
                mutate(candidate)
                return self.verifier.verify_desktop_record(candidate, "src-hash", root)

            self.assertIn("screenshot", broken(lambda item: (base / "capture.png").unlink()))
            (base / "capture.png").write_bytes(b"\x89PNG\r\n\x1a\ncontrolled-capture")
            self.assertIn("operation", broken(lambda item: item.update(operation="   ")))
            self.assertIn("stale", broken(lambda item: item.update(source_tree_hash="old")))
            self.assertIn("executable", broken(
                lambda item: item.update(executable_sha256="0" * 64)))
            self.assertIn("escapes", broken(
                lambda item: item.update(executable_path="../../../outside.exe")))
            shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
