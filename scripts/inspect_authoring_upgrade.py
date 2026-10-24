"""Independent whole-package upgrade model and actual two-instance audio oracle."""
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
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "authoring/index.json").is_file()
         and start <= (p / "authoring/index.json").stat().st_mtime <= end]
assert len(roots) == 2, roots
evidence = []
for p in roots:
    root = filesystem(p)
    before = json.loads((root / "project.aaproj").read_text(encoding="utf-8"))
    if (root / "rejected.aaproj").is_file():
        assert (root / "project.aaproj").read_bytes() == (root / "rejected.aaproj").read_bytes()
        assert not (root / "upgraded.aaproj").exists() and not (root / "runs").exists()
        evidence.append({"root": str(p), "rejected_unchanged": True})
        continue
    after = json.loads((root / "upgraded.aaproj").read_text(encoding="utf-8"))
    assert after["revision"] == before["revision"] + 1
    index = json.loads((root / "authoring/index.json").read_text(encoding="utf-8"))
    entry, = index["packages"]
    assert after["connections"] == before["connections"] and len(after["connections"]) == 3
    normalized = json.loads(json.dumps(after))
    normalized["revision"] = before["revision"]
    for n, original in zip(normalized["nodes"], before["nodes"]):
        if "managed_version" in n:
            assert n["managed_version"]["content_sha256"] == entry["content_sha256"]
            assert n["managed_version"]["content_sha256"] != original["managed_version"]["content_sha256"]
            n["managed_version"] = original["managed_version"]
    assert normalized == before
    assert after["nodes"][1]["parameters"]["gain"] == .6 and after["nodes"][2]["parameters"]["gain"] == .8
    assert after["nodes"][1]["future_node"] == {"preserve": True}
    runs = [x for x in (root / "runs").iterdir() if (x / "complete.json").is_file()]
    assert len(runs) == 1
    run = runs[0]
    raw = (run / "output.wav").read_bytes()
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
    errors = []
    for n, value in enumerate(samples):
        expected = .216 * math.sin(.03 * (n + 1))
        assert math.isfinite(value) and abs(value - expected) <= 2e-6 + 1e-5 * abs(expected)
        errors.append(abs(value - expected))
    package = root / "authoring/packages" / entry["package_id"] / entry["content_sha256"]
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    executed = []
    for result_path in run.rglob("result.json"):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result.get("code_files") != metadata["files"]:
            continue
        for name, digest in metadata["files"].items():
            assert sha(package / name) == sha(result_path.parent.parent / result["code_snapshot"] / name) == digest
        executed.append(str(result_path))
    assert len(executed) == 2, executed
    evidence.append({"root": str(p), "upgraded_project_sha256": sha(root / "upgraded.aaproj"),
                     "wav_sha256": sha(run / "output.wav"), "max_abs_error": max(errors), "executed": executed})
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "evidence": evidence}, stream, indent=2)
print("Whole-package upgrade changes only locks/revision; two v2 instances match audio oracle, incompatible project byte-identical")
