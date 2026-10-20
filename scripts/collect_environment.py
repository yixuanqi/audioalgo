"""Collect local build facts without guessing at unavailable GUI/device proof."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import os
import platform
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build" / "reports" / "environment.json"
PYTHON = Path(r"E:\0qyx\res\miniconda\python.exe")
CL = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe")
VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


def capture(command: list[str]) -> dict:
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False, timeout=30)
        return {"command": command, "exit_code": result.returncode, "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except Exception as exc:
        return {"command": command, "error": f"{type(exc).__name__}: {exc}"}


def version_of(module_name: str) -> str | None:
    if importlib.util.find_spec(module_name) is None:
        return None
    module = __import__(module_name)
    return getattr(module, "__version__", "installed; version attribute unavailable")


def current_source_tree_hash() -> str:
    spec = importlib.util.spec_from_file_location(
        "audioalgo_verify_editor_interaction", ROOT / "scripts" / "verify_editor_interaction.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the source tree hash implementation")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.source_tree_hash()


def latest_loop_log(prefix: str, fallback: Path) -> str:
    candidates = list((ROOT / "build" / "reports" / "loops").glob(f"NI*/{prefix}*.log"))
    if not candidates:
        return str(fallback)
    return str(max(candidates, key=lambda path: path.stat().st_mtime))


def main() -> None:
    py_version = capture([str(PYTHON), "--version"])
    py_info = capture([str(PYTHON), "-c", "import sys,platform; print(sys.executable); print(sys.version); print(platform.platform())"])
    cl_info = capture([str(CL), "/Bv"])
    vs_info = capture([str(VSWHERE), "-all", "-products", "*", "-format", "json"])
    git_info = capture(["git", "rev-parse", "HEAD"])

    cache_candidates = {
        "JUCE": [ROOT / "build" / "windows-debug" / "_deps" / "juce-src", Path(r"E:\0qyx\res\vcpkg\installed\x64-windows\include\juce")],
        "Catch2": [ROOT / "build" / "windows-headless" / "_deps" / "catch2-src", Path(r"E:\0qyx\res\vcpkg\installed\x64-windows\include\catch2")],
        "SpeexDSP": [Path(r"E:\0qyx\res\vcpkg\installed\x64-windows\include\speex")],
    }
    dep_cache = {name: [str(path) for path in candidates if path.exists()] for name, candidates in cache_candidates.items()}

    manifest = {
        "schema_version": 1,
        "scope": "toolchain and dependency inventory; not product Gate acceptance",
        "collected_local": dt.datetime.now().astimezone().isoformat(),
        "workspace": str(ROOT),
        "revision": None if git_info["exit_code"] else git_info["stdout"],
        "source_tree_hash": current_source_tree_hash(),
        "platform": {"system": platform.platform(), "architecture": platform.machine(), "cpu_count": os.cpu_count()},
        "toolchain": {
            "cmake": capture([shutil.which("cmake") or "cmake", "--version"]),
            "ctest": capture([shutil.which("ctest") or "ctest", "--version"]),
            "ninja": capture([shutil.which("ninja") or "ninja", "--version"]),
            "python3": {"path": str(PYTHON), "version": py_version, "details": py_info},
            "python_packages": {name: version_of(name) for name in ("pytest", "numpy", "scipy", "pyroomacoustics")},
            "msvc": {"path": str(CL), "details": cl_info},
            "visual_studio_installations": vs_info,
        },
        "dependency_cache": dep_cache,
        "desktop_gui": "not_verified; editor launch succeeded, but the computer-use screenshot capture timed out twice",
        "audio_device": "not_verified; no device playback test has run",
    }
    editor_exe = ROOT / "build" / "windows-debug" / "bin" / "AudioAlgo Editor.exe"
    cli_exe = ROOT / "build" / "windows-debug" / "bin" / "audio_cli.exe"
    manifest["debug_artifacts"] = {
        "editor": {"path": str(editor_exe), "sha256": hashlib.sha256(editor_exe.read_bytes()).hexdigest()} if editor_exe.is_file() else None,
        "cli": {"path": str(cli_exe), "sha256": hashlib.sha256(cli_exe.read_bytes()).hexdigest()} if cli_exe.is_file() else None,
        "debug_ctest_log": latest_loop_log("debug-ctest", ROOT / "build" / "reports" / "loops" / "NI0000" / "debug-ctest-final-pass.log"),
        "headless_ctest_log": latest_loop_log("headless-ctest", ROOT / "build" / "reports" / "loops" / "NI0000" / "headless-ctest-final-pass.log"),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(OUTPUT)


if __name__ == "__main__":
    main()
