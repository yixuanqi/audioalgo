"""AR-01 evidence checks for the frozen legacy and current test inventories."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENV_SPEC = importlib.util.spec_from_file_location("editor_verifier", ROOT / "scripts" / "verify_editor_interaction.py")
ENV_VERIFY = importlib.util.module_from_spec(ENV_SPEC)
ENV_SPEC.loader.exec_module(ENV_VERIFY)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def current_source_manifest() -> dict[str, str]:
    ignored = {".git", "build", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
    return {path.relative_to(ROOT).as_posix(): sha(path) for path in sorted(ROOT.rglob("*"))
            if path.is_file() and not ignored.intersection(path.relative_to(ROOT).parts)}


def junit(path: Path) -> tuple[set[str], int]:
    cases = list(ET.parse(path).getroot().iter("testcase"))
    skipped = sum(case.find("./skipped") is not None for case in cases)
    bad = sum(case.find("./failure") is not None or case.find("./error") is not None for case in cases)
    if bad:
        raise AssertionError(f"{path} contains {bad} failing cases")
    return {case.attrib["name"] for case in cases}, skipped


def load_json(path: str) -> dict:
    return json.loads((ROOT / path).read_text(encoding="utf-8"))


def check_recorded_artifacts(record: dict) -> None:
    for item in record.get("artifacts", []):
        path = Path(item["path"])
        if item.get("status") == "missing" or not path.is_file():
            raise AssertionError(f"recorded artifact is missing: {path}")
        if sha(path) != item["sha256"]:
            raise AssertionError(f"recorded artifact hash mismatch: {path}")


class ArchitectureBaselineTests(unittest.TestCase):
    def test_a_current_environment_configs_and_real_baselines(self):
        environment = load_json("build/reports/environment.json")
        self.assertEqual(environment["revision"], subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True, capture_output=True,
            text=True, encoding="utf-8", errors="replace").stdout.strip())
        self.assertEqual(environment["source_tree_hash"], ENV_VERIFY.source_tree_hash())
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        self.assertEqual({item["name"] for item in presets["configurePresets"]},
                         {"windows-debug", "windows-headless", "windows-release", "windows-application-only"})
        application_only = next(item for item in presets["configurePresets"]
                                if item["name"] == "windows-application-only")
        self.assertEqual(application_only["cacheVariables"]["AUDIO_BUILD_RUNTIME"], "OFF")
        self.assertEqual(application_only["cacheVariables"]["AUDIO_BUILD_EDITOR"], "OFF")
        self.assertEqual(application_only["cacheVariables"]["AUDIO_BUILD_TESTS"], "ON")
        self.assertEqual(application_only["cacheVariables"]["AUDIO_BUILD_INFRASTRUCTURE_TESTS"], "ON")
        expectations = {
            "debug": ("build/reports/loops/RF0000/baseline-debug-build-sdk.json",
                      "build/reports/loops/RF0000/baseline-debug-ctest.json",
                      "build/windows-debug/build/reports/loops/RF0000/baseline-debug-junit.xml", 88),
            "headless": ("build/reports/loops/RF0000/baseline-headless-build.json",
                         "build/reports/loops/RF0000/baseline-headless-ctest.json",
                         "build/windows-headless/build/reports/loops/RF0000/baseline-headless-junit.xml", 87),
        }
        for name, (build_path, ctest_path, junit_path, expected_count) in expectations.items():
            with self.subTest(configuration=name):
                build = load_json(build_path)
                ctest = load_json(ctest_path)
                self.assertEqual(build["exit_code"], 0)
                self.assertEqual(ctest["exit_code"], 0)
                names, skipped = junit(ROOT / junit_path)
                self.assertEqual(len(names), expected_count)
                self.assertEqual(skipped, 0)
                log = Path(ctest["log_path"]).read_text(encoding="utf-8", errors="replace")
                self.assertIn(f"100% tests passed, 0 tests failed out of {expected_count}", log)

        # The live inventory must grow with new registrations without ever dropping a legacy-proven
        # case, so this checks each configuration's own mapped set instead of freezing a brittle total.
        mapping = load_json("tests/legacy_test_mapping.json")
        legacy_by_configuration = {
            name: {item["legacy_test_name"] for item in row["mapping"]}
            for name, row in mapping["configurations"].items()}
        self.assertTrue(all(legacy_by_configuration.values()), "legacy scenario mapping is empty")

        current = {"debug": ("build/windows-debug", "Debug"),
                   "headless": ("build/windows-headless", "Debug"),
                   "release": ("build/windows-release", "Release")}
        for name, (build_relative, configuration) in current.items():
            with self.subTest(current_configuration=name):
                build_dir = ROOT / build_relative
                inventory = subprocess.run(
                    ["ctest", "--test-dir", str(build_dir), "-C", configuration, "--show-only=json-v1", "--no-tests=error"],
                    cwd=ROOT, check=True, capture_output=True, text=True, encoding="utf-8", errors="replace")
                rows = json.loads(inventory.stdout)["tests"]
                names = {row["name"] for row in rows}
                self.assertGreater(len(rows), 0)
                self.assertTrue(all(row.get("command") for row in rows))
                self.assertEqual(len(names), len(rows), "test names must be unique in the inventory")
                legacy_names = legacy_by_configuration.get(name)
                if legacy_names is not None:
                    self.assertTrue(legacy_names.issubset(names),
                                    f"{name} inventory dropped legacy cases: {sorted(legacy_names - names)}")

    def test_b_frozen_fixture_and_independent_cli_oracle(self):
        fixture_dir = ROOT / "build/reports/loops/RF0000/legacy_fixture"
        fixture = json.loads((fixture_dir / "fixture.json").read_text(encoding="utf-8"))
        for name, details in fixture["frozen_files"].items():
            path = fixture_dir / name
            self.assertEqual(sha(path), details["sha256"])
            self.assertEqual(path.stat().st_size, details["bytes"])
            self.assertEqual(path.stat().st_mode & 0o222, 0, f"fixture is writable: {name}")
        self.assertEqual(sha(fixture_dir / "fixture.json"), "283a5dd13b82d47ccca66d2ad5edeb4090484f86cd5b370425aa57bc14da04e2")
        run = load_json("build/runs/RF0000-legacy/run.json")
        complete = load_json("build/runs/RF0000-legacy/complete.json")
        cli = load_json("build/reports/loops/RF0000/legacy-cli.json")
        oracle = load_json("build/runs/RF0000-legacy/independent_verification.json")
        self.assertEqual(cli["exit_code"], 0)
        self.assertEqual(run["run_id"], complete["run_id"])
        self.assertEqual(run["project_id"], fixture["project_id"])
        self.assertEqual(run["project_revision"], fixture["revision"])
        self.assertEqual(run["input_sha256"], fixture["frozen_files"]["input.wav"]["sha256"])
        self.assertEqual(run["output_sha256"], oracle["output_sha256"])
        self.assertEqual(run["output_sha256"], next(item["sha256"] for item in cli["artifacts"] if item["path"].endswith("output.wav")))
        self.assertEqual(oracle["status"], "passed")
        self.assertEqual(oracle["sample_count"], 192000)
        self.assertEqual(oracle["failures"], [])

    def test_c_every_legacy_registration_maps_without_dropping_ned_desktop(self):
        mapping = load_json("tests/legacy_test_mapping.json")
        self.assertEqual(mapping["legacy_acceptance_documents"]["pipeline"]["sha256"],
                         sha(ROOT / "doc/testing_audio_pipeline_editor.md"))
        self.assertEqual(mapping["legacy_acceptance_documents"]["node_interaction"]["sha256"],
                         sha(ROOT / "doc/testing_editor_node_interaction.md"))
        expected_desktop = sorted(
            f"{item['id']}.{letter}"
            for item in json.loads((ROOT / "tests/editor_interaction_manifest.json").read_text(encoding="utf-8"))["requirements"]
            for letter in item.get("desktop_subcases", []))
        self.assertEqual(mapping["desktop_required_ned_subcases"], expected_desktop)
        required_categories = {"CORE", "DSP", "GRAPH", "IO", "PRJ", "E2E", "NED"}
        for configuration, row in mapping["configurations"].items():
            with self.subTest(configuration=configuration):
                legacy = row["legacy_junit"]
                current = row["current_inventory"]
                self.assertEqual(sha(ROOT / legacy["path"]), legacy["sha256"])
                old_names = set(legacy["test_names"])
                inventory = subprocess.run(["ctest", "--test-dir", str(ROOT / current["build_dir"]),
                                            "-C", "Debug", "--show-only=json-v1", "--no-tests=error"],
                                           cwd=ROOT, check=True, capture_output=True,
                                           text=True, encoding="utf-8", errors="replace")
                current_names = {item["name"] for item in json.loads(inventory.stdout)["tests"]}
                records = row["mapping"]
                names_hash = hashlib.sha256(json.dumps(sorted(current_names), ensure_ascii=False).encode()).hexdigest()
                self.assertEqual(names_hash, current["inventory_sha256"])
                self.assertEqual({item["legacy_test_name"] for item in records}, old_names)
                self.assertEqual({item["current_test_name"] for item in records}, old_names)
                self.assertTrue(old_names.issubset(current_names))
                self.assertEqual(len(records), len({item["current_test_name"] for item in records}))
                self.assertTrue(required_categories.issubset({item["category"] for item in records}))
                for case_id in mapping["desktop_required_ned_subcases"]:
                    self.assertIn(case_id, current_names)

    def test_d_fresh_build_commands_bind_current_sources_and_binary_hashes(self):
        live_sources = current_source_manifest()
        source_hash = hashlib.sha256(json.dumps(live_sources, sort_keys=True).encode()).hexdigest()
        for name in ("debug", "headless"):
            with self.subTest(configuration=name):
                # Auto-follow refreshes: pick the newest build record that already binds this source set,
                # so a new loop never needs to edit this check while a stale record still fails it.
                candidates = sorted(ROOT.glob(f"build/reports/loops/RF*/**/{name}-current-final-build*.json"))
                self.assertTrue(candidates, f"no {name} build record exists")
                matching = []
                for path in candidates:
                    try:
                        data = json.loads(path.read_text(encoding="utf-8"))
                    except json.JSONDecodeError:
                        continue
                    if data.get("source_tree_hash") == source_hash:
                        matching.append((path, data))
                self.assertTrue(matching,
                                f"no {name} build record is bound to the current source set; "
                                f"refresh the build record for {source_hash}")
                path, record = matching[-1]
                self.assertEqual(record["exit_code"], 0, f"{path} did not exit zero")
                self.assertEqual(record["sources"], live_sources, f"{path} source manifest is stale")
                self.assertTrue(record["command"])
                self.assertIn("--build", record["command"])
                check_recorded_artifacts(record)
                self.assertGreaterEqual(len(record["artifacts"]), 3)


CASES = {letter: f"test_{letter}_{description}" for letter, description in (
    ("a", "current_environment_configs_and_real_baselines"),
    ("b", "frozen_fixture_and_independent_cli_oracle"),
    ("c", "every_legacy_registration_maps_without_dropping_ned_desktop"),
    ("d", "fresh_build_commands_bind_current_sources_and_binary_hashes"),
)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=sorted(CASES), required=True)
    case = parser.parse_args().case
    suite = unittest.TestSuite([ArchitectureBaselineTests(CASES[case])])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
