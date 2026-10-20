"""AR-05.d fault injection: mutate one Gain coefficient inside an isolated copy only.

The cycle never writes to the active working tree and never builds into the main
build directory. It copies the source, configures a private build tree, proves the
baseline oracles pass, mutates only the copy, proves every oracle catches the
mutation, restores the copy and proves the oracles pass again. The active
``libs/audio_algorithms/Gain.cpp`` and the main build's binaries are hashed before
and after and must stay byte-identical.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[2]
ACTIVE_GAIN = ROOT / "libs" / "audio_algorithms" / "Gain.cpp"
ACTIVE_SOURCE_ROOTS = tuple(ROOT / name for name in (
    "libs", "apps", "plugins", "tests", "scripts", "doc", "examples", "cmake", "dependencies"))
MAIN_BUILD_PRESET_DIRS = tuple(ROOT / "build" / name for name in (
    "windows-debug", "windows-headless", "windows-release"))
SOURCE_INCLUDES = ("CMakeLists.txt", "CMakePresets.json", "cmake", "dependencies",
                   "libs", "apps", "plugins", "tests", "examples")
PRESERVED_BINARIES = ("audio_algorithm_tests.exe", "audioalgo_tests.exe", "audio_cli.exe")
OLD_ANCHOR = b"linear_gain_ = std::pow(10.0, config.gain_db / 20.0);"
MUTATED_ANCHOR = b"linear_gain_ = std::pow(10.0, (config.gain_db + 0.25) / 20.0);"
ORACLES = [
    ("independent", "audio_algorithm_tests.exe", "AR-05.a Gain APIs match a float64 oracle across the full block matrix"),
    ("node_and_juce", "audioalgo_tests.exe", "AR-05.b Gain node adapter matches an independent block oracle"),
    ("run_service_and_cli", "audioalgo_tests.exe", "E2E-01 RunService and CLI share a project snapshot and gain oracle"),
]


def sha_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha_file(path: Path) -> str:
    return sha_bytes(path.read_bytes())


def _inside(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def assert_isolated(target: Path, isolation_root: Path, main_build_dirs: Sequence[Path]) -> Path:
    """Return the resolved target, refusing anything the mutation cycle must never touch."""
    resolved = Path(target).resolve()
    root = Path(isolation_root).resolve()
    if not _inside(resolved, root):
        raise RuntimeError(f"mutation target escapes the isolation root {root}: {resolved}")
    for build_dir in main_build_dirs:
        if _inside(resolved, Path(build_dir).resolve()):
            raise RuntimeError(f"mutation target is inside an active build directory: {resolved}")
    for active_root in ACTIVE_SOURCE_ROOTS:
        if _inside(resolved, active_root.resolve()):
            raise RuntimeError(f"mutation target is inside the active working tree: {resolved}")
    return resolved


def reserve_isolation_root(parent: Path, name: str) -> Path:
    """Atomically reserve one named isolation directory; an existing one is never reused or removed."""
    parent = Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    candidate = parent / name
    try:
        candidate.mkdir(exist_ok=False)
    except FileExistsError as error:
        raise RuntimeError(f"isolation directory already belongs to another run: {candidate}") from error
    return candidate


def create_isolation_root(parent: Path) -> Path:
    """Create a unique short isolation root under ``parent``.

    ``mkdtemp`` guarantees an atomically created, never pre-existing directory, so two concurrent
    configurations (or agents) can never delete or reuse each other's source copy or build tree.
    """
    parent = Path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=str(parent)))


def copy_source_tree(destination: Path) -> Path:
    """Copy the buildable source tree into ``destination`` and never touch the original."""
    destination = Path(destination)
    for active_root in ACTIVE_SOURCE_ROOTS:
        if _inside(destination.resolve(), active_root.resolve()):
            raise RuntimeError(f"source copies must never be written inside the active tree: {destination}")
    if _inside(destination.resolve(), ROOT.resolve() / "libs"):
        raise RuntimeError(f"source copies must never overwrite algorithm sources: {destination}")
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    for name in SOURCE_INCLUDES:
        source = ROOT / name
        if not source.exists():
            raise RuntimeError(f"required source entry is missing: {source}")
        if source.is_dir():
            shutil.copytree(source, destination / name,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".pytest_cache"))
        else:
            shutil.copy2(source, destination / name)
    if (destination / "build").exists():
        raise RuntimeError("the isolated source copy must not contain a build tree")
    return destination


def workspace_snapshot(root: Path, main_build_dir: Path) -> dict[str, str]:
    """Hash the guarded active source file and the main build's binaries."""
    root = Path(root)
    main_build_dir = Path(main_build_dir)
    snapshot = {"libs/audio_algorithms/Gain.cpp": sha_file(root / "libs" / "audio_algorithms" / "Gain.cpp")}
    for name in PRESERVED_BINARIES:
        binary = main_build_dir / "bin" / name
        snapshot[f"bin/{name}"] = sha_file(binary) if binary.is_file() else "missing"
    return snapshot


