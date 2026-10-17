"""Run the NI0000-NI0006 interaction acceptance inventory fail-closed."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "editor_interaction_manifest.json"
REQUIRED_SUBCASES = {
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
ALLOWED_STATUSES = {"passed", "failed", "not_run", "not_verified", "blocked", "skipped", "timeout"}
DESKTOP_COMMON_FIELDS = {
    "operation",
    "os",
    "display_scale_percent",
    "physical_client_size",
    "logical_client_size",
    "executable_path",
    "executable_sha256",
    "source_tree_hash",
    "timestamp_utc",
    "screenshot_path",
    "screenshot_sha256",
}


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_tree_hash() -> str:
    digest = hashlib.sha256()
    for path in sorted(ROOT.rglob("*")):
        parts = path.relative_to(ROOT).parts
        if not path.is_file() or any(part in {"build", ".git", "__pycache__", ".venv"} for part in parts):
            continue
        relative = path.relative_to(ROOT).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def load_manifest(path: Path = DEFAULT_MANIFEST) -> dict[str, Any]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    expanded = dict(manifest)
    requirements: list[dict[str, Any]] = []
    pattern = manifest.get("test_name_pattern", "{requirement_id}.{subcase_id}")
    for requirement in manifest.get("requirements", []):
        item = dict(requirement)
        desktop_ids = set(item.get("desktop_subcases", []))
        evidence_tags = item.get("evidence_tags", {})
        subcases: list[dict[str, Any]] = []
        for raw_case in item.get("subcases", []):
            case = dict(raw_case) if isinstance(raw_case, dict) else {"id": raw_case}
            case_id = str(case.get("id", ""))
            full_id = f"{item.get('id', '')}.{case_id}"
            case["full_id"] = full_id
            case.setdefault("test_name", pattern.format(requirement_id=item.get("id", ""), subcase_id=case_id))
            case.setdefault("runner", "ctest")
            case.setdefault("desktop_required", case_id in desktop_ids)
            case.setdefault("evidence_tags", evidence_tags.get(case_id, []))
            subcases.append(case)
        item["subcases"] = subcases
        requirements.append(item)
    expanded["requirements"] = requirements
    expanded["manifest_path"] = str(path.resolve())
    return expanded


def validate_manifest(manifest: dict[str, Any]) -> list[dict[str, str]]:
    issues: list[dict[str, str]] = []
    expected_ids = set(REQUIRED_SUBCASES)
    requirements = manifest.get("requirements")
    if not isinstance(requirements, list):
        return [{"code": "manifest_invalid", "detail": "requirements must be an array"}]

    actual_ids = [item.get("id") for item in requirements if isinstance(item, dict)]
    if set(actual_ids) != expected_ids or len(actual_ids) != len(expected_ids):
        issues.append({"code": "manifest_id_coverage", "detail": "manifest must contain each NED-01 through NED-14 exactly once"})

    all_test_names: list[str] = []
    for requirement in requirements:
        if not isinstance(requirement, dict):
            issues.append({"code": "manifest_invalid", "detail": "each requirement must be an object"})
            continue
        requirement_id = requirement.get("id", "")
        expected_subcases = REQUIRED_SUBCASES.get(requirement_id)
        subcases = requirement.get("subcases", [])
        subcase_ids = [case.get("id") for case in subcases if isinstance(case, dict)]
        if expected_subcases is None or set(subcase_ids) != expected_subcases or len(subcase_ids) != len(expected_subcases):
            issues.append({"code": "manifest_subcase_coverage", "detail": f"{requirement_id} must list all required subcases"})
        desktop_ids = {case.get("id") for case in subcases if isinstance(case, dict) and case.get("desktop_required")}
        declared_desktop_ids = set(requirement.get("desktop_subcases", []))
        if desktop_ids != declared_desktop_ids:
            issues.append({"code": "manifest_desktop_coverage", "detail": f"{requirement_id} desktop subcases do not match the declaration"})
        for case in subcases:
            if not isinstance(case, dict):
                issues.append({"code": "manifest_invalid", "detail": f"{requirement_id} has a malformed subcase"})
                continue
            test_name = case.get("test_name")
            if not isinstance(test_name, str) or not test_name.strip():
                issues.append({"code": "manifest_test_name_missing", "detail": f"{requirement_id}.{case.get('id')} has no test registration name"})
            else:
                all_test_names.append(test_name)
            if case.get("runner") != "ctest":
                issues.append({"code": "manifest_runner_invalid", "detail": f"{requirement_id}.{case.get('id')} must use a registered CTest runner"})

    if len(all_test_names) != len(set(all_test_names)):
        issues.append({"code": "manifest_test_name_duplicate", "detail": "CTest registration names must be unique per required subcase"})
    acceptance_path = ROOT / str(manifest.get("acceptance_source", ""))
    expected_doc_hash = manifest.get("acceptance_document_sha256")
    if not acceptance_path.is_file() or not isinstance(expected_doc_hash, str):
        issues.append({"code": "manifest_acceptance_source_missing", "detail": "manifest does not identify the acceptance source and its hash"})
    elif _sha256_file(acceptance_path).lower() != expected_doc_hash.lower():
        issues.append({"code": "manifest_acceptance_source_changed", "detail": "acceptance document hash differs from the versioned manifest"})
    declared_fields = set(manifest.get("desktop_evidence_required_fields", []))
    if not DESKTOP_COMMON_FIELDS.issubset(declared_fields):
        issues.append({"code": "manifest_desktop_fields_missing", "detail": "desktop evidence schema omits required environment or screenshot fields"})
    return issues


def _load_evidence_record(record: dict[str, Any], full_id: str, evidence: dict[str, Any], current_hash: str, base_dir: Path) -> str | None:
    required = set(DESKTOP_COMMON_FIELDS) | set(record.get("evidence_tags", []))
    missing = sorted(field for field in required if record.get(field) in (None, "", []))
    if missing:
        return "missing required fields: " + ", ".join(missing)
    if record.get("source_tree_hash") != current_hash:
        return "evidence source_tree_hash does not match the current source"
    try:
        timestamp = dt.datetime.fromisoformat(str(record["timestamp_utc"]).replace("Z", "+00:00"))
    except ValueError:
        return "timestamp_utc is not an ISO-8601 timestamp"
    if timestamp.tzinfo is None:
        return "timestamp_utc must include a timezone"
    if record.get("display_scale_percent") not in {100, 150, 200}:
        return "display_scale_percent must be one of the required DPI scales (100, 150, 200)"
    for size_field in ("physical_client_size", "logical_client_size"):
        size = record.get(size_field)
        if not isinstance(size, list) or len(size) != 2 or any(not isinstance(value, int) or value <= 0 for value in size):
            return f"{size_field} must contain positive width and height"
    screenshot = Path(str(record["screenshot_path"]))
    if not screenshot.is_absolute():
        screenshot = base_dir / screenshot
    if not screenshot.is_file():
        return "screenshot file does not exist"
    screenshot_bytes = screenshot.read_bytes()
    if not screenshot_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
        return "screenshot is not a PNG image"
    if _sha256_bytes(screenshot_bytes) != str(record["screenshot_sha256"]).lower():
        return "screenshot SHA-256 does not match"
    executable = Path(str(record["executable_path"]))
    if not executable.is_absolute():
        executable = base_dir / executable
    if not executable.is_file() or _sha256_file(executable) != str(record["executable_sha256"]).lower():
        return "editor executable is missing or its SHA-256 does not match"
    return None


def evaluate(
    manifest: dict[str, Any],
    inventory_names: list[str],
    execution_results: dict[str, dict[str, Any]],
    evidence: dict[str, Any],
    *,
    current_source_tree_hash: str,
    base_dir: Path = ROOT,
    command_records: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    issues = validate_manifest(manifest)
    if not inventory_names:
        issues.append({"code": "zero_inventory", "detail": "CTest returned zero registered tests"})
    required_cases = [case for requirement in manifest.get("requirements", []) for case in requirement.get("subcases", [])]
    if not any(case.get("test_name") in set(inventory_names) for case in required_cases):
        issues.append({"code": "zero_required_tests", "detail": "CTest registered none of the required NED subcases"})
    inventory_set = set(inventory_names)
    evidence_by_id = {
        str(record.get("subcase_id")): record
        for record in evidence.get("records", [])
        if isinstance(record, dict) and record.get("subcase_id")
    }
    subcase_results: list[dict[str, Any]] = []
    for requirement in manifest.get("requirements", []):
        if not isinstance(requirement, dict):
            continue
        for case in requirement.get("subcases", []):
            if not isinstance(case, dict):
                continue
            name = str(case.get("test_name", ""))
            status = "not_run"
            detail = ""
            if name not in inventory_set:
                issues.append({"code": "missing_registration", "detail": f"CTest is missing {name}"})
                detail = "required test is not registered"
            elif name not in execution_results:
                issues.append({"code": "test_result_missing", "detail": f"no CTest result was collected for {name}"})
                detail = "registered test was not executed or its result was not collected"
            else:
                result = execution_results[name]
                result_status = str(result.get("status", "not_run"))
                exit_code = result.get("exit_code")
                if result_status not in ALLOWED_STATUSES:
                    status, detail = "failed", f"unsupported execution status {result_status!r}"
                elif result_status == "passed" and exit_code not in (0, None):
                    status, detail = "failed", f"passing status contradicts exit code {exit_code}"
                else:
                    status = result_status
                    detail = str(result.get("detail", ""))
                if status != "passed":
                    issues.append({"code": "test_not_passed", "detail": f"{name}: {status}{(': ' + detail) if detail else ''}"})

            if case.get("desktop_required"):
                evidence_record = evidence_by_id.get(str(case.get("full_id")))
                evidence_error = "desktop evidence record is missing" if evidence_record is None else _load_evidence_record(
                    {**evidence_record, "evidence_tags": case.get("evidence_tags", [])},
                    str(case.get("full_id")), evidence, current_source_tree_hash, base_dir,
                )
                if evidence_error is not None:
                    if status not in {"failed", "skipped", "timeout", "blocked"}:
                        status = "not_verified"
                    detail = evidence_error
                    issues.append({"code": "desktop_evidence_missing_or_invalid", "detail": f"{case.get('full_id')}: {evidence_error}"})

            subcase_results.append({
                "id": str(case.get("full_id")),
                "test_name": name,
                "status": status,
                "detail": detail,
                "desktop_required": bool(case.get("desktop_required")),
            })

    counts = {status: sum(item["status"] == status for item in subcase_results) for status in sorted(ALLOWED_STATUSES)}
    return {
        "schema_version": 1,
        "status": "passed" if subcase_results and all(item["status"] == "passed" for item in subcase_results) and not issues else "failed",
        "source_tree_hash": current_source_tree_hash,
        "required_subcase_count": len(subcase_results),
        "test_counts": counts,
        "inventory_count": len(inventory_names),
        "subcases": subcase_results,
        "issues": issues,
        "commands": list(command_records or []),
    }


def inventory_names(build_dir: Path, config: str) -> tuple[list[str], list[str], str]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1", "--no-tests=error"]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"CTest inventory failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    inventory = json.loads(result.stdout)
    return [test["name"] for test in inventory.get("tests", [])], command, result.stdout + result.stderr


def parse_junit(path: Path, expected_names: list[str]) -> dict[str, dict[str, Any]]:
    if not path.is_file():
        return {}
    root = ET.parse(path).getroot()
    results: dict[str, dict[str, Any]] = {}
    for testcase in root.iter("testcase"):
        name = testcase.attrib.get("name", "")
        failure = testcase.find("failure")
        error = testcase.find("error")
        skipped = testcase.find("skipped")
        if skipped is not None:
            results[name] = {"status": "skipped", "exit_code": 0, "detail": skipped.attrib.get("message", "")}
        elif failure is not None or error is not None:
            node = failure if failure is not None else error
            detail = " ".join([node.attrib.get("message", ""), node.text or ""]).strip()
            status = "timeout" if "timeout" in detail.lower() else "failed"
            results[name] = {"status": status, "exit_code": 1, "detail": detail[:4000]}
        else:
            results[name] = {"status": "passed", "exit_code": 0}
    return {name: results[name] for name in expected_names if name in results}


def run_registered_tests(build_dir: Path, config: str, names: list[str], timeout_seconds: int) -> tuple[dict[str, dict[str, Any]], dict[str, Any] | None]:
    if not names:
        return {}, None
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    report_dir = ROOT / "build" / "reports" / "editor_interaction" / "runs"
    report_dir.mkdir(parents=True, exist_ok=True)
    junit_path = report_dir / f"ctest-{stamp}.xml"
    log_path = report_dir / f"ctest-{stamp}.log"
    regex = "^(" + "|".join(re.escape(name).replace(r"\-", "-") for name in names) + ")$"
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "-R", regex, "--output-on-failure", "--no-tests=error", "--output-junit", str(junit_path)]
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False, timeout=timeout_seconds)
        output = result.stdout + result.stderr
        exit_code = result.returncode
        results = parse_junit(junit_path, names)
        if exit_code != 0 and not results:
            results = {name: {"status": "failed", "exit_code": exit_code, "detail": "CTest failed before per-test results were collected"} for name in names}
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode(errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        output = stdout + stderr + f"\nCTest timed out after {timeout_seconds} seconds.\n"
        exit_code = 124
        results = {name: {"status": "timeout", "exit_code": exit_code, "detail": output[-4000:]} for name in names}
    log_path.write_text(output, encoding="utf-8")
    command_record = {
        "argv": command,
        "cwd": str(ROOT),
        "started_utc": started,
        "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "exit_code": exit_code,
        "log_path": str(log_path),
        "log_sha256": _sha256_file(log_path),
        "junit_path": str(junit_path) if junit_path.is_file() else None,
    }
    return results, command_record


def run_controlled_failure_probe() -> dict[str, Any]:
    """Exercise CMake/CTest's nonzero failure propagation in a disposable project."""
    with tempfile.TemporaryDirectory(prefix="audioalgo-ni-failure-probe-") as temporary:
        project_dir = Path(temporary)
        build_dir = project_dir / "build"
        python_exe = Path(sys.executable).as_posix()
        (project_dir / "fail_probe.py").write_text("raise SystemExit(23)\n", encoding="utf-8")
        cmake_text = (
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(AudioAlgoEditorInteractionProbe NONE)\n"
            "enable_testing()\n"
            f'set(PROBE_PYTHON "{python_exe}")\n'
            'add_test(NAME controlled_assertion_failure COMMAND "${PROBE_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/fail_probe.py")\n'
        )
        (project_dir / "CMakeLists.txt").write_text(cmake_text, encoding="utf-8")
        configure_command = ["cmake", "-S", str(project_dir), "-B", str(build_dir)]
        configure = subprocess.run(configure_command, cwd=ROOT, text=True, encoding="utf-8", errors="replace",
                                   capture_output=True, check=False, timeout=60)
        probe_command = ["ctest", "--test-dir", str(build_dir), "--output-on-failure", "--no-tests=error"]
        if configure.returncode == 0:
            probe = subprocess.run(probe_command, cwd=ROOT, text=True, encoding="utf-8", errors="replace",
                                   capture_output=True, check=False, timeout=60)
            probe_output = configure.stdout + configure.stderr + probe.stdout + probe.stderr
            probe_exit = probe.returncode
        else:
            probe_output = configure.stdout + configure.stderr
            probe_exit = configure.returncode
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    log_path = ROOT / "build" / "reports" / "loops" / "NI0000" / f"controlled-failure-probe-{stamp}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(probe_output, encoding="utf-8")
    return {
        "configure_command": configure_command,
        "configure_exit_code": configure.returncode,
        "command": probe_command,
        "exit_code": probe_exit,
        "expected_nonzero": True,
        "log_path": str(log_path),
        "probe_log_sha256": _sha256_file(log_path),
    }


