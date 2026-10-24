"""Read production Run WAV, project locks and frozen source without product readers."""
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
receipts = [p / "receipt.json" for p in args.root.iterdir()
            if p.is_dir() and (p / "receipt.json").is_file()
            and start <= (p / "receipt.json").stat().st_mtime <= end]
assert len(receipts) == 1, receipts
receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
output = filesystem(Path(receipt["output_file"]))
data = output.read_bytes()
assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
chunks = {}
offset = 12
while offset + 8 <= len(data):
    kind, size = struct.unpack_from("<4sI", data, offset)
    chunks[kind] = data[offset + 8:offset + 8 + size]
    offset += 8 + size + size % 2
tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
assert tag == 3 and bits == 32 and channels == 1 and rate == 16000
samples = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
assert len(samples) == 513
errors = []
for n, sample in enumerate(samples):
    expected = 0.16 * math.sin(0.03 * (n + 1))
    assert math.isfinite(sample) and abs(sample - expected) <= 2e-6 + 1e-5 * abs(expected)
    errors.append(abs(sample - expected))
project_path = receipts[0].parent / "project.aaproj"
project = json.loads(project_path.read_text(encoding="utf-8"))
assert project["schema_version"] == 2 and len(project["nodes"]) == 3 and len(project["connections"]) == 2
managed = [n for n in project["nodes"] if "managed_version" in n]
assert len(managed) == 1
lock = managed[0]["managed_version"]
package = filesystem(receipts[0].parent / "authoring/packages" / lock["package_id"] / lock["content_sha256"])
metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
for name, digest in metadata["files"].items():
    assert sha(package / name) == digest
assert managed[0]["parameters"]["gain"] == receipt["gain"] or abs(managed[0]["parameters"]["gain"] - receipt["gain"]) <= 1e-15
executed = []
for result_path in filesystem(output.parent).rglob("result.json"):
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("code_files") != metadata["files"]:
        continue
    snapshot = result_path.parent.parent / result["code_snapshot"]
    for name, digest in result["code_files"].items():
        assert sha(snapshot / name) == digest
    executed.append({"result": str(result_path), "sha256": sha(result_path)})
assert executed, "No actual executed matching frozen package found"
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"command_record_sha256": sha(args.command_record), "wav_sha256": sha(output),
               "project_sha256": sha(project_path), "frames": len(samples), "max_abs_error": max(errors),
               "exact_lock": lock, "executed": executed, "result": "passed"}, stream, indent=2)
print(f"Production WAV {len(samples)} frames; independent max_abs_error={max(errors):.12g}; exact lock and executed bytes verified")
