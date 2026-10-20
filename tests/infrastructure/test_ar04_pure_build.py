"""Verify a clean algorithms-only CMake build without JUCE or Python discovery."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hash(root: Path) -> str:
    sources = {path.relative_to(root).as_posix(): sha(path) for path in sorted(root.rglob("*"))
               if path.is_file() and not {"build", ".git", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
               .intersection(path.relative_to(root).parts)}
    return hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=("b", "c", "d"), required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--catch2-source", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--generator", required=True)
    parser.add_argument("--architecture", default="")
    args = parser.parse_args()
    root = args.source_root.resolve()
    catch2_source = args.catch2_source.resolve()
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence_dir = root / "build" / "reports" / "architecture_refactor" / f"ar04_{args.case}" / stamp
    evidence_dir.mkdir(parents=True, exist_ok=False)
    build_dir = evidence_dir / "fresh-build"
    build_dir_was_absent = not build_dir.exists()
    commands: list[dict[str, object]] = []
    violations: list[str] = []

    def run(label: str, argv: list[str]) -> subprocess.CompletedProcess[str]:
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        try:
            result = subprocess.run(argv, cwd=root, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", check=False)
            output = (result.stdout or "") + (result.stderr or "")
            code = result.returncode
        except OSError as exc:
            output, code = f"command could not start: {exc}", 127
            result = subprocess.CompletedProcess(argv, code, "", output)
        ended = dt.datetime.now(dt.timezone.utc).isoformat()
        log = evidence_dir / f"{label}.log"
        log.write_text(output, encoding="utf-8")
        commands.append({"argv": argv, "cwd": str(root), "started_utc": started,
                         "ended_utc": ended, "exit_code": code,
                         "log_path": str(log), "log_sha256": sha(log)})
        return result

    if not catch2_source.is_dir():
        violations.append(f"offline Catch2 source is unavailable: {catch2_source}")
    configure = [args.cmake, "-S", str(root), "-B", str(build_dir), "-G", args.generator]
    if args.architecture:
        configure.extend(["-A", args.architecture])
    configure.extend([
                 "-DAUDIO_BUILD_RUNTIME=OFF", "-DAUDIO_BUILD_EDITOR=OFF",
                 "-DAUDIO_ENABLE_PYTHON=OFF", "-DAUDIO_ENABLE_SPEEXDSP=OFF",
                 "-DAUDIO_BUILD_TESTS=ON", "-DAUDIO_BUILD_INFRASTRUCTURE_TESTS=OFF",
                 "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
                 "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                 f"-DFETCHCONTENT_SOURCE_DIR_CATCH2={catch2_source}"])
    configure_result = run("configure", configure)
    if configure_result.returncode != 0:
        violations.append(f"fresh algorithms-only CMake configure failed with exit {configure_result.returncode}")
    if configure_result.returncode == 0:
        targets = (["audio_core", "audio_algorithms", "audio_algorithm_tests"]
                   if args.case == "b" else
                   ["audio_algorithm_tests", "audio_application_tests", "audio_editor_application_tests"]
                   if args.case == "c" else ["audio_algorithm_tests"])
        build = [args.cmake, "--build", str(build_dir), "--config", args.config,
                 "--target", *targets, "--parallel", "4"]
        build_result = run("build", build)
        if build_result.returncode != 0:
            violations.append(f"pure target build failed with exit {build_result.returncode}")

        cache = build_dir / "CMakeCache.txt"
        if not cache.is_file():
            violations.append("fresh CMakeCache.txt was not produced")
        else:
            cache_text = cache.read_text(encoding="utf-8", errors="replace")
            if re.search(r"^(?:JUCE_SOURCE_DIR|FETCHCONTENT_SOURCE_DIR_JUCE|Python3_EXECUTABLE|Python3_ROOT_DIR):", cache_text, re.I | re.M):
                violations.append("fresh algorithms-only cache contains a JUCE or Python discovery path")
            if "AUDIO_BUILD_RUNTIME:BOOL=OFF" not in cache_text:
                violations.append("AUDIO_BUILD_RUNTIME=OFF was not consumed by CMake")
        if (build_dir / "_deps" / "juce-src").exists():
            violations.append("a fresh algorithms-only build populated a JUCE source tree")

    if args.case in {"c", "d"} and configure_result.returncode == 0 and commands[-1]["exit_code"] == 0:
        ctest = [args.ctest, "--test-dir", str(build_dir), "-C", args.config,
                 "--output-on-failure", "--output-junit", str(evidence_dir / "algorithm-tests.xml")]
        if args.case == "d":
            ctest.extend(["-R", "^AR-04\\.d$"])
        test_result = run("ctest", ctest)
        if test_result.returncode != 0:
            violations.append(f"pure algorithm CTest failed with exit {test_result.returncode}")
        junit = evidence_dir / "algorithm-tests.xml"
        if junit.is_file():
            suite = ET.parse(junit).getroot()
            cases = suite.findall(".//testcase")
            required_prefix = "AR-04.d" if args.case == "d" else "DSP-01"
            algorithm_cases = [case for case in cases if case.get("name", "").startswith(required_prefix)]
            failed = [case.get("name", "") for case in cases
                      if case.find("failure") is not None or case.find("error") is not None]
            skipped = [case.get("name", "") for case in cases if case.find("skipped") is not None]
            if not cases or not algorithm_cases:
                violations.append("CTest JUnit contains no executed pure algorithm tests")
            if failed:
                violations.append(f"CTest JUnit records failures: {failed}")
            if skipped:
                violations.append(f"CTest JUnit records unexecuted tests: {skipped}")
        else:
            violations.append("CTest JUnit report is missing")
        if "audio_algorithm_tests" not in (build_dir / "CTestTestfile.cmake").read_text(encoding="utf-8", errors="replace"):
            violations.append("the fresh CTest inventory is not backed by the dedicated algorithm test target")

    report = {"schema_version": 1, "case": f"AR-04.{args.case}",
              "status": "passed" if not violations else "failed",
              "source_tree_hash": source_hash(root), "commands": commands,
              "build_dir": str(build_dir), "fresh_build_dir": build_dir_was_absent,
              "violations": violations}
    report_path = evidence_dir / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "case": report["case"],
                      "report": str(report_path), "violations": violations}, ensure_ascii=False))
    return 0 if not violations else 1


if __name__ == "__main__":
    raise SystemExit(main())
