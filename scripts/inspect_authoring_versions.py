"""Independent RIFF/package/lock oracle for real v1 -> v2 Apply runs."""
import argparse
import datetime as dt
import hashlib
import json
import math
import struct
from pathlib import Path
from inspect_authoring_probe import filesystem, sha


def wav(path, amplitude):
    data = filesystem(path).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    chunks, offset = {}, 12
    while offset + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, offset)
        chunks[kind] = data[offset + 8:offset + 8 + size]
        offset += 8 + size + size % 2
    tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
    assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
    samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
    assert len(samples) == 513
    errors = []
    for n, value in enumerate(samples):
        expected = amplitude * math.sin(.03 * (n + 1))
        error = abs(value - expected)
        assert math.isfinite(value) and error <= 2e-6 + 1e-5 * abs(expected)
        errors.append(error)
    return {"path": str(path), "sha256": sha(filesystem(path)), "max_abs_error": max(errors)}


parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("root", type=Path)
parser.add_argument("--workspace", action="store_true")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
record = json.loads(args.command_record.read_text(encoding="utf-8"))
assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "index.json").is_file()
         and start <= (p / "index.json").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = roots[0]
index = json.loads((root / "index.json").read_text(encoding="utf-8"))
assert index["schema_version"] == 1 and len(index["packages"]) == 1
entry = index["packages"][0]
packages = list((root / "packages" / entry["package_id"]).iterdir())
assert len(packages) == 2
metadata = {}
for path in packages:
    snapshot = json.loads((path / "snapshot.json").read_text(encoding="utf-8"))
    assert snapshot["content_sha256"] == path.name
    signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(snapshot["files"].items()))
    assert hashlib.sha256(signature.encode()).hexdigest() == path.name
    for name, digest in snapshot["files"].items():
        assert sha(path / name) == digest
    metadata[path.name] = snapshot["files"]
v2 = entry["content_sha256"]
assert v2 in metadata
v1, = set(metadata) - {v2}
assert b"* 1.5" in (root / "packages" / entry["package_id"] / v2 / "user_source.py").read_bytes()
assert b"* 1.5" not in (root / "packages" / entry["package_id"] / v1 / "user_source.py").read_bytes()

if args.workspace:
    before, after = root / "before-v2.aaproj", root / "after-v2.aaproj"
    assert before.read_bytes() == after.read_bytes() == (root / "project.aaproj").read_bytes()
    project = json.loads(before.read_text(encoding="utf-8"))
    locks = [n["managed_version"] for n in project["nodes"] if "managed_version" in n]
    assert len(locks) == 1 and locks[0]["content_sha256"] == v1
    outputs = [p / "output.wav" for p in (root / "runs").iterdir() if (p / "output.wav").is_file()]
    assert len(outputs) == 2
    values = [wav(p, .16) for p in outputs]
    assert outputs[0].read_bytes() == outputs[1].read_bytes()
    expected_runs = [(p.parent, v1) for p in outputs]
else:
    values = [wav(root / name / "output.wav", amplitude) for name, amplitude in
              [("v1-before", .1), ("v2", .15), ("v1-after", .1)]]
    assert (root / "v1-before/output.wav").read_bytes() == (root / "v1-after/output.wav").read_bytes()
    expected_runs = [(root / name, version) for name, version in [("v1-before", v1), ("v2", v2), ("v1-after", v1)]]
    for run, version in expected_runs:
        project = json.loads((root / (run.name + ".aaproj")).read_text(encoding="utf-8"))
        assert [n["managed_version"]["content_sha256"] for n in project["nodes"] if "managed_version" in n] == [version]

executed = []
for run, version in expected_runs:
    matches = []
    for result_path in filesystem(run).rglob("result.json"):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result.get("code_files") != metadata[version]:
            continue
        code = result_path.parent.parent / result["code_snapshot"]
        for name, digest in metadata[version].items():
            assert sha(code / name) == digest
        matches.append(str(result_path))
    assert matches, (run, version)
    executed.append({"run": str(run), "exact_hash": version, "results": matches})
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "root": str(root),
               "v1": v1, "v2_default": v2, "outputs": values, "executed": executed}, stream, indent=2)
print("Immutable v1/v2 bytes, exact executed code, locks and independent WAV formulas verified")
