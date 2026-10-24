"""Independent old-lock reopen oracle: current index v2, actual GUI/CLI output v1."""
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
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "authoring/index.json").is_file()
         and start <= (p / "authoring/index.json").stat().st_mtime <= end]
assert len(roots) == 2, roots
outputs, modes = [], set()
for original_root in roots:
    root = filesystem(original_root)
    project = json.loads((root / "project.aaproj").read_text(encoding="utf-8"))
    lock, = [n["managed_version"] for n in project["nodes"] if "managed_version" in n]
    index = json.loads((root / "authoring/index.json").read_text(encoding="utf-8"))
    entry, = index["packages"]
    assert entry["package_id"] == lock["package_id"] and entry["content_sha256"] != lock["content_sha256"]
    package = root / "authoring/packages" / lock["package_id"] / lock["content_sha256"]
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    assert metadata["content_sha256"] == lock["content_sha256"]
    signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(metadata["files"].items()))
    assert hashlib.sha256(signature.encode()).hexdigest() == lock["content_sha256"]
    for name, digest in metadata["files"].items():
        assert sha(package / name) == digest
    if (root / "v1/complete.json").is_file():
        modes.add("fresh_cli")
        runs = [root / "v1", root / "recovered-v1"]
        for name in ["missing-v1", "corrupt-v1"]:
            assert not (root / name / "complete.json").exists()
            assert not (root / name / "output.wav").exists()
            assert "managed_version_unavailable" in (root / (name + ".log")).read_text(encoding="utf-8")
        assert (runs[0] / "output.wav").read_bytes() == (runs[1] / "output.wav").read_bytes()
    else:
        modes.add("production_open_run")
        runs = [p for p in (root / "runs").iterdir() if (p / "complete.json").is_file()]
        assert len(runs) == 1
    for run in runs:
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
        for n, sample in enumerate(samples):
            expected = .1 * math.sin(.03 * (n + 1))
            assert math.isfinite(sample) and abs(sample - expected) <= 2e-6 + 1e-5 * abs(expected)
            errors.append(abs(sample - expected))
        executed = []
        for result_path in run.rglob("result.json"):
            result = json.loads(result_path.read_text(encoding="utf-8"))
            if result.get("code_files") != metadata["files"]:
                continue
            for name, digest in metadata["files"].items():
                assert sha(result_path.parent.parent / result["code_snapshot"] / name) == digest
            executed.append(str(result_path))
        assert executed
        outputs.append({"run": str(run), "wav_sha256": sha(run / "output.wav"), "max_abs_error": max(errors),
                        "exact_lock": lock, "current_default_hash": entry["content_sha256"], "executed": executed})
assert modes == {"fresh_cli", "production_open_run"}
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "outputs": outputs,
               "missing_corrupt_rejected": True, "recovered_identical": True}, stream, indent=2)
print("GUI reopen and fresh CLI execute exact v1 under v2 default; missing/corrupt rejected, byte recovery verified")