def isolated_configure_arguments(source_root: Path, build_root: Path,
                                 juce_source: Path | None = None,
                                 catch2_source: Path | None = None,
                                 generator: str | None = None,
                                 architecture: str | None = None) -> list[str]:
    arguments = ["cmake", "-S", str(Path(source_root).resolve()), "-B", str(Path(build_root).resolve())]
    if generator:
        arguments += ["-G", generator]
        if architecture:
            arguments += ["-A", architecture]
    arguments += [
        "-DAUDIO_BUILD_EDITOR=OFF",
        "-DAUDIO_BUILD_RUNTIME=ON",
        "-DAUDIO_ENABLE_PYTHON=OFF",
        "-DAUDIO_ENABLE_SPEEXDSP=OFF",
        "-DAUDIO_BUILD_TESTS=ON",
        "-DAUDIO_BUILD_INFRASTRUCTURE_TESTS=OFF",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
    ]
    for name, source in (("JUCE", juce_source), ("CATCH2", catch2_source)):
        if source and Path(source).is_dir():
            arguments.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={Path(source).resolve()}")
    return arguments


def dependency_source(main_build_dir: Path, name: str) -> Path | None:
    candidate = Path(main_build_dir) / "_deps" / name
    return candidate.resolve() if candidate.is_dir() else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="main build directory; only hashed, never built into")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--generator", default=None)
    parser.add_argument("--architecture", default=None)
    parser.add_argument("--parallel", type=int, default=12)
    args = parser.parse_args()
    main_build_dir = args.build_dir.resolve()
    if main_build_dir not in [preset.resolve() for preset in MAIN_BUILD_PRESET_DIRS]:
        raise SystemExit(f"--build-dir must be a main build directory, got {main_build_dir}")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    evidence_dir = ROOT / "build" / "reports" / "architecture_refactor" / "ar05d_mutation" / stamp
    evidence_dir.mkdir(parents=True, exist_ok=False)
    # MSBuild's tracked-dependency logs break past MAX_PATH, so the isolated tree uses a short root.
    # The directory is created atomically and uniquely: a parallel configuration must never delete
    # or reuse another run's source copy, build tree or mutation state.
    isolation_root = create_isolation_root(ROOT / "build" / "ar05d")
    if isolation_root.name in {"source", "build"}:
        raise SystemExit(f"unsafe isolation root name: {isolation_root}")

    active_before = workspace_snapshot(ROOT, main_build_dir)
    active_gain_before = active_before["libs/audio_algorithms/Gain.cpp"]
    commands: list[dict[str, object]] = []
    errors: list[str] = []

    def record(label: str, argv: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        try:
            result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", check=False)
            output = (result.stdout or "") + (result.stderr or "")
            code = result.returncode
        except OSError as exc:
            result = subprocess.CompletedProcess(argv, 127, "", str(exc))
            output, code = str(exc), 127
        ended = dt.datetime.now(dt.timezone.utc).isoformat()
        log = evidence_dir / f"{label}.log"
        log.write_text(output, encoding="utf-8")
        commands.append({"label": label, "argv": argv, "cwd": str(cwd),
                         "started_utc": started, "ended_utc": ended,
                         "exit_code": code, "log_path": str(log), "log_sha256": sha_file(log)})
        return result

    source_copy = copy_source_tree(isolation_root / "source")
    copied_gain = assert_isolated(source_copy / "libs" / "audio_algorithms" / "Gain.cpp",
                                  isolation_root, [main_build_dir])
    build_root = isolation_root / "build"
    assert_isolated(build_root, isolation_root, [main_build_dir])
    original = copied_gain.read_bytes()
    if original.count(OLD_ANCHOR) != 1:
        raise SystemExit("Gain kernel mutation anchor was not unique in the isolated copy")
    original_hash = sha_bytes(original)

    configure_arguments = isolated_configure_arguments(
        source_copy, build_root,
        dependency_source(main_build_dir, "juce-src"), dependency_source(main_build_dir, "catch2-src"),
        args.generator, args.architecture)
    configured = record("isolated-configure", [args.cmake, *configure_arguments[1:]], ROOT)
    configure_ok = configured.returncode == 0
    if not configure_ok:
        errors.append(f"isolated configure failed with exit {configured.returncode}")

    def build(label: str) -> bool:
        result = record(label, [args.cmake, "--build", str(build_root), "--config", args.config,
                                "--target", "audio_algorithm_tests", "audioalgo_tests", "audio_cli",
                                "--parallel", str(args.parallel)], ROOT)
        if result.returncode != 0:
            errors.append(f"{label} failed with exit {result.returncode}")
            return False
        return True

    def exercise(phase: str) -> list[dict[str, object]]:
        results = []
        for label, binary, test_filter in ORACLES:
            executable = build_root / "bin" / binary
            result = record(f"{phase}-{label}", [str(executable), test_filter], build_root)
            output = result.stdout + result.stderr
            executed = (f'Filters: "{test_filter}"' in output and
                        "test case" in output.lower() and "assertions" in output.lower())
            caught = result.returncode != 0 and "failed" in output.lower()
            passed = result.returncode == 0 and executed
            expected = caught if phase == "mutated" else passed
            if not expected:
                errors.append(f"{label} oracle did not "
                              f"{'catch the mutation' if phase == 'mutated' else 'pass in the isolated build'}; "
                              f"exit={result.returncode}")
            results.append({"oracle": label, "test_filter": test_filter, "executable": str(executable),
                            "exit_code": result.returncode, "executed": executed,
                            "caught_mutation": caught, "baseline_pass": passed})
        return results

    baseline_results: list[dict[str, object]] = []
    mutation_results: list[dict[str, object]] = []
    restored_results: list[dict[str, object]] = []
    mutated_hash = None
    if configure_ok and build("isolated-baseline-build"):
        baseline_results = exercise("baseline")
        try:
            copied_gain.write_bytes(original.replace(OLD_ANCHOR, MUTATED_ANCHOR))
            mutated_hash = sha_file(copied_gain)
            if mutated_hash == original_hash:
                errors.append("mutated copy hash unexpectedly equals the original")
            if build("isolated-mutated-build"):
                mutation_results = exercise("mutated")
        finally:
            copied_gain.write_bytes(original)
        restored_hash = sha_file(copied_gain)
        if restored_hash != original_hash:
            errors.append("the isolated Gain.cpp copy was not restored byte-for-byte")
        if build("isolated-restored-build"):
            restored_results = exercise("restored")

    active_after = workspace_snapshot(ROOT, main_build_dir)
    if active_after != active_before:
        errors.append("the active Gain.cpp or a main build binary changed during the mutation cycle")
    if sha_file(ACTIVE_GAIN) != active_gain_before:
        errors.append("the active working-tree Gain.cpp is not byte-identical to its pre-run hash")

    report = {
        "schema_version": 1, "case": "AR-05.d",
        "status": "passed" if (not errors and configure_ok and len(baseline_results) == len(ORACLES)
                               and len(mutation_results) == len(ORACLES)
                               and len(restored_results) == len(ORACLES)) else "failed",
        "isolation_root": str(isolation_root.resolve()),
        "isolated_source_root": str(source_copy.resolve()),
        "isolated_build_root": str(build_root.resolve()),
        "main_build_dir_read_only": str(main_build_dir),
        "active_source_sha256_before": active_gain_before,
        "active_source_sha256_after": active_after["libs/audio_algorithms/Gain.cpp"],
        "active_binaries_before": active_before,
        "active_binaries_after": active_after,
        "active_tree_unchanged": active_after == active_before,
        "isolated_original_sha256": original_hash,
        "isolated_mutated_sha256": mutated_hash,
        "commands": commands,
        "baseline_oracles": baseline_results,
        "mutation_oracles": mutation_results,
        "restored_oracles": restored_results,
        "errors": errors,
    }
    report_path = evidence_dir / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(report_path),
                      "active_tree_unchanged": report["active_tree_unchanged"],
                      "errors": errors}, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
