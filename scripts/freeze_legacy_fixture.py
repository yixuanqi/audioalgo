"""Freeze the existing gain demo as a read-only R0 regression fixture."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples" / "gain_demo"
DEST = ROOT / "build" / "reports" / "loops" / "RF0000" / "legacy_fixture"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if DEST.exists():
        raise SystemExit(f"refusing to overwrite frozen fixture: {DEST}")
    DEST.mkdir(parents=True)
    for name in ("gain_demo.aaproj", "input.wav"):
        (DEST / name).write_bytes((SOURCE / name).read_bytes())
    project_path = DEST / "gain_demo.aaproj"
    project = json.loads(project_path.read_text(encoding="utf-8"))
    manifest = {
        "schema_version": 1,
        "source_project": "examples/gain_demo/gain_demo.aaproj",
        "source_input": "examples/gain_demo/input.wav",
        "frozen_files": {name: {"sha256": sha(DEST / name), "bytes": (DEST / name).stat().st_size}
                         for name in ("gain_demo.aaproj", "input.wav")},
        "project_id": project["project_id"],
        "revision": project["revision"],
        "nodes": project["nodes"],
        "connections": project["connections"],
        "selected_output_node_id": project["selected_output_node_id"],
        "oracle": {"formula": "y = float64(x) * pow(10, gain_db / 20)",
                   "gain_db": project["nodes"][1]["parameters"]["gain_db"],
                   "tolerance": "abs(actual-expected) <= 2e-6 + 1e-5*abs(expected)"},
    }
    (DEST / "fixture.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for name in ("gain_demo.aaproj", "input.wav", "fixture.json"):
        (DEST / name).chmod(os.stat(DEST / name).st_mode & ~0o222)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
