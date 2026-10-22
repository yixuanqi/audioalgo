"""Run registered CTest checks and reject missing acceptance IDs."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_SPEC = ROOT / "doc" / "testing_audio_pipeline_editor.md"
GATE_ORDER = ["G0", "G1", "G2", "G3", "G4", "G5", "G6"]
ID_RE = re.compile(r"^([A-Z][A-Z0-9]*-\d+)\b")
# Required IDs whose acceptance text demands a real desktop interaction, a real audio device,
# or DPI screenshot evidence. A green CTest suite alone can never satisfy these.
DESKTOP_EVIDENCE_IDS = {"UI-01", "UI-02", "UI-04", "UI-07"}
TEST_LINE = re.compile(
    r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(?P<name>.+?)\s+[*.\s]*?"
    r"(?P<marker>Passed|Failed|Skipped|Not Run|Timeout|Exception)\b",
    re.MULTILINE)
MARKER_STATUS = {
    "Passed": "passed",
    "Failed": "failed",
    "Skipped": "skipped",
    "Not Run": "not_run",
    "Timeout": "timeout",
    "Exception": "failed",
}
_LAST_REPORT: Path | None = None
INVARIANT_GATE_IDS = {
    "G0": {"BLD-01", "BLD-02", "BLD-03", "BLD-04", "LOOP-01"},
    "G1": {"UI-01", "UI-02"},
    "G4": {"QUAL-01", "QUAL-02", "QUAL-03"},
    "G5": {"UI-04", "UI-07"},
    "G6": {"LOOP-02", "EXT-01", "EXT-02", "PKG-01"},
}


def required_test_ids(gate: str) -> set[str]:
    if gate not in GATE_ORDER:
        raise ValueError(f"unknown gate: {gate}")
    max_index = GATE_ORDER.index(gate)
    found: set[str] = set()
    for line in TEST_SPEC.read_text(encoding="utf-8").splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and re.fullmatch(r"[A-Z][A-Z0-9]*-\d+", cells[0]) and cells[1] in GATE_ORDER:
            if GATE_ORDER.index(cells[1]) <= max_index:
                found.add(cells[0])
    # LOOP-01 is introduced in the final table instead of the main test matrix.
    if max_index >= 0:
        found.add("LOOP-01")
    if max_index == len(GATE_ORDER) - 1:
        found.add("LOOP-02")
    for invariant_gate, invariant_ids in INVARIANT_GATE_IDS.items():
        if GATE_ORDER.index(invariant_gate) <= max_index:
            found.update(invariant_ids)
    return found


def desktop_evidence_ids(gate: str) -> set[str]:
    """Required IDs for this Gate that also demand real desktop/device evidence."""
    return required_test_ids(gate) & DESKTOP_EVIDENCE_IDS


def last_report_path() -> Path | None:
    return _LAST_REPORT


def test_results(stdout: str) -> dict[str, str]:
    """Per-test execution status parsed from the CTest console listing."""
    results: dict[str, str] = {}
    for match in TEST_LINE.finditer(stdout or ""):
        results[match.group("name").strip()] = MARKER_STATUS[match.group("marker")]
    return results


def verify_desktop_evidence(ids: set[str], evidence_path: Path | None, source_hash: str) -> tuple[list[str], dict]:
    """Every desktop-required ID needs a hash-verified screenshot, executable and record."""
    if not ids:
        return [], {}
    errors: list[str] = []
    if evidence_path is None or not Path(evidence_path).is_file():
        return [f"desktop evidence file is required for: {', '.join(sorted(ids))}"], {}
    try:
        evidence = json.loads(Path(evidence_path).read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"desktop evidence is unreadable: {exc}"], {}
    if not isinstance(evidence, dict):
        return ["desktop evidence must be a JSON object keyed by required case ID"], {}
    for case_id in sorted(ids):
        record = evidence.get(case_id)
        reason = verify_desktop_record(record, source_hash)
        if reason is not None:
            errors.append(f"{case_id}: {reason}")
    return errors, evidence


def architecture_verifier():
    """Single owner of desktop-evidence semantics; imported lazily to keep this script standalone."""
    spec = importlib.util.spec_from_file_location(
        "architecture_verifier", ROOT / "scripts" / "verify_architecture_refactor.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_desktop_record(record: object, source_hash: str) -> str | None:
    return architecture_verifier().verify_desktop_record(record, source_tree_hash=source_hash, root=ROOT)


def inventory_names(build_dir: Path, config: str) -> tuple[list[str], list[str]]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1", "--no-tests=error"]
    result = subprocess.run(command, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"CTest inventory failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    inventory = json.loads(result.stdout)
    names = [test["name"] for test in inventory.get("tests", [])]
    return names, command


def source_tree_hash() -> str:
    excluded = {"build", ".git", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
    paths = []
    for folder, directories, names in os.walk(ROOT, followlinks=False):
        directories[:] = [name for name in directories if name not in excluded]
        paths.extend(Path(folder) / name for name in names if name not in excluded)
    manifest = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(paths) if path.is_file()}
    return hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()


def write_report(gate: str, report: dict) -> Path:
    report_dir = ROOT / "build" / "reports" / "gates" / gate
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report_path = report_dir / f"verification-{stamp}.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    global _LAST_REPORT
    _LAST_REPORT = report_path
    return report_path


def verify_failure_probe(build_dir: Path, config: str) -> int:
    fixture_source = ROOT / "tests" / "infrastructure" / "gate_failure_probe_fixture"
    configure_on = ["cmake", "-S", str(fixture_source), "-B", str(build_dir), "-DAUDIO_GATE_FAILURE_PROBE=ON"]
    configured = subprocess.run(configure_on, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
    if configured.returncode != 0:
        print((configured.stdout or "") + (configured.stderr or ""), file=sys.stderr)
        return 2
    names, inventory_command = inventory_names(build_dir, config)
    probe = "G0_temporary_assertion_failure_probe"
    if probe not in names:
        print(f"failure probe is not registered in {build_dir}", file=sys.stderr)
        return 2
    required = required_test_ids("G0")
    registered = {match.group(1) for name in names if (match := ID_RE.match(name))}
    if required - registered:
        print("isolated probe fixture is missing Gate IDs: " + ", ".join(sorted(required - registered)), file=sys.stderr)
        return 2
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "-R", f"^{probe}$", "--output-on-failure", "--no-tests=error"]
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    result = subprocess.run(command, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
    ended = dt.datetime.now(dt.timezone.utc).isoformat()
    failed_gate_code = run_gate("G0", build_dir, config)
    failed_gate_path = last_report_path()
    failed_gate = json.loads(failed_gate_path.read_text(encoding="utf-8")) if failed_gate_path else {}
    gate_check = next((check for check in failed_gate.get("checks", []) if check.get("name") == "ctest_registered_suite"), {})
    ctest_output = gate_check.get("output", "")
    propagated = (
        result.returncode != 0
        and failed_gate_code != 0
        and failed_gate.get("status") == "failed"
        and gate_check.get("exit_code", 0) != 0
        and probe in ctest_output
        and failed_gate.get("missing_required_ids") == []
    )
    configure_off = ["cmake", "-S", str(fixture_source), "-B", str(build_dir), "-DAUDIO_GATE_FAILURE_PROBE=OFF"]
    restored = subprocess.run(configure_off, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
    clean_names, clean_inventory_command = inventory_names(build_dir, config) if restored.returncode == 0 else ([], [])
    clean_gate_code = run_gate("G0", build_dir, config) if restored.returncode == 0 and probe not in clean_names else 2
    clean_gate_path = last_report_path() if clean_gate_code != 2 else None
    clean_gate = json.loads(clean_gate_path.read_text(encoding="utf-8")) if clean_gate_path else {}
    clean = restored.returncode == 0 and probe not in clean_names and clean_gate_code == 0 and clean_gate.get("status") == "passed"
    report_status = "red_observed" if propagated and clean else "failed_to_propagate_or_restore"
    report = {
        "schema_version": 1,
        "gate": "G0",
        "mode": "temporary_failure_propagation_probe",
        "status": report_status,
        "source_tree_hash": source_tree_hash(),
        "started_utc": started,
        "ended_utc": ended,
        "cwd": str(ROOT),
        "configure_on_command": configure_on,
        "configure_on_exit_code": configured.returncode,
        "inventory_command": inventory_command,
        "command": command,
        "exit_code": result.returncode,
        "expected_nonzero": True,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "gate_command": [sys.executable, str(ROOT / "scripts" / "verify_gate.py"), "--gate", "G0", "--build-dir", str(build_dir)],
        "gate_exit_code": failed_gate_code,
        "gate_report_path": str(failed_gate_path) if failed_gate_path else None,
        "gate_report_sha256": hashlib.sha256(failed_gate_path.read_bytes()).hexdigest() if failed_gate_path else None,
        "gate_status": failed_gate.get("status"),
        "gate_ctest_exit_code": gate_check.get("exit_code"),
        "probe_named_in_gate_failure": probe in ctest_output,
        "configure_off_command": configure_off,
        "configure_off_exit_code": restored.returncode,
        "clean_inventory_command": clean_inventory_command,
        "clean_inventory_names": clean_names,
        "probe_absent_after_restore": probe not in clean_names,
        "clean_gate_exit_code": clean_gate_code,
        "clean_gate_report_path": str(clean_gate_path) if clean_gate_path else None,
        "clean_gate_report_sha256": hashlib.sha256(clean_gate_path.read_bytes()).hexdigest() if clean_gate_path else None,
        "clean_gate_status": clean_gate.get("status"),
    }
    report_dir = ROOT / "build" / "reports" / "gates" / "G0"
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report_path = report_dir / f"failure_probe-{stamp}.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"failure probe: {report['status']} (probe CTest exit {result.returncode}, Gate exit {failed_gate_code}, restored Gate exit {clean_gate_code})")
    print(f"evidence: {report_path}")
    return 0 if result.returncode != 0 else 1


def run_gate(gate: str, build_dir: Path, config: str, probe: str | None = None,
             evidence: Path | None = None) -> int:
    started_at = time.monotonic()
    start_utc = dt.datetime.now(dt.timezone.utc).isoformat()
    current_source_hash = source_tree_hash()
    report: dict = {
        "schema_version": 1,
        "gate": gate,
        "probe": probe,
        "status": "not_run",
        "source_tree_hash": current_source_hash,
        "started_utc": start_utc,
        "build_dir": str(build_dir),
        "configuration": config,
        "checks": [],
        "missing_required_ids": [],
        "status_failures": [],
        "evidence_failures": [],
        "limitations": [],
    }
    try:
        names, inventory_command = inventory_names(build_dir, config)
        expected = required_test_ids(gate)
        registered = {match.group(1) for name in names if (match := ID_RE.match(name))}
        missing = sorted(expected - registered)
        report["inventory_command"] = inventory_command
        report["registered_test_names"] = names
        report["required_test_ids"] = sorted(expected)
        report["registered_test_ids"] = sorted(registered)
        report["missing_required_ids"] = missing

        command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--output-on-failure", "--no-tests=error"]
        test_start = dt.datetime.now(dt.timezone.utc).isoformat()
        result = subprocess.run(command, cwd=ROOT, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
        test_end = dt.datetime.now(dt.timezone.utc).isoformat()
        run_stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        log_path = ROOT / "build" / "reports" / "gates" / gate / f"ctest-{run_stamp}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        output = result.stdout + result.stderr
        log_path.write_text(output, encoding="utf-8")

        outcomes = test_results(output)
        report["test_outcomes"] = outcomes
        if not outcomes:
            report["infrastructure_errors"] = ["CTest produced no per-test result lines to classify"]
        counts = {state: sum(value == state for value in outcomes.values())
                  for state in ("passed", "failed", "skipped", "not_run", "timeout")}
        counts["inventory"] = len(names)
        counts["unclassified"] = len(names) - len(outcomes)
        report["test_counts"] = counts

        # A required ID only passes when every mapped test both ran and passed.
        status_failures: list[str] = []
        for case_id in sorted(expected):
            mapped = [name for name in names if (match := ID_RE.match(name)) and match.group(1) == case_id]
            if not mapped:
                continue
            states = {name: outcomes.get(name, "not_run") for name in mapped}
            bad = {name: state for name, state in states.items() if state != "passed"}
            if bad:
                status_failures.extend(f"{case_id}: {name} {state}" for name, state in sorted(bad.items()))
        report["status_failures"] = status_failures

        evidence_failures, evidence_data = verify_desktop_evidence(
            desktop_evidence_ids(gate), evidence, current_source_hash)
        report["evidence_failures"] = evidence_failures
        report["desktop_required_ids"] = sorted(desktop_evidence_ids(gate))
        if evidence_data:
            report["desktop_evidence"] = evidence_data

        report["checks"].append({
            "name": "ctest_registered_suite",
            "command": command,
            "started_utc": test_start,
            "ended_utc": test_end,
            "exit_code": result.returncode,
            "log_path": str(log_path),
            "log_sha256": hashlib.sha256(log_path.read_bytes()).hexdigest(),
            "output": output,
        })
        failures = (result.returncode != 0 or bool(missing) or bool(status_failures)
                    or bool(evidence_failures) or not outcomes)
        report["status"] = "failed" if failures else "passed"
        report["limitations"] = [
            "Skipped, not-run and timed-out required tests fail the Gate; passing counts only executed passes.",
            "Desktop/device acceptance additionally requires hash-verified evidence per required ID.",
        ]
        print(f"{gate}: {report['status']}; registered={len(registered)} required={len(expected)} "
              f"missing={len(missing)} status_failures={len(status_failures)} "
              f"evidence_failures={len(evidence_failures)} ctest_exit={result.returncode}")
        if missing:
            print("missing test IDs: " + ", ".join(missing))
        if status_failures:
            print("non-passing required tests: " + "; ".join(status_failures))
        if evidence_failures:
            print("desktop evidence failures: " + "; ".join(evidence_failures))
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
    except Exception as exc:  # The report records infrastructure failures as failures.
        report["status"] = "failed"
        report["infrastructure_error"] = f"{type(exc).__name__}: {exc}"
        print(report["infrastructure_error"], file=sys.stderr)
    report["duration_ms"] = int((time.monotonic() - started_at) * 1000)
    report_path = write_report(gate, report)
    print(f"evidence: {report_path}")
    return 0 if report["status"] == "passed" else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", choices=GATE_ORDER, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "windows-debug")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--failure-probe-only", action="store_true")
    parser.add_argument("--evidence", type=Path, help="JSON map from desktop-required case ID to artifact record")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    if args.failure_probe_only:
        if args.gate != "G0":
            parser.error("--failure-probe-only is limited to G0")
        return verify_failure_probe(build_dir, args.config)
    return run_gate(args.gate, build_dir, args.config, evidence=args.evidence)


if __name__ == "__main__":
    raise SystemExit(main())
