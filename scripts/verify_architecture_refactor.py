"""Fail-closed AR stage inventory and CTest/JUnit evidence verifier."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests" / "architecture_refactor_manifest.json"
ACCEPTANCE = ROOT / "doc" / "testing_architecture_refactor.md"
ALLOWED = {"passed", "failed", "not_run", "not_verified", "blocked", "skipped", "timeout"}


def validate_upn_manifest(manifest: dict[str, Any]) -> list[str]:
    """UPN supplements, never replaces or changes the 110 AR cases."""
    block = manifest.get("user_native_plugins", {})
    rows = block.get("requirements", [])
    errors = []
    if {r.get("id") for r in rows} != {f"UPN-{i:02}" for i in range(1, 7)} or len(rows) != 6:
        errors.append("UPN requires exactly UPN-01..06")
    source = ROOT / "doc/plan_delivery_convergence.md"
    if block.get("acceptance_source") != "doc/plan_delivery_convergence.md" or block.get("acceptance_document_sha256") != sha(source):
        errors.append("UPN acceptance document binding mismatch")
    for row in rows:
        if row.get("stage") != "R6" or row.get("required") is not True or not row.get("test_names") or not row.get("evidence_requirements"):
            errors.append(f"invalid UPN mapping: {row.get('id')}")
        if not isinstance(row.get("test_names"), list) or not all(isinstance(n, str) and n.strip() for n in row.get("test_names", [])):
            errors.append(f"invalid UPN test names: {row.get('id')}")
    return errors


def evaluate_upn_cases(manifest: dict[str, Any], registered: set[str], executed: dict[str, str], stage: str,
                       evidence: dict[str, Any] | None = None, current_source_hash: str | None = None) -> list[dict[str, str]]:
    if stage != "R6":
        return []
    rows = []
    for item in manifest.get("user_native_plugins", {}).get("requirements", []):
        names = item.get("test_names", [])
        state, reason = "passed", "all mapped native SDK/DLL tests executed and passed"
        missing = [n for n in names if n not in registered or n not in executed]
        failures = [executed[n] for n in names if n in executed and executed[n] != "passed"]
        if not names or missing:
            state, reason = "not_run", "missing UPN registration/execution: " + ", ".join(missing)
        elif failures:
            state, reason = failures[0], "UPN mapped test did not pass"
        elif item.get("desktop_required"):
            error = verify_desktop_record((evidence or {}).get(item["id"]), source_tree_hash=current_source_hash)
            if error:
                state, reason = "not_verified", error
        elif item.get("offline_installation_required"):
            # No OS network-isolation/clean-user acceptance importer exists yet. A
            # proxy-only development run must never satisfy the formal requirement.
            state, reason = "not_verified", "formal clean/offline installation evidence has not been collected"
        rows.append({"id": item["id"], "stage": "R6", "status": state, "reason": reason})
    return rows


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def acceptance_ids(document: str) -> set[str]:
    return set(acceptance_stage_map(document))


def acceptance_stage_map(document: str) -> dict[str, str]:
    """Exact ID -> required stage mapping taken from the versioned acceptance table."""
    mapping: dict[str, str] = {}
    for line in document.splitlines():
        if not line.startswith("| R"):
            continue
        cells = [part.strip() for part in line.strip("|").split("|")]
        if len(cells) < 2 or not re.fullmatch(r"R[0-6]", cells[0]):
            continue
        stage = cells[0]
        for number, first, last in re.findall(r"AR-(\d+)\.([a-z])～([a-z])", cells[1]):
            for code in range(ord(first), ord(last) + 1):
                mapping[f"AR-{number}.{chr(code)}"] = stage
        for number, letter in re.findall(r"AR-(\d+)\.([a-z])(?=、|$)", cells[1]):
            mapping.setdefault(f"AR-{number}.{letter}", stage)
    return mapping


def validate_manifest(manifest: dict[str, Any], document: str) -> list[str]:
    errors: list[str] = []
    requirements = manifest.get("requirements")
    if not isinstance(requirements, list):
        return ["requirements must be an array"]
    ids = [item.get("id") for item in requirements if isinstance(item, dict)]
    required = acceptance_ids(document)
    stage_by_id = acceptance_stage_map(document)
    if set(ids) != required or len(ids) != len(required):
        errors.append(f"required case coverage mismatch: expected {len(required)}, found {len(ids)}")
    if manifest.get("acceptance_document_sha256") != hashlib.sha256(document.encode("utf-8")).hexdigest():
        errors.append("acceptance document hash mismatch")
    stages = manifest.get("stages", [])
    if list(stages) != [f"R{i}" for i in range(7)]:
        errors.append("manifest stages must list R0..R6 in order")
    stage_set = set(stages)
    test_owners: dict[str, set[str]] = {}
    for item in requirements:
        if not isinstance(item, dict):
            errors.append("malformed requirement entry")
            continue
        if item.get("stage") not in stage_set or item.get("required") is not True:
            errors.append(f"invalid stage/required flag for {item.get('id')}")
        documented = stage_by_id.get(item.get("id"))
        if documented is not None and documented != item.get("stage"):
            errors.append(f"stage for {item.get('id')} must be {documented} per the acceptance table, "
                          f"manifest says {item.get('stage')}")
        if not isinstance(item.get("test_names"), list) or not isinstance(item.get("evidence_requirements"), list):
            errors.append(f"test/evidence arrays missing for {item.get('id')}")
        if not item.get("layer") or not item.get("runner"):
            errors.append(f"layer/runner missing for {item.get('id')}")
        names = item.get("test_names", [])
        if isinstance(names, list):
            valid_names = [name for name in names if isinstance(name, str) and name.strip()]
            if len(valid_names) != len(names) or len(valid_names) != len(set(valid_names)):
                errors.append(f"invalid or repeated test name within {item.get('id')}")
            for name in valid_names:
                test_owners.setdefault(name, set()).add(item.get("id"))
    # Acceptance permits sharing an executed fact, while every AR row keeps its own
    # pass/desktop/evidence decision. Require exact owners and an explicit rationale.
    shared = manifest.get("shared_test_facts", [])
    declared: set[str] = set()
    if not isinstance(shared, list):
        errors.append("shared_test_facts must be an array")
        shared = []
    for fact in shared:
        if not isinstance(fact, dict):
            errors.append("malformed shared test fact")
            continue
        name, owners, reason = fact.get("test_name"), fact.get("requirement_ids"), fact.get("fact")
        if not isinstance(name, str) or not name.strip() or name in declared:
            errors.append("invalid or repeated shared test declaration")
            continue
        declared.add(name)
        if (not isinstance(owners, list) or not all(isinstance(owner, str) for owner in owners)
                or len(owners) < 2 or len(set(owners)) != len(owners)
                or set(owners) != test_owners.get(name, set())
                or not isinstance(reason, str) or not reason.strip()):
            errors.append(f"shared fact must identify all exact owners and a reason: {name}")
    if declared != {name for name, owners in test_owners.items() if len(owners) > 1}:
        errors.append("shared test declarations do not match repeated mappings")
    documented_counts = {stage: sum(1 for value in stage_by_id.values() if value == stage) for stage in stages}
    manifest_counts = {stage: sum(1 for item in requirements if isinstance(item, dict) and item.get("stage") == stage)
                       for stage in stages}
    if documented_counts != manifest_counts:
        errors.append(f"per-stage required counts differ from the acceptance table: "
                      f"document={documented_counts}, manifest={manifest_counts}")
    return errors


def junit_results(path: Path | None) -> dict[str, str]:
    if path is None:
        return {}
    root = ET.parse(path).getroot()
    results: dict[str, str] = {}
    for case in root.iter("testcase"):
        name = case.attrib.get("name", "")
        failure = case.find("./failure")
        if failure is None:
            failure = case.find("./error")
        if failure is not None:
            state = "timeout" if "timeout" in (failure.attrib.get("message", "") + (failure.text or "")).lower() else "failed"
        elif case.find("./skipped") is not None:
            state = "skipped"
        else:
            state = "passed"
        results[name] = state
    return results


def evaluate_cases(manifest: dict[str, Any], registered: set[str], executed: dict[str, str], stage: str,
                   pure: bool = False, evidence: dict[str, Any] | None = None,
                   current_source_hash: str | None = None) -> list[dict[str, str]]:
    order = manifest["stages"]
    limit = order.index(stage)
    rows = []
    for item in manifest["requirements"]:
        if not item.get("required") or order.index(item["stage"]) > limit:
            continue
        if pure and item.get("layer") not in {"pure", "infrastructure"}:
            continue
        names = item.get("test_names", [])
        if item.get("runner") == "gate_failure_probe":
            if any(name not in registered for name in names):
                state, reason = "not_run", "required probe-validator test is missing from current CTest inventory"
            elif any(name not in executed for name in names):
                state, reason = "not_run", "required probe-validator test has no result in the supplied JUnit report"
            elif any(executed[name] != "passed" for name in names):
                state, reason = "failed", "probe evidence validator test did not pass"
            else:
                reason = verify_gate_probe_record((evidence or {}).get(item["id"]), source_tree_hash=current_source_hash)
                state = "passed" if reason is None else "not_verified"
                if reason is None:
                    reason = "isolated assertion failure reached G0 and clean inventory passed"
        elif item.get("runner") == "unassigned" or not names:
            state, reason = "not_run", "no test runner/name is implemented in the manifest"
        elif any(name not in registered for name in names):
            state, reason = "not_run", "required test is missing from current CTest inventory"
        elif any(name not in executed for name in names):
            state, reason = "not_run", "required test has no result in the supplied JUnit report"
        else:
            states = [executed[name] for name in names]
            nonpassing = next((value for value in ("failed", "skipped", "timeout") if value in states), None)
            if nonpassing is not None:
                state, reason = nonpassing, "mapped test did not pass"
            elif item.get("remaining_contracts"):
                state, reason = "not_verified", "unimplemented/unverified contract: " + "; ".join(item["remaining_contracts"])
            elif item.get("desktop_required"):
                record = (evidence or {}).get(item["id"])
                failure = verify_desktop_record(record, source_tree_hash=current_source_hash)
                if failure is None:
                    state, reason = "passed", "current-source desktop artifact verified"
                else:
                    state, reason = "not_verified", failure
            else:
                state, reason = "passed", "all mapped tests executed and passed"
        rows.append({"id": item["id"], "stage": item["stage"], "status": state, "reason": reason})
    return rows


def inventory(build_dir: Path, config: str) -> tuple[list[dict[str, Any]], list[str], str, str]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1", "--no-tests=error"]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    if result.returncode:
        raise RuntimeError(f"CTest inventory failed ({result.returncode}): {result.stderr}")
    data = json.loads(result.stdout)
    return data.get("tests", []), command, result.stdout, result.stderr


def validate_run_records(ctest_record_path: Path | None, build_record_path: Path | None,
                         build_dir: Path, config: str, junit_path: Path | None,
                         inventory_rows: list[dict[str, Any]], sources: dict[str, str]) -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    attachments: dict[str, Any] = {}
    current_hash = hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()
    ctest_record = None
    build_record = None
    for label, path in (("ctest", ctest_record_path), ("build", build_record_path)):
        if path is None:
            errors.append(f"{label} command record is required")
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{label} command record is unreadable: {exc}")
            continue
        attachments[f"{label}_record"] = {"path": str(path.resolve()), "sha256": sha(path)}
        if data.get("exit_code") != 0:
            errors.append(f"{label} command did not exit zero")
        if data.get("cwd") != str(ROOT):
            errors.append(f"{label} command cwd does not match workspace")
        if data.get("sources") != sources or data.get("source_tree_hash") != current_hash:
            errors.append(f"{label} command source manifest is stale")
        log_path = Path(data.get("log_path", ""))
        if not log_path.is_file() or sha(log_path) != data.get("log_sha256"):
            errors.append(f"{label} command log is missing or hash-mismatched")
        if label == "ctest":
            ctest_record = data
        else:
            build_record = data
    if ctest_record is not None:
        command = ctest_record.get("command", [])
        normalized = [str(value) for value in command]
        try:
            recorded_build = Path(normalized[normalized.index("--test-dir") + 1]).resolve()
            recorded_config = normalized[normalized.index("-C") + 1]
            recorded_junit = Path(normalized[normalized.index("--output-junit") + 1]).resolve()
        except (ValueError, IndexError):
            errors.append("CTest command record lacks build/config/JUnit arguments")
        else:
            if recorded_build != build_dir.resolve() or recorded_config != config:
                errors.append("CTest command record uses a wrong build directory or configuration")
            if junit_path is None or recorded_junit != junit_path.resolve():
                errors.append("CTest command record does not identify the supplied JUnit artifact")
            expected_junit_hash = sha(junit_path.resolve()) if junit_path and junit_path.is_file() else None
            if not any(Path(item.get("path", "")).resolve() == (junit_path.resolve() if junit_path else Path())
                       and item.get("sha256") == expected_junit_hash for item in ctest_record.get("artifacts", [])):
                errors.append("CTest command record does not hash the supplied JUnit artifact")
    if build_record is not None:
        command = [str(value) for value in build_record.get("command", [])]
        expected_preset = "windows-debug" if "debug" in build_dir.name else "windows-headless" if "headless" in build_dir.name else None
        if "--build" not in command or expected_preset is None or expected_preset not in command:
            errors.append("build command record does not match this build configuration")
        artifacts = {Path(item.get("path", "")).resolve(): item.get("sha256")
                     for item in build_record.get("artifacts", []) if item.get("status") != "missing"}
        required_basenames = {"audio_cli.exe", "audioalgo_tests.exe", "audio_application_tests.exe"}
        if expected_preset == "windows-debug":
            required_basenames.add("AudioAlgo Editor.exe")
        for binary in required_basenames:
            candidates = [path for path in artifacts if path.name == binary and path.is_file()]
            if len(candidates) != 1 or sha(candidates[0]) != artifacts.get(candidates[0]):
                errors.append(f"build record lacks a current hashed binary: {binary}")
        if expected_preset:
            for row in inventory_rows:
                argv = row.get("command", [])
                if not argv:
                    errors.append(f"CTest inventory entry has no command: {row.get('name')}")
                    continue
                executable = Path(argv[0])
                try:
                    executable.resolve().relative_to(build_dir.resolve())
                except ValueError:
                    continue
                if executable.resolve() not in artifacts:
                    errors.append(f"CTest executable is absent from build artifact hashes: {executable}")
    return errors, attachments


def source_manifest() -> dict[str, str]:
    excluded = {"build", ".git", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
    output = {}
    paths = []
    for folder, directories, names in os.walk(ROOT, followlinks=False):
        directories[:] = [name for name in directories if name not in excluded]
        paths.extend(Path(folder) / name for name in names if name not in excluded)
    for path in sorted(paths):
        relative = path.relative_to(ROOT)
        if path.is_file() and not excluded.intersection(relative.parts):
            output[relative.as_posix()] = sha(path)
    return output


def verify_evidence_record(record: Any, root: Path = ROOT, source_tree_hash: str | None = None,
                          required_fields: set[str] | None = None) -> str | None:
    """Validate artifact bytes/provenance; a self-reported status string proves nothing."""
    if not isinstance(record, dict):
        return "evidence record missing"
    required = {"path", "sha256"} | (required_fields or set())
    missing = sorted(required - set(record))
    if missing:
        return "evidence fields missing: " + ", ".join(missing)
    path = (root / str(record["path"])).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        return "evidence path escapes the workspace"
    if not path.is_file():
        return "evidence file missing or expired"
    if sha(path).lower() != str(record["sha256"]).lower():
        return "evidence file hash mismatch"
    if source_tree_hash is not None and record.get("source_tree_hash") != source_tree_hash:
        return "evidence source hash is stale"
    return None


def verify_gate_probe_record(record: Any, root: Path = ROOT, source_tree_hash: str | None = None) -> str | None:
    error = verify_evidence_record(record, root, source_tree_hash, {"source_tree_hash"})
    if error:
        return error
    proof_path = (root / str(record["path"])).resolve()
    try:
        proof = json.loads(proof_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return f"failure-probe evidence is unreadable: {exc}"
    if proof.get("status") != "red_observed" or proof.get("gate_status") != "failed":
        return "failure-probe evidence does not show a propagated red Gate"
    if proof.get("exit_code", 0) == 0 or proof.get("gate_exit_code", 0) == 0 or proof.get("gate_ctest_exit_code", 0) == 0:
        return "failure-probe and Gate exits must be nonzero"
    if not proof.get("probe_named_in_gate_failure") or not proof.get("expected_nonzero"):
        return "Gate failure is not tied to the intentional assertion probe"
    if proof.get("configure_off_exit_code") != 0 or not proof.get("probe_absent_after_restore"):
        return "normal inventory was not restored without the probe"
    if proof.get("clean_gate_exit_code") != 0 or proof.get("clean_gate_status") != "passed":
        return "restored clean Gate did not pass"
    for path_key, hash_key, expected_status in (
        ("gate_report_path", "gate_report_sha256", "failed"),
        ("clean_gate_report_path", "clean_gate_report_sha256", "passed"),
    ):
        try:
            report_path = Path(proof[path_key]).resolve()
            report_path.relative_to(root.resolve())
            if not report_path.is_file() or sha(report_path).lower() != str(proof[hash_key]).lower():
                return f"referenced Gate report missing or hash mismatch: {path_key}"
            gate_report = json.loads(report_path.read_text(encoding="utf-8"))
        except (KeyError, OSError, ValueError, json.JSONDecodeError) as exc:
            return f"referenced Gate report invalid: {path_key}: {exc}"
        if gate_report.get("status") != expected_status:
            return f"referenced Gate report has unexpected status: {path_key}"
    return None


def desktop_fields() -> set[str]:
    return {"operation", "os", "display_scale_percent", "physical_client_size", "logical_client_size",
            "executable_path", "executable_sha256", "source_tree_hash", "timestamp_utc",
            "screenshot_path", "screenshot_sha256"}


def verify_desktop_record(record: Any, source_tree_hash: str | None = None, root: Path = ROOT) -> str | None:
    """A desktop claim must ship real, hash-matched screenshot and executable artifacts."""
    error = verify_evidence_record(record, root, source_tree_hash, desktop_fields())
    if error is not None:
        return error
    for field in ("operation", "os", "timestamp_utc", "physical_client_size", "logical_client_size",
                  "screenshot_path", "screenshot_sha256", "executable_path", "executable_sha256"):
        if not str(record.get(field, "")).strip():
            return f"desktop evidence field is empty: {field}"
    try:
        float(str(record.get("display_scale_percent")))
    except (TypeError, ValueError):
        return "desktop evidence display_scale_percent must be numeric"
    for label, path_key, hash_key in (("screenshot", "screenshot_path", "screenshot_sha256"),
                                      ("executable", "executable_path", "executable_sha256")):
        try:
            artifact = (root / str(record[path_key])).resolve()
            artifact.relative_to(root.resolve())
        except (KeyError, ValueError):
            return f"desktop {label} path escapes or is missing: {record.get(path_key)}"
        if not artifact.is_file():
            return f"desktop {label} file missing or expired: {record[path_key]}"
        if sha(artifact).lower() != str(record[hash_key]).lower():
            return f"desktop {label} hash mismatch: {record[path_key]}"
    return None


def validate_report(report: Any) -> list[str]:
    if not isinstance(report, dict):
        return ["report must be an object"]
    errors = []
    for field in ("schema_version", "stage", "timestamp_utc", "source_tree_hash", "commands", "artifacts", "cases"):
        if field not in report:
            errors.append(f"report field missing: {field}")
    commands = report.get("commands")
    command_fields = {"argv", "cwd", "started_utc", "ended_utc", "exit_code", "log_path", "log_sha256"}
    if not isinstance(commands, list) or not commands or any(
        not isinstance(command, dict) or not command_fields.issubset(command)
        or not isinstance(command.get("argv"), list) or not command.get("argv")
        or not isinstance(command.get("exit_code"), int)
        or not re.fullmatch(r"[0-9a-f]{64}", str(command.get("log_sha256", "")))
        for command in commands
    ):
        errors.append("report must preserve complete argv/cwd/UTC/exit/log-hash command records")
    if not isinstance(report.get("artifacts"), list) or any(
        not isinstance(artifact, dict) or not {"path", "sha256"}.issubset(artifact)
        or not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("sha256", "")))
        for artifact in report.get("artifacts", [])
    ):
        errors.append("report has an incomplete artifact hash record")
    if not isinstance(report.get("cases"), list) or any(
        not isinstance(case, dict) or not {"id", "status", "reason"}.issubset(case)
        or case.get("status") not in ALLOWED for case in report.get("cases", [])
    ):
        errors.append("report has an incomplete or invalid per-case result")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True, choices=[f"R{i}" for i in range(7)])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--junit", type=Path)
    parser.add_argument("--ctest-record", type=Path, help="record_command.py JSON for the exact CTest/JUnit run")
    parser.add_argument("--build-record", type=Path, help="record_command.py JSON with current test executable hashes")
    parser.add_argument("--pure", action="store_true")
    parser.add_argument("--evidence", type=Path, help="JSON map from required case ID to artifact record")
    args = parser.parse_args()
    invocation_started = dt.datetime.now(dt.timezone.utc)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    document = ACCEPTANCE.read_text(encoding="utf-8")
    errors = validate_manifest(manifest, document) + validate_upn_manifest(manifest)
    inventory_started = dt.datetime.now(dt.timezone.utc)
    inventory_rows, inventory_command, inventory_stdout, inventory_stderr = inventory(args.build_dir.resolve(), args.config)
    inventory_ended = dt.datetime.now(dt.timezone.utc)
    registered = {item["name"] for item in inventory_rows}
    executed = junit_results(args.junit.resolve() if args.junit else None)
    sources = source_manifest()
    current_source_hash = hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()
    runner_errors, attached_records = validate_run_records(
        args.ctest_record.resolve() if args.ctest_record else None,
        args.build_record.resolve() if args.build_record else None,
        args.build_dir.resolve(), args.config, args.junit.resolve() if args.junit else None,
        inventory_rows, sources)
    errors.extend(runner_errors)
    if set(executed) != registered:
        errors.append(f"JUnit/inventory test count mismatch: inventory={len(registered)}, executed={len(executed)}")
    evidence = json.loads(args.evidence.read_text(encoding="utf-8")) if args.evidence else {}
    cases = evaluate_cases(manifest, registered, executed, args.stage, args.pure, evidence, current_source_hash)
    cases.extend(evaluate_upn_cases(manifest, registered, executed, args.stage, evidence, current_source_hash))
    if runner_errors or set(executed) != registered:
        suite_error = "; ".join(runner_errors) if runner_errors else "JUnit did not execute the complete registered inventory"
        for row in cases:
            if row["status"] == "passed":
                row["status"] = "not_verified"
                row["reason"] = "actual test-run evidence is invalid: " + suite_error
    status_counts = {state: sum(row["status"] == state for row in cases) for state in sorted(ALLOWED)}
    full_count = sum(case.get("required") and manifest["stages"].index(case["stage"]) <= manifest["stages"].index(args.stage)
                     for case in manifest["requirements"])
    if args.stage == "R6":
        full_count += len(manifest.get("user_native_plugins", {}).get("requirements", []))
    report_status = "passed" if not errors and not args.pure and len(cases) == full_count and all(row["status"] == "passed" for row in cases) else "failed"
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report_dir = ROOT / "build" / "reports" / "architecture_refactor" / timestamp
    report_dir.mkdir(parents=True, exist_ok=False)
    inventory_log = report_dir / "inventory.log"
    inventory_log.write_text(inventory_stdout + inventory_stderr, encoding="utf-8")
    ctest_record_data = json.loads(args.ctest_record.read_text(encoding="utf-8")) if args.ctest_record else None
    build_record_data = json.loads(args.build_record.read_text(encoding="utf-8")) if args.build_record else None
    command_records = [{"argv": inventory_command, "cwd": str(ROOT),
                        "started_utc": inventory_started.isoformat(), "ended_utc": inventory_ended.isoformat(),
                        "exit_code": 0, "log_path": str(inventory_log.resolve()), "log_sha256": sha(inventory_log)}]
    for path_value, data in ((args.ctest_record, ctest_record_data), (args.build_record, build_record_data)):
        if data is not None:
            command_records.append({"argv": data.get("command", []), "cwd": data.get("cwd"),
                                    "started_utc": data.get("started_utc"), "ended_utc": data.get("finished_utc"),
                                    "exit_code": data.get("exit_code"), "log_path": data.get("log_path"),
                                    "log_sha256": data.get("log_sha256"), "record_path": str(path_value.resolve()),
                                    "record_sha256": sha(path_value)})
    summary_payload = {"status": report_status, "counts": status_counts, "manifest_errors": errors}
    verifier_log = report_dir / "verifier.log"
    verifier_log.write_text(json.dumps(summary_payload, ensure_ascii=False) + "\n", encoding="utf-8")
    command_records.append({"argv": [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
                            "cwd": str(ROOT), "started_utc": invocation_started.isoformat(),
                            "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                            "exit_code": 0 if report_status == "passed" else 1,
                            "log_path": str(verifier_log.resolve()), "log_sha256": sha(verifier_log)})
    report = {
        "schema_version": 1, "stage": args.stage, "scope": "development_subset" if args.pure else "cumulative_stage",
        "status": report_status, "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "cwd": str(ROOT), "config": args.config, "build_dir": str(args.build_dir.resolve()),
        "acceptance_sha256": sha(ACCEPTANCE), "manifest_sha256": sha(MANIFEST),
        "source_manifest": sources,
        "source_tree_hash": hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest(),
        "commands": command_records,
        "junit": {"path": str(args.junit.resolve()), "sha256": sha(args.junit.resolve())} if args.junit and args.junit.is_file() else None,
        "artifacts": ([{"path": str(args.junit.resolve()), "sha256": sha(args.junit.resolve())}]
                      if args.junit and args.junit.is_file() else [])
                     + [{"path": str(inventory_log.resolve()), "sha256": sha(inventory_log)},
                        {"path": str(verifier_log.resolve()), "sha256": sha(verifier_log)}],
        "run_records": attached_records,
        "manifest_errors": errors, "registered_test_count": len(registered), "cases": cases,
        "counts": status_counts,
        "limitations": ["--pure is a development subset and never passes the formal stage"] if args.pure else [],
    }
    path = report_dir / "report.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report_status, "counts": status_counts, "manifest_errors": errors, "report": str(path)}, ensure_ascii=False))
    return 0 if report_status == "passed" else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError, ET.ParseError, RuntimeError) as exc:
        print(f"architecture verification error: {exc}", file=sys.stderr)
        sys.exit(2)
