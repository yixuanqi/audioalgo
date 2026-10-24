"""Independent v1/v2/undo-v1/redo-v2 model, executed-code and RIFF oracle."""
import argparse
import datetime as dt
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
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "redo-v2.aaproj").is_file()
         and start <= (p / "redo-v2.aaproj").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = filesystem(roots[0])
before = json.loads((root / "v1-before.aaproj").read_text(encoding="utf-8"))
index = json.loads((root / "authoring/index.json").read_text(encoding="utf-8"))
entry, = index["packages"]
v1 = before["nodes"][1]["managed_version"]["content_sha256"]
v2 = entry["content_sha256"]
assert v1 != v2
outputs = []
raw_wavs = []
for ordinal, (name, version, amplitude) in enumerate([("v1-before", v1, .1), ("v2", v2, .15), ("undo-v1", v1, .1), ("redo-v2", v2, .15)]):
    project = json.loads((root / (name + ".aaproj")).read_text(encoding="utf-8"))
    assert project["revision"] == before["revision"] + ordinal
    assert project["nodes"][1]["managed_version"]["content_sha256"] == version
    project["revision"] = before["revision"]
    project["nodes"][1]["managed_version"] = before["nodes"][1]["managed_version"]
    assert project == before
    directory = (root / (name + ".run")).read_text(encoding="utf-8")
    assert Path(directory).name == directory
    run = root / "runs" / directory
    raw = (run / "output.wav").read_bytes()
    raw_wavs.append(raw)
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
    chunks, offset = {}, 12
    while offset + 8 <= len(raw):
        kind, size = struct.unpack_from("<4sI", raw, offset)
        chunks[kind] = raw[offset + 8:offset + 8 + size]
        offset += 8 + size + size % 2
    tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
    assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
    samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
    assert len(samples) == 513
    maximum = 0
    for n, value in enumerate(samples):
        expected = amplitude * math.sin(.03 * (n + 1))
        assert math.isfinite(value) and abs(value - expected) <= 2e-6 + 1e-5 * abs(expected)
        maximum = max(maximum, abs(value - expected))
    package = root / "authoring/packages" / entry["package_id"] / version
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    executed = []
    for result_path in run.rglob("result.json"):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result.get("code_files") != metadata["files"]:
            continue
        for file, digest in metadata["files"].items():
            assert sha(package / file) == sha(result_path.parent.parent / result["code_snapshot"] / file) == digest
        executed.append(str(result_path))
    assert len(executed) == 1
    outputs.append({"name": name, "exact_hash": version, "wav_sha256": sha(run / "output.wav"), "max_abs_error": maximum, "executed": executed})
assert raw_wavs[0] == raw_wavs[2] and raw_wavs[1] == raw_wavs[3]
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "outputs": outputs,
               "v1_undo_identical": True, "v2_redo_identical": True}, stream, indent=2)
print("Four real version/history runs verified: monotonic revision, exact code, v1/undo and v2/redo byte-identical WAVs")
