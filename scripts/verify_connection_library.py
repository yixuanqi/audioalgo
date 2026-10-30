#!/usr/bin/env python3
"""Fail-closed inventory and evidence checker for the CNL acceptance set."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ACCEPTANCE_PATH = "doc/testing_connection_editing_and_node_library.md"
GROUPS = {"NCE": 10, "NLC": 8, "NRG": 4}
LETTERS = ("a", "b", "c")
STATUSES = {"passed", "failed", "not_run", "not_verified", "blocked", "skipped", "timeout"}
RED_KINDS = {"behavior_red", "compile_red", "sensitivity_probe", "already_present"}


def expected_ids() -> list[str]:
    return [f"{group}-{number:02}.{letter}"
            for group, count in GROUPS.items()
            for number in range(1, count + 1)
            for letter in LETTERS]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_acceptance(text: str) -> dict[str, set[str]]:
    rows: dict[str, set[str]] = {}
    for line in text.splitlines():
        match = re.match(r"^\|\s*((?:NCE|NLC|NRG)-\d{2}\.[a-c])\s*\|\s*([^|]+)\|", line)
        if match:
            rows[match.group(1)] = set(re.findall(r"[PCIDF]", match.group(2)))
    return rows


def collect_inventory(ctest: str, build_dir: Path, cwd: Path) -> tuple[list[str], dict[str, Any]]:
    argv = [ctest, "--test-dir", str(build_dir), "-N"]
    completed = subprocess.run(argv, cwd=cwd, text=True, capture_output=True, check=False)
    output = completed.stdout + completed.stderr
    names = [match.group(1).strip() for line in output.splitlines()
             if (match := re.match(r"^\s*Test\s+#\d+:\s*(.+?)\s*$", line))]
    return names, {"argv": argv, "cwd": str(cwd), "exit_code": completed.returncode,
                   "output": output}


def _issue(code: str, message: str, item_id: str | None = None) -> dict[str, str]:
    result = {"code": code, "message": message}
    if item_id is not None:
        result["id"] = item_id
    return result


def _safe_artifact(root: Path, record: dict[str, Any], item_id: str,
                    errors: list[dict[str, str]], *, require_nonempty: bool = True) -> str | None:
    path_text = record.get("log_path") or record.get("path")
    expected_hash = record.get("log_sha256") or record.get("sha256")
    if not isinstance(path_text, str) or not path_text or Path(path_text).is_absolute():
        errors.append(_issue("evidence_path_invalid", "Evidence must use a repository-relative path", item_id))
        return None
    path = (root / path_text).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        errors.append(_issue("evidence_path_escape", "Evidence path escapes the repository", item_id))
        return None
    if not path.is_file():
        errors.append(_issue("evidence_missing", f"Evidence file does not exist: {path_text}", item_id))
        return None
    content = path.read_bytes()
    if require_nonempty and not content:
        errors.append(_issue("evidence_empty", f"Evidence file is empty: {path_text}", item_id))
    if not isinstance(expected_hash, str) or sha256(content) != expected_hash.lower():
        errors.append(_issue("evidence_hash_mismatch", f"Evidence hash does not match: {path_text}", item_id))
    return content.decode("utf-8", errors="replace")


def source_snapshot_sha256(source_files: list[dict[str, str]]) -> str:
    records = sorted(source_files, key=lambda record: record["path"].replace("\\", "/"))
    payload = b"".join(
        record["path"].replace("\\", "/").encode("utf-8") + b"\0" +
        bytes.fromhex(record["sha256"]) + b"\n"
        for record in records
    )
    return sha256(payload)


def _has_nonpassing_ctest_result(log: str) -> bool:
    # A success substring cannot override another terminal result in the same
    # retained command output. CTest counts skipped cases as a successful exit.
    patterns = (
        r"(?m)^\s*(?:\d+:\s*)?(?:[^\r\n]*\(\d+\):\s*)?FAILED:\s*$",
        r"\*{3}(?:Failed|Skipped|Not Run|Timeout|Exception)\b",
        r"(?mi)^\s*(?:\d+:\s*)?No tests were found",
        r"\b[1-9]\d* tests failed out of\b",
        r"\btests failed out of 0\b",
        r"(?m)^\s*(?:\d+:\s*)?(?:test cases|assertions):[^\r\n]*\b[1-9]\d* failed\b",
        r"(?m)^\s*(?:\d+:\s*)?FAILED\s+\((?:failures|errors)=",
    )
    return any(re.search(pattern, log) is not None for pattern in patterns)


def _validate_digest_file(root: Path, record: dict[str, Any], item_id: str, label: str,
                          errors: list[dict[str, str]]) -> bool:
    path_text = record.get("path")
    expected_hash = record.get("sha256")
    if not isinstance(path_text, str) or not path_text or Path(path_text).is_absolute():
        errors.append(_issue(f"{label}_path_invalid", f"{label} path must be repository-relative", item_id))
        return False
    path = (root / path_text).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        errors.append(_issue(f"{label}_path_escape", f"{label} path escapes the repository", item_id))
        return False
    if not path.is_file():
        errors.append(_issue(f"{label}_missing", f"{label} file does not exist: {path_text}", item_id))
        return False
    actual_hash = sha256(path.read_bytes())
    if not isinstance(expected_hash, str) or actual_hash != expected_hash.lower():
        errors.append(_issue(f"{label}_hash_mismatch", f"{label} hash does not match: {path_text}", item_id))
        return False
    return True


def _validate_command(record: dict[str, Any], phase: str, test_names: list[str], item_id: str,
                      root: Path, errors: list[dict[str, str]]) -> None:
    argv = record.get("argv")
    cwd = record.get("cwd")
    if not isinstance(argv, list) or not argv or any(not isinstance(arg, str) for arg in argv):
        errors.append(_issue("command_record_missing", f"{phase} needs its exact argument array", item_id))
        return
    if not any("ctest" in arg.lower() for arg in argv[:1]):
        errors.append(_issue("command_runner_mismatch", f"{phase} command must invoke CTest", item_id))
    if "-R" not in argv or "--test-dir" not in argv:
        errors.append(_issue("command_scope_missing", f"{phase} command must bind a build directory and test filter", item_id))
    normalized = re.sub(r"\\([.\-+()])", r"\1", " ".join(argv))
    if not all(name in normalized for name in test_names):
        errors.append(_issue("command_test_filter_mismatch", f"{phase} command does not select every mapped test", item_id))
    if not isinstance(cwd, str) or not cwd or Path(cwd).resolve() != root.resolve():
        errors.append(_issue("command_cwd_mismatch", f"{phase} command working directory is missing or incorrect", item_id))


def validate_manifest(manifest: dict[str, Any], acceptance_text: str, inventory: list[str],
                      root: Path, acceptance_content_sha256: str | None = None) -> list[dict[str, str]]:
    errors: list[dict[str, str]] = []
    expected = expected_ids()
    acceptance = parse_acceptance(acceptance_text)
    acceptance_ids = set(acceptance)
    if acceptance_ids != set(expected):
        errors.append(_issue("acceptance_document_ids", "Acceptance document does not contain the fixed 66-item set"))
    if manifest.get("schema_version") != 1:
        errors.append(_issue("manifest_schema", "Manifest schema_version must be 1"))
    if manifest.get("acceptance_document") != ACCEPTANCE_PATH:
        errors.append(_issue("acceptance_document_path", "Manifest must bind the CNL acceptance document"))
    actual_acceptance_hash = acceptance_content_sha256 or sha256(acceptance_text.encode("utf-8"))
    if manifest.get("acceptance_sha256") != actual_acceptance_hash:
        errors.append(_issue("acceptance_hash_mismatch", "Manifest acceptance hash does not match the current document"))
    items = manifest.get("items")
    if not isinstance(items, list):
        return errors + [_issue("manifest_items_invalid", "Manifest items must be an array")]
    ids = [item.get("id") for item in items if isinstance(item, dict)]
    duplicates = sorted(item_id for item_id, count in Counter(ids).items() if count > 1)
    if duplicates:
        errors.append(_issue("duplicate_id", "Duplicate acceptance IDs: " + ", ".join(duplicates)))
    if set(ids) != set(expected) or len(ids) != len(expected):
        missing = sorted(set(expected) - set(ids))
        extra = sorted(set(ids) - set(expected))
        errors.append(_issue("manifest_id_coverage", f"Manifest IDs differ from the fixed set; missing={missing}, extra={extra}"))
    if not inventory:
        errors.append(_issue("zero_test_collection", "CTest inventory contains zero tests"))

    inventory_counts = Counter(inventory)
    for item in items:
        if not isinstance(item, dict):
            errors.append(_issue("manifest_item_invalid", "Each manifest item must be an object"))
            continue
        item_id = item.get("id")
        if not isinstance(item_id, str):
            continue
        status = item.get("status")
        if status not in STATUSES:
            errors.append(_issue("status_invalid", f"Unknown status {status!r}", item_id))
            continue
        if status != "passed":
            if status in {"skipped", "failed", "timeout", "blocked"}:
                errors.append(_issue("required_item_not_passed", f"Required item status is {status}", item_id))
            continue

        source_files = item.get("source_files")
        if not isinstance(source_files, list) or not source_files:
            errors.append(_issue("source_identity_missing", "Passed items require exact source file hashes", item_id))
            source_files = []
        valid_source_files = []
        seen_source_paths: set[str] = set()
        for source_file in source_files:
            if not isinstance(source_file, dict):
                errors.append(_issue("source_record_invalid", "Source file records must be objects", item_id))
                continue
            path_text = source_file.get("path")
            if not isinstance(path_text, str):
                continue
            normalized_path = path_text.replace("\\", "/")
            if normalized_path in seen_source_paths:
                errors.append(_issue("source_record_duplicate", f"Duplicate source file: {normalized_path}", item_id))
                continue
            seen_source_paths.add(normalized_path)
            if _validate_digest_file(root, source_file, item_id, "source", errors):
                valid_source_files.append(source_file)
        expected_source_hash = item.get("source_snapshot_sha256")
        try:
            actual_source_hash = source_snapshot_sha256(valid_source_files) if valid_source_files else None
        except (KeyError, ValueError):
            actual_source_hash = None
        if not isinstance(expected_source_hash, str) or actual_source_hash != expected_source_hash.lower():
            errors.append(_issue("source_snapshot_hash_mismatch", "Passed item source snapshot hash is missing or incorrect", item_id))

        binary_artifacts = item.get("binary_artifacts")
        if not isinstance(binary_artifacts, list) or not binary_artifacts:
            errors.append(_issue("binary_identity_missing", "Passed items require at least one hashed binary", item_id))
        else:
            for binary in binary_artifacts:
                if not isinstance(binary, dict):
                    errors.append(_issue("binary_record_invalid", "Binary records must be objects", item_id))
                    continue
                _validate_digest_file(root, binary, item_id, "binary", errors)

        required_layers = acceptance.get(item_id)
        if required_layers is None:
            errors.append(_issue("acceptance_layer_missing", "No independent layer contract for this ID", item_id))
            continue
        verified_layers = set(item.get("verified_layers", []))
        if not required_layers.issubset(verified_layers):
            missing_layers = sorted(required_layers - verified_layers)
            errors.append(_issue("required_layer_missing", f"Required observation layers missing: {missing_layers}", item_id))
        if "D" in required_layers and "D" in verified_layers:
            desktop = item.get("desktop_evidence")
            if not isinstance(desktop, dict) or desktop.get("type") != "real_os_window":
                errors.append(_issue("desktop_evidence_not_real", "D-layer evidence must be a real OS window record", item_id))
            else:
                artifacts = desktop.get("artifacts", [])
                if not artifacts:
                    errors.append(_issue("desktop_evidence_missing", "D-layer evidence has no screenshot or recording", item_id))
                for artifact in artifacts:
                    _safe_artifact(root, artifact, item_id, errors, require_nonempty=True)

        test_names = item.get("test_names")
        if not isinstance(test_names, list) or not test_names or any(not isinstance(name, str) or not name for name in test_names):
            errors.append(_issue("passed_test_mapping_missing", "Passed items require exact registered test names", item_id))
            test_names = []
        for name in test_names:
            if inventory_counts[name] != 1:
                errors.append(_issue("passed_test_unregistered", f"Test must be registered exactly once: {name}", item_id))

        tdd = item.get("tdd")
        if not isinstance(tdd, dict):
            errors.append(_issue("tdd_record_missing", "Passed items require Red and Green records", item_id))
            continue
        red = tdd.get("red")
        if not isinstance(red, dict) or red.get("kind") not in RED_KINDS:
            errors.append(_issue("red_evidence_missing", "Passed items require a classified Red or prior-capability record", item_id))
        elif red.get("kind") == "behavior_red":
            _validate_command(red, "red", test_names, item_id, root, errors)
            red_log = _safe_artifact(root, red, item_id, errors)
            if red.get("exit_code") == 0 or not red_log or "FAILED:" not in red_log:
                errors.append(_issue("behavior_red_not_proven", "Behavior Red needs a nonzero run and a failed assertion", item_id))
            if not all(name in (red_log or "") for name in test_names):
                errors.append(_issue("behavior_red_test_mismatch", "Red log does not name every mapped test", item_id))
        elif red.get("kind") in {"compile_red", "sensitivity_probe"}:
            _validate_command(red, "red", test_names, item_id, root, errors)
            red_log = _safe_artifact(root, red, item_id, errors)
            if red.get("exit_code") == 0 or not red_log:
                errors.append(_issue("classified_red_not_proven", "Classified Red needs a nonzero command and retained output", item_id))
        elif red.get("kind") == "already_present":
            if not isinstance(red.get("observation"), str) or not red["observation"].strip():
                errors.append(_issue("prior_capability_record_missing", "Existing behavior needs a concrete observation", item_id))

        if isinstance(red, dict) and red.get("source_files") is not None:
            red_source_files = red.get("source_files")
            if not isinstance(red_source_files, list) or not red_source_files:
                errors.append(_issue("red_source_identity_missing", "Red source identity must contain hashed files", item_id))
                red_source_files = []
            valid_red_sources = []
            for source_file in red_source_files:
                if not isinstance(source_file, dict):
                    errors.append(_issue("red_source_record_invalid", "Red source records must be objects", item_id))
                    continue
                if _validate_digest_file(root, source_file, item_id, "source", errors):
                    valid_red_sources.append(source_file)
            try:
                red_snapshot = source_snapshot_sha256(valid_red_sources) if valid_red_sources else None
            except (KeyError, ValueError):
                red_snapshot = None
            if red_snapshot != red.get("source_snapshot_sha256"):
                errors.append(_issue("red_source_snapshot_hash_mismatch", "Red source snapshot hash is missing or incorrect", item_id))
        if isinstance(red, dict) and red.get("binary_artifacts") is not None:
            red_binaries = red.get("binary_artifacts")
            if not isinstance(red_binaries, list) or not red_binaries:
                errors.append(_issue("red_binary_identity_missing", "Red binary identity must contain hashed files", item_id))
            else:
                for binary in red_binaries:
                    if not isinstance(binary, dict):
                        errors.append(_issue("red_binary_record_invalid", "Red binary records must be objects", item_id))
                        continue
                    _validate_digest_file(root, binary, item_id, "binary", errors)

        green = tdd.get("green")
        if not isinstance(green, dict) or green.get("kind") != "behavior_green":
            errors.append(_issue("green_evidence_missing", "Passed items require a behavior Green record", item_id))
            continue
        _validate_command(green, "green", test_names, item_id, root, errors)
        green_log = _safe_artifact(root, green, item_id, errors)
        if green.get("exit_code") != 0:
            errors.append(_issue("green_command_nonzero", "Green command must exit zero", item_id))
        if not green_log or "100% tests passed" not in green_log:
            errors.append(_issue("green_result_missing", "Green log must show a successful nonzero CTest collection", item_id))
        if green_log and _has_nonpassing_ctest_result(green_log):
            errors.append(_issue("green_log_not_passed", "Green log contains failed, skipped, or zero-test results", item_id))
        if not all(name in (green_log or "") for name in test_names):
            errors.append(_issue("green_test_mismatch", "Green log does not name every mapped test", item_id))

        regression = item.get("direct_regression")
        if not isinstance(regression, dict):
            errors.append(_issue("direct_regression_missing", "Passed items require a direct regression record", item_id))
            continue
        regression_names = regression.get("test_names")
        if not isinstance(regression_names, list) or not regression_names or any(
                not isinstance(name, str) or not name for name in regression_names):
            errors.append(_issue("direct_regression_mapping_missing", "Direct regression requires exact test names", item_id))
            regression_names = []
        for name in regression_names:
            if inventory_counts[name] != 1:
                errors.append(_issue("direct_regression_unregistered", f"Regression test must be registered exactly once: {name}", item_id))
        _validate_command(regression, "regression", regression_names, item_id, root, errors)
        regression_log = _safe_artifact(root, regression, item_id, errors)
        if regression.get("exit_code") != 0:
            errors.append(_issue("direct_regression_nonzero", "Direct regression must exit zero", item_id))
        if not regression_log or "100% tests passed" not in regression_log:
            errors.append(_issue("direct_regression_result_missing", "Direct regression log must show all tests passed", item_id))
        if regression_log and _has_nonpassing_ctest_result(regression_log):
            errors.append(_issue("regression_log_not_passed", "Regression log contains failed, skipped, or zero-test results", item_id))
        if not all(name in (regression_log or "") for name in regression_names):
            errors.append(_issue("direct_regression_test_mismatch", "Regression log does not name every mapped test", item_id))

    return errors


def evaluate(manifest: dict[str, Any], acceptance_text: str, inventory: list[str],
             root: Path, acceptance_content_sha256: str | None = None) -> dict[str, Any]:
    errors = validate_manifest(manifest, acceptance_text, inventory, root, acceptance_content_sha256)
    counts = Counter(item.get("status", "invalid") for item in manifest.get("items", [])
                     if isinstance(item, dict))
    all_passed = len(manifest.get("items", [])) == 66 and counts["passed"] == 66
    fatal_states = counts["failed"] + counts["blocked"] + counts["skipped"] + counts["timeout"]
    status = "failed" if errors or fatal_states else "passed" if all_passed else "incomplete"
    return {"schema_version": 1, "status": status,
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "acceptance_document": ACCEPTANCE_PATH,
            "acceptance_sha256": acceptance_content_sha256 or sha256(acceptance_text.encode("utf-8")),
            "required_count": 66, "registered_test_count": len(inventory),
            "status_counts": {key: counts.get(key, 0) for key in sorted(STATUSES)},
            "passed_count": counts.get("passed", 0), "issues": errors,
            "items": manifest.get("items", [])}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=Path("build/windows-debug"))
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--manifest", type=Path, default=Path("tests/connection_library_manifest.json"))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    manifest_path = args.manifest if args.manifest.is_absolute() else root / args.manifest
    acceptance_path = root / ACCEPTANCE_PATH
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    acceptance_bytes = acceptance_path.read_bytes()
    acceptance_text = acceptance_bytes.decode("utf-8")
    inventory, inventory_command = collect_inventory(args.ctest, build_dir, root)
    report = evaluate(manifest, acceptance_text, inventory, root, sha256(acceptance_bytes))
    report["inventory_command"] = {key: value for key, value in inventory_command.items() if key != "output"}
    report["inventory_sha256"] = sha256(inventory_command["output"].encode("utf-8"))
    report["manifest_sha256"] = sha256(manifest_path.read_bytes())
    if inventory_command["exit_code"] != 0:
        report["status"] = "failed"
        report["issues"].append(_issue("inventory_command_failed", "CTest inventory command exited nonzero"))
    if args.report:
        report_path = args.report if args.report.is_absolute() else root / args.report
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "required_count": report["required_count"],
                      "registered_test_count": report["registered_test_count"],
                      "status_counts": report["status_counts"], "issue_count": len(report["issues"]),
                      "report": str(args.report) if args.report else None}, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
