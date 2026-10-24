"""Independent read of the CLI restore/strict-lock regression artifacts."""
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
roots = [p for p in args.root.iterdir() if (p / "v1/complete.json").is_file()
         and start <= (p / "v1/complete.json").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = filesystem(roots[0])
assert not (root / "missing/complete.json").exists()
valid = json.loads((root / "v1/project.aaproj").read_text(encoding="utf-8"))
invalid = json.loads((root / "project.aaproj").read_text(encoding="utf-8"))
assert valid["nodes"][1]["managed_version"]["content_sha256"] != "f" * 64
assert invalid["nodes"][1]["managed_version"]["content_sha256"] == "f" * 64
raw = (root / "v1/output.wav").read_bytes()
assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
chunks = {}; offset = 12
while offset + 8 <= len(raw):
    kind, size = struct.unpack_from("<4sI", raw, offset)
    chunks[kind] = raw[offset + 8:offset + 8 + size]
    offset += 8 + size + size % 2
tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
assert tag == 3 and channels == 1 and rate == 16000 and bits == 32
samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
assert len(samples) == 513
maximum = 0
for n, sample in enumerate(samples):
    expected = 0.1 * math.sin(0.03 * (n + 1))
    assert math.isfinite(sample) and abs(sample - expected) <= 2e-6 + 1e-5 * abs(expected)
    maximum = max(maximum, abs(sample - expected))
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"command_record_sha256": sha(args.command_record), "output_sha256": sha(root / "v1/output.wav"),
               "frames": 513, "max_abs_error": maximum, "exact_lock": valid["nodes"][1]["managed_version"],
               "missing_success_marker": False, "result": "passed"}, stream, indent=2)
print(f"Fresh CLI WAV independently verified; 513 frames, max_abs_error={maximum:.12g}; missing lock has no success marker")
