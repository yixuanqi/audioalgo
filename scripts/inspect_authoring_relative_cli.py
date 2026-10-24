"""Independent result for CLI invoked with relative project/output arguments."""
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
record = json.loads(args.command_record.read_text(encoding="utf-8")); assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "project/run/complete.json").is_file()
         and start <= (p / "project/run/complete.json").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = filesystem(roots[0]); run = root / "project/run"
project = json.loads((root / "project/project.aaproj").read_text(encoding="utf-8"))
lock = project["nodes"][1]["managed_version"]
assert not (root / "empty-authoring/packages").exists()
package = root / "project" / lock["package_path"]
metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
assert metadata["content_sha256"] == lock["content_sha256"]
executed = []
for result_path in run.rglob("result.json"):
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("code_files") != metadata["files"]: continue
    for name, digest in metadata["files"].items():
        assert sha(package / name) == sha(result_path.parent.parent / result["code_snapshot"] / name) == digest
    executed.append(str(result_path))
assert len(executed) == 1
raw = (run / "output.wav").read_bytes(); assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
chunks, offset = {}, 12
while offset + 8 <= len(raw):
    kind, size = struct.unpack_from("<4sI", raw, offset)
    chunks[kind] = raw[offset + 8:offset + 8 + size]; offset += 8 + size + size % 2
tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"]); assert len(samples) == 513
maximum = 0
for n, value in enumerate(samples):
    expected = .1 * math.sin(.03 * (n + 1))
    assert math.isfinite(value) and abs(value - expected) <= 2e-6 + 1e-5 * abs(expected)
    maximum = max(maximum, abs(value - expected))
resource, = json.loads((run / "run.json").read_text(encoding="utf-8"))["resources"]
assert sha(run / resource["snapshot_path"]) == sha(root / "project/input.wav") == resource["sha256"]
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record), "exact_lock": lock,
               "executed": executed, "wav_sha256": sha(run / "output.wav"), "max_abs_error": maximum}, stream, indent=2)
print("Relative CLI project/output path: carried exact code, input capture and 513-frame WAV verified independently")
