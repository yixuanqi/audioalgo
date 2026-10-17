"""Run registered CTest checks and reject missing acceptance IDs."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_SPEC = ROOT / "doc" / "testing_audio_pipeline_editor.md"
GATE_ORDER = ["G0", "G1", "G2", "G3", "G4", "G5", "G6"]
ID_RE = re.compile(r"^([A-Z][A-Z0-9]*-\d+)\b")


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
    return found


def inventory_names(build_dir: Path, config: str) -> tuple[list[str], list[str]]:
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "--show-only=json-v1", "--no-tests=error"]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"CTest inventory failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    inventory = json.loads(result.stdout)
    names = [test["name"] for test in inventory.get("tests", [])]
    return names, command


def source_tree_hash() -> str:
    digest = hashlib.sha256()
    for path in sorted(ROOT.rglob("*")):
        relative_parts = path.relative_to(ROOT).parts
        if (
            not path.is_file()
            or "build" in relative_parts
            or ".git" in relative_parts
            or "__pycache__" in relative_parts
            or ".venv" in relative_parts
        ):
            continue
        rel = path.relative_to(ROOT).as_posix().encode("utf-8")
        digest.update(len(rel).to_bytes(4, "big"))
        digest.update(rel)
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def write_report(gate: str, report: dict) -> Path:
    report_dir = ROOT / "build" / "reports" / "gates" / gate
    report_dir.mkdir(parents=True, exist_ok=True)
    report_path = report_dir / "verification.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report_path


def verify_failure_probe(build_dir: Path, config: str) -> int:
    names, inventory_command = inventory_names(build_dir, config)
    probe = "G0_temporary_assertion_failure_probe"
    if probe not in names:
        print(f"failure probe is not registered in {build_dir}", file=sys.stderr)
        return 2
    command = ["ctest", "--test-dir", str(build_dir), "-C", config, "-R", f"^{probe}$", "--output-on-failure", "--no-tests=error"]
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    ended = dt.datetime.now(dt.timezone.utc).isoformat()
    report = {
        "schema_version": 1,
        "gate": "G0",
        "mode": "temporary_failure_propagation_probe",
        "status": "red_observed" if result.returncode != 0 else "failed_to_propagate",
        "source_tree_hash": source_tree_hash(),
        "started_utc": started,
        "ended_utc": ended,
        "inventory_command": inventory_command,
        "command": command,
        "exit_code": result.returncode,
        "expected_nonzero": True,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
    report_dir = ROOT / "build" / "reports" / "gates" / "G0"
    report_dir.mkdir(parents=True, exist_ok=True)
    report_path = report_dir / "failure_probe.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"failure probe: {report['status']} (CTest exit {result.returncode})")
    print(f"evidence: {report_path}")
    return 0 if result.returncode != 0 else 1


def run_gate(gate: str, build_dir: Path, config: str) -> int:
    started_at = time.monotonic()
    start_utc = dt.datetime.now(dt.timezone.utc).isoformat()
    report: dict = {
        "schema_version": 1,
        "gate": gate,
        "status": "not_run",
        "source_tree_hash": source_tree_hash(),
        "started_utc": start_utc,
        "build_dir": str(build_dir),
        "configuration": config,
        "checks": [],
        "missing_required_ids": [],
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
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
        test_end = dt.datetime.now(dt.timezone.utc).isoformat()
        run_stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        log_path = ROOT / "build" / "reports" / "gates" / gate / f"ctest-{run_stamp}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(result.stdout + result.stderr, encoding="utf-8")
        summary = re.search(r"(\d+)% tests? passed,\s*(\d+) tests? failed out of (\d+)", result.stdout)
        report["test_counts"] = {
            "passed": int(summary.group(3)) - int(summary.group(2)) if summary else (len(names) if result.returncode == 0 else 0),
            "failed": int(summary.group(2)) if summary else (1 if result.returncode else 0),
            "skipped": len(re.findall(r"\*\*\*Skipped", result.stdout + result.stderr)),
            "inventory": len(names),
        }
        report["checks"].append({
            "name": "ctest_registered_suite",
            "command": command,
            "started_utc": test_start,
            "ended_utc": test_end,
            "exit_code": result.returncode,
            "log_path": str(log_path),
            "output": result.stdout + result.stderr,
        })
        failures = result.returncode != 0 or bool(missing)
        report["status"] = "failed" if failures else "passed"
        report["limitations"] = ["GUI and physical audio-device acceptance require separate desktop/device evidence."]
        print(f"{gate}: {report['status']}; registered={len(registered)} required={len(expected)} missing={len(missing)} ctest_exit={result.returncode}")
        if missing:
            print("missing test IDs: " + ", ".join(missing))
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
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    if args.failure_probe_only:
        if args.gate != "G0":
            parser.error("--failure-probe-only is limited to G0")
        return verify_failure_probe(build_dir, args.config)
    return run_gate(args.gate, build_dir, args.config)


if __name__ == "__main__":
    raise SystemExit(main())
