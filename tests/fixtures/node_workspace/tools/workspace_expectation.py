#!/usr/bin/env python3
"""Independent W0 expectations for the custom node workspace contract.

This tool is deliberately *not* the product implementation. It reparses every
frozen fixture with the Python standard library only, reimplements the ADR0040
rules (canonical JSON, runtime whitelist, manifest text, content hash) and
writes/verifies ``expected/workspace_lock.json``.

    python tests/fixtures/node_workspace/tools/workspace_expectation.py --write
    python tests/fixtures/node_workspace/tools/workspace_expectation.py --check

``--check`` recomputes everything from the fixture bytes and fails if the lock
file, the fixtures or the legacy draft drift apart. It never imports a product
module and never links the C++ library under test.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
FIXTURES = ROOT / "tests" / "fixtures" / "node_workspace"
LOCK = FIXTURES / "expected" / "workspace_lock.json"
LEGACY = FIXTURES / "legacy" / "print_a.aadraft"

ALGORITHM = "AudioAlgoNodeWorkspace/1"
MAX_NODE_JSON_BYTES = 1024 * 1024
MAX_SOURCE_FILES = 128
MAX_SOURCE_BYTES = 8 * 1024 * 1024 - 16 * 1024
MAX_RESOURCE_FILES = 64
MAX_RESOURCE_BYTES = 64 * 1024 * 1024
MAX_RELATIVE_PATH_BYTES = 512

EXCLUDED_SEGMENTS = {
    "tests", ".git", ".vscode", ".idea", "__pycache__", "build", ".build",
    "staging", "dist", "out", "node_modules", ".pytest_cache", ".mypy_cache",
}


# --------------------------------------------------------------------------
# Token preserving JSON reader/writer (frozen canonical encoding, ADR0040 §3)
# --------------------------------------------------------------------------
class RawNumber(str):
    """A number token kept exactly as written in the source document."""


class JsonInvalid(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise JsonInvalid("workspace_json_duplicate_key", f"duplicate object key {key!r}")
        result[key] = value
    return result


def _reject_constant(token: str) -> object:
    raise JsonInvalid("workspace_json_invalid", f"non-finite number {token} is not valid JSON")


def parse_json(text: str) -> object:
    if text.startswith("\ufeff"):
        raise JsonInvalid("workspace_json_invalid", "UTF-8 BOM is not accepted")
    try:
        return json.loads(
            text,
            object_pairs_hook=_pairs,
            parse_int=RawNumber,
            parse_float=RawNumber,
            parse_constant=_reject_constant,
        )
    except JsonInvalid:
        raise
    except ValueError as error:
        raise JsonInvalid("workspace_json_invalid", str(error)) from error


_ESCAPES = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def _quote(text: str) -> str:
    out = ['"']
    for char in text:
        if char in _ESCAPES:
            out.append(_ESCAPES[char])
        elif ord(char) < 0x20:
            out.append(f"\\u{ord(char):04x}")
        else:
            out.append(char)
    out.append('"')
    return "".join(out)


def dump_json(value: object) -> str:
    if isinstance(value, RawNumber):
        return str(value)
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, list):
        return "[" + ",".join(dump_json(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(_quote(key) + ":" + dump_json(item) for key, item in value.items()) + "}"
    raise TypeError(f"unsupported canonical JSON value {type(value)!r}")


# --------------------------------------------------------------------------
# Legacy .aadraft (ADR0036) read/write, independent of the product code
# --------------------------------------------------------------------------
DRAFT_MAGIC = b"AudioAlgoDraft/1\n"


def encode_draft(draft: dict) -> bytes:
    out = bytearray(DRAFT_MAGIC)
    for field in ("id", "generation", "language", "declaration", "source"):
        value = draft[field]
        raw = str(value).encode("utf-8") if field == "generation" else value.encode("utf-8")
        out += str(len(raw)).encode("ascii") + b"\n" + raw
    return bytes(out)


def decode_draft(payload: bytes) -> dict:
    if not payload.startswith(DRAFT_MAGIC):
        raise ValueError("not an AudioAlgoDraft/1 document")
    offset = len(DRAFT_MAGIC)
    fields = []
    for _ in range(5):
        end = payload.find(b"\n", offset)
        if end < 0:
            raise ValueError("truncated draft")
        size = int(payload[offset:end])
        offset = end + 1
        fields.append(payload[offset:offset + size])
        offset += size
    if offset != len(payload):
        raise ValueError("trailing draft bytes")
    return {
        "id": fields[0].decode("utf-8"),
        "generation": int(fields[1]),
        "language": fields[2].decode("utf-8"),
        "declaration": fields[3].decode("utf-8"),
        "source": fields[4].decode("utf-8"),
    }


# --------------------------------------------------------------------------
# Workspace rules (ADR0040 §2/§4/§5/§6/§7)
# --------------------------------------------------------------------------
def normalize_relative(path: str) -> str:
    """Return the frozen normalized relative path or raise workspace_path_invalid."""
    if not path or len(path.encode("utf-8")) > MAX_RELATIVE_PATH_BYTES:
        raise JsonInvalid("workspace_path_invalid", f"relative path length/empty: {path!r}")
    if "\\" in path:
        raise JsonInvalid("workspace_path_invalid", f"backslash separator: {path!r}")
    if path.startswith("/") or path.startswith("./"):
        raise JsonInvalid("workspace_path_invalid", f"rooted path: {path!r}")
    parts = path.split("/")
    for part in parts:
        if part in ("", ".", ".."):
            raise JsonInvalid("workspace_path_invalid", f"empty or dot segment: {path!r}")
        if part.endswith(".") or part.endswith(" "):
            raise JsonInvalid("workspace_path_invalid", f"trailing dot/space segment: {path!r}")
        if ":" in part:
            raise JsonInvalid("workspace_path_invalid", f"drive or ADS segment: {path!r}")
    if len(parts) == 1 and (len(parts[0]) >= 2 and parts[0][1] == ":"):
        raise JsonInvalid("workspace_path_invalid", f"drive path: {path!r}")
    return path


def excluded_from_runtime(relative: str) -> bool:
    parts = relative.split("/")
    if any(part in EXCLUDED_SEGMENTS for part in parts[:-1]):
        return True
    if any(part.startswith(".") for part in parts):
        return True
    if parts[-1].endswith((".pyc", ".pyo")):
        return True
    return False


def is_case_conflict(paths: list[str]) -> bool:
    seen: dict[str, str] = {}
    for path in paths:
        key = path.casefold()
        if key in seen and seen[key] != path:
            return True
        seen[key] = path
    return False


def validate_node_json(text: str, root: Path | None) -> dict:
    """Validate a node.json document; return the parsed document.

    Validation order is frozen as json -> schema -> paths -> existence ->
    budget -> manifest (invalid/expectations.json).
    """
    document = parse_json(text)
    if not isinstance(document, dict):
        raise JsonInvalid("workspace_contract_invalid", "node.json must be an object")
    schema = document.get("workspace_schema")
    if not isinstance(schema, RawNumber) or str(schema) != "1":
        raise JsonInvalid("workspace_schema_unsupported", f"workspace_schema {schema!r} is not 1")
    language = document.get("language")
    if language not in ("python", "cpp"):
        raise JsonInvalid("workspace_language_unsupported", f"language {language!r} is not python/cpp")
    node = document.get("node")
    if not isinstance(node, dict) or not isinstance(node.get("type_id"), str) or not node["type_id"]:
        raise JsonInvalid("workspace_contract_invalid", "node must be an object with a type_id")

    declared: list[str] = []
    if language == "python":
        section = document.get("python")
        if not isinstance(section, dict):
            raise JsonInvalid("workspace_contract_invalid", "python section is required")
        if section.get("source_root") != "src":
            raise JsonInvalid("workspace_contract_invalid", "python.source_root must be src")
        entrypoint = section.get("entrypoint")
        if not isinstance(entrypoint, str) or ":" not in entrypoint:
            raise JsonInvalid("workspace_contract_invalid", "python.entrypoint must be module:function")
        module, function = entrypoint.split(":", 1)
        if not module or not function:
            raise JsonInvalid("workspace_contract_invalid", "python.entrypoint must be module:function")
        entry_path = "src/" + module.replace(".", "/") + ".py"
        try:
            normalize_relative(entry_path)
        except JsonInvalid as error:
            raise JsonInvalid("workspace_entrypoint_invalid", f"{entrypoint!r}: {error}") from error
        if ".." in module.split(".") or module.startswith(".") or "\\" in module:
            raise JsonInvalid("workspace_entrypoint_invalid", f"{entrypoint!r} escapes src/")
        declared.append(entry_path)
    else:
        section = document.get("native")
        if not isinstance(section, dict):
            raise JsonInvalid("workspace_contract_invalid", "native section is required")
        sources = section.get("sources")
        include_dirs = section.get("include_dirs", [])
        if not isinstance(sources, list) or not sources or not all(isinstance(item, str) for item in sources):
            raise JsonInvalid("workspace_contract_invalid", "native.sources must be a non-empty string array")
        if not isinstance(include_dirs, list) or not all(isinstance(item, str) for item in include_dirs):
            raise JsonInvalid("workspace_contract_invalid", "native.include_dirs must be a string array")
        declared.extend(sources)
        for directory in include_dirs:
            normalize_relative(directory)

    resources = document.get("resources", [])
    if not isinstance(resources, list) or not all(isinstance(item, str) for item in resources):
        raise JsonInvalid("workspace_contract_invalid", "resources must be a string array")
    resolved = [normalize_relative(entry) for entry in declared + resources]
    if is_case_conflict(resolved):
        raise JsonInvalid("workspace_path_case_conflict", "declared paths differ only by case")

    if root is not None:
        for relative in declared:
            if not (root / relative).is_file():
                raise JsonInvalid("workspace_source_missing", f"declared file is missing: {relative}")
        for relative in resources:
            if not (root / relative).is_file():
                raise JsonInvalid("workspace_resource_missing", f"declared resource is missing: {relative}")
    return document


def runtime_files(root: Path, document: dict) -> list[str]:
    language = document["language"]
    resources = document.get("resources", [])
    selected: list[str] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix().encode("utf-8")):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if excluded_from_runtime(relative):
            continue
        first = relative.split("/")[0]
        if relative == "node.json" or first in ("src", "include") or relative in resources:
            selected.append(relative)
    if "node.json" not in selected:  # pragma: no cover - node.json is mandatory
        raise JsonInvalid("workspace_contract_invalid", "node.json is missing from the runtime set")
    if language == "python":
        entrypoint = document["python"]["entrypoint"]
        entry_path = "src/" + entrypoint.split(":", 1)[0].replace(".", "/") + ".py"
        if entry_path not in selected:
            raise JsonInvalid("workspace_source_missing", f"entrypoint file is missing: {entry_path}")
    return selected


def file_manifest(root: Path, relative_paths: list[str]) -> list[dict]:
    manifest = []
    for relative in sorted(relative_paths, key=lambda item: item.encode("utf-8")):
        payload = (root / relative).read_bytes()
        manifest.append({
            "path": relative,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    return manifest


def manifest_text(manifest: list[dict]) -> bytes:
    lines = [ALGORITHM]
    for entry in manifest:
        lines.append(entry["path"])
        lines.append(str(entry["bytes"]))
        lines.append(entry["sha256"])
    return ("\n".join(lines) + "\n").encode("utf-8")


def content_sha256(manifest: list[dict]) -> str:
    return hashlib.sha256(manifest_text(manifest)).hexdigest()


def enforce_budget(root: Path, document: dict, manifest: list[dict], runtime: list[str]) -> None:
    node_bytes = (root / "node.json").stat().st_size
    if node_bytes > MAX_NODE_JSON_BYTES:
        raise JsonInvalid("workspace_budget_exceeded", f"node.json {node_bytes} bytes")
    sources = [entry for entry in manifest if entry["path"] in runtime and
               (entry["path"] == "node.json" or entry["path"].split("/")[0] in ("src", "include"))]
    if len(sources) > MAX_SOURCE_FILES:
        raise JsonInvalid("workspace_budget_exceeded", f"{len(sources)} source files")
    if sum(entry["bytes"] for entry in sources) > MAX_SOURCE_BYTES:
        raise JsonInvalid("workspace_budget_exceeded", "source bytes exceed the frozen budget")
    resources = set(document.get("resources", []))
    resource_entries = [entry for entry in manifest if entry["path"] in resources]
    if len(resource_entries) > MAX_RESOURCE_FILES:
        raise JsonInvalid("workspace_budget_exceeded", "resource file count")
    if sum(entry["bytes"] for entry in resource_entries) > MAX_RESOURCE_BYTES:
        raise JsonInvalid("workspace_budget_exceeded", "resource bytes")


def workspace_expectation(root: Path) -> dict:
    document = validate_node_json((root / "node.json").read_text(encoding="utf-8"), root)
    runtime = runtime_files(root, document)
    manifest = file_manifest(root, runtime)
    enforce_budget(root, document, manifest, runtime)
    excluded = sorted(
        (path.relative_to(root).as_posix() for path in root.rglob("*")
         if path.is_file() and path.relative_to(root).as_posix() not in runtime),
        key=lambda item: item.encode("utf-8"),
    )
    declaration = dump_json(document["node"])
    expectation = {
        "language": document["language"],
        "type_id": document["node"]["type_id"],
        "display_name": document["node"].get("display_name", ""),
        "declaration": declaration,
        "declaration_sha256": hashlib.sha256(declaration.encode("utf-8")).hexdigest(),
        "node_json_sha256": hashlib.sha256((root / "node.json").read_bytes()).hexdigest(),
        "runtime_files": manifest,
        "content_sha256": content_sha256(manifest),
        "excluded_present": excluded,
    }
    if document["language"] == "python":
        entrypoint = document["python"]["entrypoint"]
        entry_path = "src/" + entrypoint.split(":", 1)[0].replace(".", "/") + ".py"
        payload = (root / entry_path).read_bytes()
        expectation.update({
            "entrypoint": entrypoint,
            "entry_source_path": entry_path,
            "entry_source_sha256": hashlib.sha256(payload).hexdigest(),
        })
    else:
        expectation.update({
            "sources": document["native"]["sources"],
            "include_dirs": document["native"].get("include_dirs", []),
        })
    return expectation


# --------------------------------------------------------------------------
# Legacy draft fixture + expected conversion (ADR0040 §8)
# --------------------------------------------------------------------------
LEGACY_DECLARATION = {
    "type_id": "user.print_a",
    "version": "1.0.0",
    "display_name": "Print A",
    "execution": {"kind": "block", "stateful": False, "realtime_verified": False},
    "inputs": [
        {"id": "primary", "type": "audio.pcm@1", "required": True},
        {"id": "reference", "type": "audio.pcm@1", "required": True},
    ],
    "outputs": [{"id": "main", "type": "audio.pcm@1"}],
    "format": {"sample_rates_hz": [16000, 48000], "channels": [1, 2, 4]},
    "parameters": [{"id": "gain", "type": "float", "default": 0.5, "minimum": -2, "maximum": 2}],
}
LEGACY_SOURCE = '''"""Single-file legacy draft for the frozen NWS conversion fixture."""

import numpy as np


def process(inputs, params, state):
    primary = inputs["primary"]
    reference = inputs["reference"]
    return {"main": (np.float32(0.5) * primary + np.float32(-0.25) * reference) * np.float32(params["gain"])}
'''
LEGACY_DRAFT = {
    "id": "print-a",
    "generation": 3,
    "language": "python",
    "declaration": json.dumps(LEGACY_DECLARATION, indent=2, ensure_ascii=False),
    "source": LEGACY_SOURCE,
}


def legacy_bytes() -> bytes:
    return encode_draft(LEGACY_DRAFT)


def conversion_expectation() -> dict:
    """Expected result of converting the legacy draft into a workspace directory.

    The module name is the draft id normalized to lower case with '-' -> '_'
    (ADR0040 §8). The entry file must carry the draft source byte for byte.
    """
    module = LEGACY_DRAFT["id"].lower().replace("-", "_")
    declaration = parse_json(LEGACY_DRAFT["declaration"])
    node_json = {
        "workspace_schema": RawNumber("1"),
        "language": "python",
        "node": declaration,
        "python": {"source_root": "src", "entrypoint": f"{module}.process:process"},
        "resources": [],
    }
    files = [
        ("node.json", dump_json(node_json).encode("utf-8")),
        (f"src/{module}/__init__.py", b""),
        (f"src/{module}/process.py", LEGACY_DRAFT["source"].encode("utf-8")),
    ]
    manifest = [
        {"path": path, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
        for path, payload in sorted(files, key=lambda item: item[0].encode("utf-8"))
    ]
    return {
        "draft": "legacy/print_a.aadraft",
        "module": module,
        "entrypoint": f"{module}.process:process",
        "declaration": dump_json(declaration),
        "node_json": dump_json(node_json),
        "node_json_sha256": hashlib.sha256(dump_json(node_json).encode("utf-8")).hexdigest(),
        "runtime_files": manifest,
        "content_sha256": content_sha256(manifest),
    }


# --------------------------------------------------------------------------
# Negative fixture classification (independent of the product)
# --------------------------------------------------------------------------
def classify(fixture: Path) -> str:
    try:
        text = fixture.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return "workspace_json_invalid"
    try:
        validate_node_json(text, None)
    except JsonInvalid as error:
        return error.code
    return "accepted"


def build_lock() -> dict:
    workspaces = {}
    for name in ("python_two_files", "python_two_files_v2", "native_two_sources", "preserve_unknown"):
        workspaces[name] = workspace_expectation(FIXTURES / name)
    expectations = json.loads((FIXTURES / "invalid" / "expectations.json").read_text(encoding="utf-8"))
    negatives = []
    for case in expectations["cases"]:
        fixture = FIXTURES / case["fixture"]
        observed = classify(fixture)
        if observed != case["expected_code"]:
            raise SystemExit(f"{case['fixture']}: independent classifier gave {observed}, "
                             f"expectations.json documents {case['expected_code']}")
        negatives.append({**case, "observed_code": observed})
    for case in expectations["positive"]:
        fixture = FIXTURES / case["fixture"]
        observed = classify(fixture)
        if observed != "accepted":
            raise SystemExit(f"{case['fixture']}: expected accepted, independent classifier gave {observed}")
    payload = legacy_bytes()
    draft = decode_draft(payload)
    return {
        "lock_schema": 1,
        "algorithm": ALGORITHM,
        "generator": "tests/fixtures/node_workspace/tools/workspace_expectation.py",
        "adr": "doc/decisions/0040-node-workspace-schema-v1.md",
        "budgets": {
            "node_json_bytes": MAX_NODE_JSON_BYTES,
            "source_files": MAX_SOURCE_FILES,
            "source_bytes": MAX_SOURCE_BYTES,
            "resource_files": MAX_RESOURCE_FILES,
            "resource_bytes": MAX_RESOURCE_BYTES,
            "relative_path_bytes": MAX_RELATIVE_PATH_BYTES,
        },
        "workspaces": workspaces,
        "legacy": {
            "path": "legacy/print_a.aadraft",
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "id": draft["id"],
            "generation": draft["generation"],
            "language": draft["language"],
            "declaration_sha256": hashlib.sha256(draft["declaration"].encode("utf-8")).hexdigest(),
            "source_sha256": hashlib.sha256(draft["source"].encode("utf-8")).hexdigest(),
            "declaration": dump_json(parse_json(draft["declaration"])),
        },
        "conversion": conversion_expectation(),
        "invalid": negatives,
        "positive": expectations["positive"],
    }


def write_lock() -> int:
    LEGACY.parent.mkdir(parents=True, exist_ok=True)
    LEGACY.write_bytes(legacy_bytes())
    lock = build_lock()
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    LOCK.write_text(json.dumps(lock, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {LOCK.relative_to(ROOT)} and {LEGACY.relative_to(ROOT)}")
    print(f"workspaces={len(lock['workspaces'])} invalid={len(lock['invalid'])} "
          f"legacy_sha256={lock['legacy']['sha256'][:16]}...")
    return 0


def check() -> int:
    problems: list[str] = []
    if not LOCK.is_file():
        print(f"missing lock file {LOCK}", file=sys.stderr)
        return 1
    locked = json.loads(LOCK.read_text(encoding="utf-8"))
    expected = build_lock()
    if locked != expected:
        for key in sorted(set(locked) | set(expected)):
            if locked.get(key) != expected.get(key):
                problems.append(f"lock section {key!r} does not match the recomputed expectation")
    if not LEGACY.is_file() or LEGACY.read_bytes() != legacy_bytes():
        problems.append("legacy/print_a.aadraft does not match the independent draft encoding")
    # The lock is the expectation; recompute every fixture byte hash from disk so a
    # modified fixture cannot silently keep an old expectation.
    for name, workspace in expected["workspaces"].items():
        root = FIXTURES / name
        for entry in workspace["runtime_files"]:
            path = root / entry["path"]
            if not path.is_file():
                problems.append(f"{name}: missing runtime file {entry['path']}")
                continue
            payload = path.read_bytes()
            if hashlib.sha256(payload).hexdigest() != entry["sha256"] or len(payload) != entry["bytes"]:
                problems.append(f"{name}: runtime bytes changed for {entry['path']}")
    if problems:
        print("W0 workspace contract check FAILED", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(f"W0 workspace contract OK: {len(expected['workspaces'])} workspaces, "
          f"{sum(len(item['runtime_files']) for item in expected['workspaces'].values())} runtime files, "
          f"{len(expected['invalid'])} invalid fixtures, legacy sha256={expected['legacy']['sha256'][:16]}")
    return 0


def verify_workspace(directory: Path, draft_path: Path | None) -> int:
    """Independent read of a real workspace directory (used after a W1 run)."""
    expectation = workspace_expectation(directory)
    report = {"workspace": str(directory), "expectation": expectation, "draft": None, "matches": {}}
    if draft_path is not None:
        payload = draft_path.read_bytes()
        draft = decode_draft(payload)
        declaration = dump_json(parse_json(draft["declaration"]))
        report["draft"] = {
            "path": str(draft_path),
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "id": draft["id"],
            "generation": draft["generation"],
            "language": draft["language"],
            "declaration_sha256": hashlib.sha256(declaration.encode("utf-8")).hexdigest(),
            "source_sha256": hashlib.sha256(draft["source"].encode("utf-8")).hexdigest(),
        }
        report["matches"] = {
            "declaration": expectation["declaration"] == declaration,
            "language": expectation["language"] == draft["language"],
            "entry_source": expectation["entry_source_sha256"] == report["draft"]["source_sha256"],
        }
    print(json.dumps(report, indent=2, ensure_ascii=False))
    if draft_path is not None and not all(report["matches"].values()):
        print("independent workspace/draft comparison FAILED", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--verify", type=Path, help="workspace directory to read back independently")
    parser.add_argument("--draft", type=Path, help="legacy .aadraft the workspace must match")
    arguments = parser.parse_args()
    if arguments.verify is not None:
        return verify_workspace(arguments.verify.resolve(), arguments.draft.resolve() if arguments.draft else None)
    return write_lock() if arguments.write else check()


if __name__ == "__main__":
    raise SystemExit(main())
