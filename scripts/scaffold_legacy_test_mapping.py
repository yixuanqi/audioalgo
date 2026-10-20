"""Record every pre-refactor CTest name and its exact current counterpart."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "tests" / "legacy_test_mapping.json"
BASELINES = {
    "debug": ROOT / "build/windows-debug/build/reports/loops/RF0000/baseline-debug-junit.xml",
    "headless": ROOT / "build/windows-headless/build/reports/loops/RF0000/baseline-headless-junit.xml",
}
BUILD_DIRS = {"debug": ROOT / "build/windows-debug", "headless": ROOT / "build/windows-headless"}
CATEGORIES = ("CORE", "DSP", "GRAPH", "IO", "PRJ", "E2E", "NED")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_names(path: Path) -> list[str]:
    return sorted(case.attrib["name"] for case in ET.parse(path).getroot().iter("testcase"))


def record(path: Path, include_artifact_hash: bool = True) -> dict[str, Any]:
    if not path.is_file():
        raise SystemExit(f"required JUnit artifact is missing: {path}")
    names = test_names(path)
    row = {"path": path.relative_to(ROOT).as_posix(),
           "inventory_sha256": hashlib.sha256(json.dumps(names, ensure_ascii=False).encode()).hexdigest(),
           "test_names": names}
    if include_artifact_hash:
        row["sha256"] = sha(path)
    return row


def main() -> None:
    configurations = {}
    for configuration in BASELINES:
        before = record(BASELINES[configuration])
        build_dir = BUILD_DIRS[configuration]
        command = ["ctest", "--test-dir", str(build_dir), "-C", "Debug", "--show-only=json-v1", "--no-tests=error"]
        result = subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        inventory_names = sorted(item["name"] for item in json.loads(result.stdout).get("tests", []))
        after = {"build_dir": build_dir.relative_to(ROOT).as_posix(), "inventory_command": command,
                 "inventory_sha256": hashlib.sha256(json.dumps(inventory_names, ensure_ascii=False).encode()).hexdigest(),
                 "test_names": inventory_names}
        missing = sorted(set(before["test_names"]) - set(after["test_names"]))
        if missing:
            raise SystemExit(f"{configuration} current suite lost baseline tests: {missing}")
        mappings = []
        for old_name in before["test_names"]:
            match = re.match(r"^([A-Z][A-Z0-9]*-\d+)", old_name)
            identifier = match.group(1) if match else None
            category = identifier.split("-", 1)[0] if identifier else "unclassified"
            mappings.append({"legacy_test_name": old_name, "current_test_name": old_name,
                             "requirement_id": identifier, "category": category})
        configurations[configuration] = {
            "legacy_junit": before,
            "current_inventory": after,
            "legacy_test_count": len(before["test_names"]),
            "current_test_count": len(after["test_names"]),
            "mapping": mappings,
        }

    interaction_doc = ROOT / "doc/testing_editor_node_interaction.md"
    interaction_manifest = json.loads((ROOT / "tests/editor_interaction_manifest.json").read_text(encoding="utf-8"))
    if interaction_manifest.get("acceptance_document_sha256") != sha(interaction_doc):
        raise SystemExit("NED manifest acceptance hash does not match its document")
    desktop_required = sorted(
        f"{item['id']}.{letter}"
        for item in interaction_manifest["requirements"]
        for letter in item.get("desktop_subcases", [])
    )
    manifest = {
        "schema_version": 1,
        "purpose": "Preserve old-to-current CTest names and required NED desktop subcases across target splits.",
        "legacy_acceptance_documents": {
            "pipeline": {"path": "doc/testing_audio_pipeline_editor.md",
                         "sha256": sha(ROOT / "doc/testing_audio_pipeline_editor.md")},
            "node_interaction": {"path": "doc/testing_editor_node_interaction.md",
                                 "sha256": sha(interaction_doc)},
        },
        "desktop_required_ned_subcases": desktop_required,
        "configurations": configurations,
    }
    OUTPUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote exact legacy mappings: {OUTPUT}")


if __name__ == "__main__":
    main()
