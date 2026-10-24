"""Independent equations for named-template probe NPYs and selected-port project WAVs."""
import argparse
import datetime as dt
import hashlib
import json
import math
import struct
from pathlib import Path
import numpy as np
from inspect_authoring_probe import filesystem, sha

parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("root", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
command = json.loads(args.command_record.read_text(encoding="utf-8")); assert command["exit_code"] == 0
start = dt.datetime.fromisoformat(command["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(command["finished_utc"]).timestamp()

def current(path):
    return path.is_file() and start <= path.stat().st_mtime <= end

def package_files(package):
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    assert metadata["content_sha256"] == package.name
    signature = ""
    for name, digest in sorted(metadata["files"].items()):
        assert sha(package / name) == digest
        signature += name + "\0" + digest + "\n"
    assert hashlib.sha256(signature.encode()).hexdigest() == package.name
    node = json.loads((package / "manifest.json").read_text(encoding="utf-8"))["nodes"][0]
    assert node["authoring_template"] == {"id": "named-router", "version": 1}
    assert [p["id"] for p in node["inputs"]] == ["primary", "reference"]
    assert [p["id"] for p in node["outputs"]] == ["main", "side"]
    return metadata["files"]

def expected(port):
    n = np.arange(513, dtype=np.float64)
    primary = 0.2 * np.sin(0.03 * (n + 1)); reference = 0.2 * np.sin(0.03 * (n + 1) + 1)
    return (2 * primary + 3 * reference) * 0.5 if port == "main" else primary - reference

def compare(samples, port):
    oracle = expected(port); assert samples.shape == (1, 513) and samples.dtype == np.float32
    error = np.abs(samples[0].astype(np.float64) - oracle)
    assert np.all(np.isfinite(samples)) and np.all(error <= 2e-6 + 1e-5 * np.abs(oracle))
    return float(error.max())

def read_result(path, files, ids):
    result = json.loads(path.read_text(encoding="utf-8")); assert set(result["outputs"]) == {"main", "side"}
    assert result["code_files"] == files
    for name, digest in files.items():
        assert sha(path.parent.parent / result["code_snapshot"] / name) == digest
    errors = {}
    for port, output in result["outputs"].items():
        assert output["sample_rate_hz"] == 16000 and output["channel_ids"] == ids and output["valid_interval"] == [0, 513]
        data = path.parent / output["path"]; assert sha(data) == output["sha256"]
        errors[port] = compare(np.load(data, allow_pickle=False), port)
    return errors

reports = []
for receipt_path in filesystem(args.root).rglob("receipt.json"):
    if not current(receipt_path): continue
    receipt = json.loads(receipt_path.read_text(encoding="utf-8")); package = filesystem(receipt["package_root"])
    assert sha(package / "user_source.py") == receipt["source_sha256"]
    results = list(filesystem(receipt["probe_directory"]).glob("python-result-*/result.json")); assert len(results) == 1
    reports.append({"kind": "probe", "receipt_sha256": sha(receipt_path), "errors": read_result(results[0], package_files(package), ["authoring-channel-0"])})

roots = [p for p in filesystem(args.root / "project-run").iterdir() if current(p / "main/complete.json")]
assert len(roots) == 1, roots
for port in ("main", "side"):
    run = roots[0] / port; project = json.loads((run / "project.aaproj").read_text(encoding="utf-8"))
    assert project["selected_output_node_id"] == "custom" and project["selected_output_port_id"] == port
    lock = next(n["managed_version"] for n in project["nodes"] if n["id"] == "custom")
    files = package_files(roots[0] / "authoring/packages" / lock["package_id"] / lock["content_sha256"])
    results = list(run.rglob("result.json")); assert len(results) == 1
    errors = read_result(results[0], files, ["ch0"])
    raw = (run / "output.wav").read_bytes(); assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
    chunks = {}; offset = 12
    while offset + 8 <= len(raw):
        kind, size = struct.unpack_from("<4sI", raw, offset); chunks[kind] = raw[offset + 8:offset + 8 + size]; offset += 8 + size + size % 2
    tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
    assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
    error = compare(np.frombuffer(chunks[b"data"], dtype="<f4").reshape(1, -1), port)
    manifest = json.loads((run / "run.json").read_text(encoding="utf-8")); assert len(manifest["resources"]) == 2
    assert {r["node_id"] for r in manifest["resources"]} == {"primary", "reference"}
    for resource in manifest["resources"]:
        assert sha(run / resource["snapshot_path"]) == resource["sha256"] == resource["snapshot_sha256"]
    reports.append({"kind": "project", "port": port, "wav_sha256": sha(run / "output.wav"), "maximum_absolute_error": error,
                    "all_output_errors": errors, "exact_lock": lock})
assert len(reports) == 3
with args.output.open("x", encoding="utf-8") as output:
    json.dump({"command_record_sha256": sha(args.command_record), "reports": reports, "result": "passed"}, output, indent=2)
print("Verified both named probe outputs, both selected-port WAVs, resource snapshots and exact executed code independently")
