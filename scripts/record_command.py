"""Run one real command and preserve immutable, source-linked local evidence."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest() -> dict[str, str]:
    ignored = {".git", "build", ".venv", "__pycache__", ".pytest_cache", ".audioalgo-test-tmp"}
    paths = []
    for folder, directories, names in os.walk(ROOT, followlinks=False):
        directories[:] = [name for name in directories if name not in ignored]
        paths.extend(Path(folder) / name for name in names if name not in ignored)
    return {p.relative_to(ROOT).as_posix(): sha256(p) for p in sorted(paths) if p.is_file()}


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--artifact", action="append", default=[], help="workspace-relative output to hash after the command")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation prevents overwriting either earlier success or failure.
    log = args.output.with_suffix(".log")
    metadata = args.output.with_suffix(".json")
    if log.exists() or metadata.exists():
        parser.error("evidence output already exists; choose a new name")
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    sources = source_manifest()
    error = None
    with log.open("xb") as stream:
        try:
            result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=args.timeout, check=False)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code, error = 124, "timeout"
        except OSError as exc:
            code, error = 125, str(exc)
            stream.write(str(exc).encode("utf-8"))
    artifacts = []
    for raw in args.artifact:
        path = (ROOT / raw).resolve()
        try:
            path.relative_to(ROOT.resolve())
        except ValueError:
            parser.error("artifact must be inside the workspace")
        artifacts.append({"path": str(path), "sha256": sha256(path)} if path.is_file()
                         else {"path": str(path), "status": "missing"})
    report = {"command": command, "cwd": str(ROOT), "started_utc": started,
              "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "exit_code": code, "error": error, "log_path": str(log.resolve()),
              "log_sha256": sha256(log), "artifacts": artifacts, "sources": sources,
              "source_tree_hash": hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest()}
    metadata.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"exit={code}; evidence={metadata}")
    print(log.read_text(encoding="utf-8", errors="replace")[-5000:])
    return code


if __name__ == "__main__":
    sys.exit(main())
