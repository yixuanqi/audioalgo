"""Independent saved-package relocation, conflict preservation and GUI/CLI audio oracle."""
import argparse
import datetime as dt
import hashlib
import json
import math
import struct
from pathlib import Path
from inspect_authoring_probe import filesystem, sha

parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("root", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
record = json.loads(args.command_record.read_text(encoding="utf-8"))
assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "original-project.bytes").is_file()
         and start <= (p / "original-project.bytes").stat().st_mtime <= end]
assert len(roots) == 2, roots
evidence = []
for p in roots:
    root = filesystem(p)
    baseline = json.loads((root / "original-project.bytes").read_text(encoding="utf-8"))
    lock = baseline["nodes"][1]["managed_version"]
    if (root / "original").exists():
        assert (root / "original/project.aaproj").read_bytes() == (root / "original-project.bytes").read_bytes()
        target = root / "original" / lock["package_path"]
        assert (target / "user_source.py").read_bytes() == b"user owned bytes must not be replaced"
        assert not (target / "snapshot.json").exists()
        evidence.append({"root": str(p), "conflict_original_bytes_preserved": True})
        continue
    assert not (root / "authoring").exists()
    moved, = [x for x in root.iterdir() if (x / "project.aaproj").is_file()]
    assert " " in moved.name and any(ord(c) > 127 for c in moved.name)
    project = json.loads((moved / "project.aaproj").read_text(encoding="utf-8"))
    assert project["nodes"][1]["managed_version"] == lock
    package = moved / lock["package_path"]
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(metadata["files"].items()))
    assert hashlib.sha256(signature.encode()).hexdigest() == lock["content_sha256"]
    original = root / "unavailable-authoring/packages" / lock["package_id"] / lock["content_sha256"]
    for name, digest in metadata["files"].items():
        assert sha(package / name) == sha(original / name) == digest
    assert not (root / "empty-authoring/packages").exists()
    gui_runs = [x for x in (moved / "runs").iterdir() if (x / "complete.json").is_file()]
    assert len(gui_runs) == 1
    runs = gui_runs + [moved / "cli"]
    output_bytes, outputs = [], []
    for run in runs:
        raw = (run / "output.wav").read_bytes(); output_bytes.append(raw)
        assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
        chunks, offset = {}, 12
        while offset + 8 <= len(raw):
            kind, size = struct.unpack_from("<4sI", raw, offset)
            chunks[kind] = raw[offset + 8:offset + 8 + size]; offset += 8 + size + size % 2
        tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
        assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
        samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
        assert len(samples) == 513
        maximum = 0
        for n, value in enumerate(samples):
            expected = .1 * math.sin(.03 * (n + 1))
            assert math.isfinite(value) and abs(value - expected) <= 2e-6 + 1e-5 * abs(expected)
            maximum = max(maximum, abs(value - expected))
        manifest = json.loads((run / "run.json").read_text(encoding="utf-8"))
        resource, = manifest["resources"]
        assert sha(run / resource["snapshot_path"]) == sha(moved / "input.wav") == resource["sha256"] == resource["snapshot_sha256"]
        executed = []
        for result_path in run.rglob("result.json"):
            result = json.loads(result_path.read_text(encoding="utf-8"))
            if result.get("code_files") != metadata["files"]: continue
            for name, digest in metadata["files"].items():
                assert sha(result_path.parent.parent / result["code_snapshot"] / name) == digest
            executed.append(str(result_path))
        assert len(executed) == 1
        outputs.append({"run": str(run), "wav_sha256": sha(run / "output.wav"), "max_abs_error": maximum, "executed": executed})
    assert output_bytes[0] == output_bytes[1]
    evidence.append({"root": str(p), "exact_lock": lock, "gui_cli_identical": True, "outputs": outputs})
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "evidence": evidence}, stream, indent=2)
print("Relocated Chinese/space project uses carried exact code in GUI and CLI; audio/resource hashes and conflict bytes verified")
