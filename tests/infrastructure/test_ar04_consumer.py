"""Build, install and run a separate consumer of public Gain and ChannelSwap."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest(root: Path) -> dict[str, str]:
    excluded = {"build", ".git", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
    paths = []
    for folder, directories, names in os.walk(root, followlinks=False):
        directories[:] = [name for name in directories if name not in excluded]
        paths.extend(Path(folder) / name for name in names if name not in excluded)
    return {path.relative_to(root).as_posix(): sha(path) for path in sorted(paths) if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    args = parser.parse_args()
    root = args.source_root.resolve()
    build_dir = args.build_dir.resolve()
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence_dir = root / "build" / "reports" / "architecture_refactor" / "ar04_consumer" / stamp
    evidence_dir.mkdir(parents=True, exist_ok=False)
    prefix = evidence_dir / "install-prefix"
    consumer_build = evidence_dir / "consumer-build"
    commands = []
    steps = [
        [args.cmake, "--build", str(build_dir), "--config", args.config, "--target", "audio_algorithms", "audio_core", "--parallel", "4"],
        [args.cmake, "--install", str(build_dir), "--config", args.config, "--prefix", str(prefix),
         "--component", "AudioAlgoAlgorithms"],
        [args.cmake, "-S", str(root / "examples" / "algorithms_consumer"), "-B", str(consumer_build),
         f"-DAudioAlgo_DIR={prefix / 'lib' / 'cmake' / 'AudioAlgo'}",
         "-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON", "-DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=ON"],
        [args.cmake, "--build", str(consumer_build), "--config", args.config, "--parallel", "4"],
    ]
    overall = 0
    for index, command in enumerate(steps, 1):
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=False)
        ended = dt.datetime.now(dt.timezone.utc).isoformat()
        output = (result.stdout or "") + (result.stderr or "")
        log = evidence_dir / f"step-{index}.log"
        log.write_text(output, encoding="utf-8")
        commands.append({"argv": command, "cwd": str(root), "started_utc": started, "ended_utc": ended,
                         "exit_code": result.returncode, "log_path": str(log), "log_sha256": sha(log)})
        if result.returncode != 0:
            overall = result.returncode
            break

    if overall == 0:
        package_dir = prefix / "lib" / "cmake" / "AudioAlgo"
        installed_files = sorted(path for path in prefix.rglob("*") if path.is_file())
        export_files = [path for path in installed_files if path.suffix == ".cmake"]
        forbidden = ("juce", "audio_node_sdk", "audio_project", "view", str(root).lower())
        violations = []
        if not (package_dir / "AudioAlgoConfig.cmake").is_file():
            violations.append("AudioAlgoConfig.cmake is missing from the install prefix")
        if not (prefix / "include/audio_algorithms/Gain.h").is_file():
            violations.append("public Gain header is missing from the install prefix")
        if not (prefix / "include/audio_algorithms/ChannelSwap.h").is_file():
            violations.append("public ChannelSwap header is missing from the install prefix")
        if not (prefix / "include/audio_core/AudioBuffer.h").is_file():
            violations.append("public AudioBuffer header is missing from the install prefix")
        if not export_files:
            violations.append("no CMake target export was installed")
        for path in export_files:
            content = path.read_text(encoding="utf-8", errors="replace").lower()
            if any(token in content for token in forbidden):
                violations.append(f"export leaks a forbidden/source-tree dependency: {path.name}")
        for path in installed_files:
            if "juce" in path.relative_to(prefix).as_posix().lower():
                violations.append(f"JUCE file was included in the algorithms component: {path}")
        validation_log = evidence_dir / "install-validation.log"
        validation_log.write_text("\n".join(violations) if violations else "installed core/algorithms headers and exports are isolated", encoding="utf-8")
        commands.append({"argv": ["validate installed AudioAlgoAlgorithms component"], "cwd": str(root),
                         "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                         "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                         "exit_code": 1 if violations else 0, "log_path": str(validation_log),
                         "log_sha256": sha(validation_log)})
        if violations:
            overall = 1

    executable = consumer_build / "bin" / ("algorithms_consumer.exe" if os.name == "nt" else "algorithms_consumer")
    if overall == 0:
        run_command = [str(executable)]
        run_started = dt.datetime.now(dt.timezone.utc).isoformat()
        try:
            result = subprocess.run(run_command, cwd=consumer_build, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", check=False)
            run_code = result.returncode
            run_output = (result.stdout or "") + (result.stderr or "")
        except OSError as exc:
            run_code = 127
            run_output = f"consumer executable could not start: {exc}"
        run_ended = dt.datetime.now(dt.timezone.utc).isoformat()
        run_log = evidence_dir / "consumer-run.log"
        run_log.write_text(run_output, encoding="utf-8")
        commands.append({"argv": run_command, "cwd": str(consumer_build),
                         "started_utc": run_started,
                         "ended_utc": run_ended,
                         "exit_code": run_code, "log_path": str(run_log), "log_sha256": sha(run_log)})
        overall = 0 if (run_code == 0 and "independent Gain consumer passed" in run_output
                        and "independent ChannelSwap consumer passed" in run_output) else (run_code or 1)

    report = {"schema_version": 1, "status": "passed" if overall == 0 else "failed",
              "source_tree_hash": hashlib.sha256(json.dumps(source_manifest(root), sort_keys=True).encode()).hexdigest(),
              "commands": commands, "consumer_binary": str(executable) if executable.is_file() else None,
              "consumer_binary_sha256": sha(executable) if executable.is_file() else None,
              "result": overall}
    report_path = evidence_dir / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(report_path)}, ensure_ascii=False))
    return 0 if overall == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