def write_report(report: dict[str, Any]) -> Path:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    report_dir = ROOT / "build" / "reports" / "editor_interaction" / stamp
    report_dir.mkdir(parents=True, exist_ok=True)
    path = report_dir / "verification.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return path


def run_verification(build_dir: Path, config: str, manifest_path: Path, evidence_path: Path, timeout_seconds: int) -> tuple[dict[str, Any], Path]:
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    current_hash = source_tree_hash()
    manifest = load_manifest(manifest_path)
    evidence = json.loads(evidence_path.read_text(encoding="utf-8")) if evidence_path.is_file() else {"schema_version": 1, "records": []}
    try:
        names, inventory_command, inventory_output = inventory_names(build_dir, config)
        inventory_error = None
    except Exception as exc:
        names, inventory_command, inventory_output = [], ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1", "--no-tests=error"], ""
        inventory_error = f"{type(exc).__name__}: {exc}"
    expected_names = [case["test_name"] for requirement in manifest["requirements"] for case in requirement["subcases"]]
    registered_required = [name for name in expected_names if name in set(names)]
    execution_results, test_command_record = run_registered_tests(build_dir, config, registered_required, timeout_seconds)
    commands: list[dict[str, Any]] = [{
        "argv": inventory_command,
        "cwd": str(ROOT),
        "exit_code": 1 if inventory_error else 0,
        "stdout_sha256": _sha256_bytes(inventory_output.encode("utf-8")),
        "detail": inventory_error,
    }]
    if test_command_record is not None:
        commands.append(test_command_record)
    report = evaluate(
        manifest, names, execution_results, evidence,
        current_source_tree_hash=current_hash,
        command_records=commands,
    )
    report.update({
        "scope": "node interaction acceptance; does not establish platform G0-G6 completion",
        "build_dir": str(build_dir.resolve()),
        "configuration": config,
        "platform": platform.platform(),
        "source_revision": None,
        "started_utc": started,
        "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "evidence_manifest": str(evidence_path.resolve()),
        "registered_required_subcase_count": len(registered_required),
        "environment_limitations": ["Desktop and DPI acceptance require captured Windows desktop evidence tied to this source and executable."],
    })
    editor_exe = build_dir / "bin" / "AudioAlgo Editor.exe"
    if editor_exe.is_file():
        report["editor_executable"] = {"path": str(editor_exe.resolve()), "sha256": _sha256_file(editor_exe)}
    if inventory_error:
        report["issues"].append({"code": "inventory_command_failed", "detail": inventory_error})
        report["status"] = "failed"
    if not evidence_path.is_file():
        report["issues"].append({"code": "desktop_evidence_manifest_missing", "detail": f"desktop evidence manifest does not exist: {evidence_path}"})
        report["status"] = "failed"
    report_path = write_report(report)
    return report, report_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "windows-debug")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--evidence", type=Path, default=ROOT / "build" / "reports" / "editor_interaction" / "desktop_evidence.json")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    try:
        report, report_path = run_verification(args.build_dir.resolve(), args.config, args.manifest.resolve(), args.evidence.resolve(), args.timeout)
    except Exception as exc:
        print(f"interaction verifier infrastructure failure: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    counts = report["test_counts"]
    print(
        "node interaction: "
        f"{report['status']}; registered={report.get('registered_required_subcase_count', 0)}/"
        f"{report['required_subcase_count']} subcases; passed={counts['passed']} failed={counts['failed']} "
        f"skipped={counts['skipped']} not_run={counts['not_run']} not_verified={counts['not_verified']}"
    )
    print(f"report: {report_path}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
