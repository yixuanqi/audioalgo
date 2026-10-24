"""Read actual authoring probe sidecars/NPY using a separate numerical oracle."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def filesystem(path):
    name = str(Path(path).absolute())
    if os.name == "nt" and not name.startswith("\\\\?\\"):
        name = "\\\\?\\UNC\\" + name[2:] if name.startswith("\\\\") else "\\\\?\\" + name
    return Path(name)


def inspect(receipt, ramp_step=0.0):
    r = json.loads(receipt.read_text(encoding="utf-8"))
    package = filesystem(r["package_root"])
    assert sha(package / "user_source.py") == r["source_sha256"]
    assert package.name == r["package_sha256"]
    snapshot = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    assert snapshot["content_sha256"] == r["package_sha256"]
    signature = ""
    for name, digest in sorted(snapshot["files"].items()):
        assert sha(package / name) == digest
        signature += name + "\0" + digest + "\n"
    assert hashlib.sha256(signature.encode()).hexdigest() == r["package_sha256"]
    assert not (receipt.parent / "should-not-execute.txt").exists()
    probe = filesystem(r["probe_directory"])
    results = list(probe.glob("python-result-*/result.json"))
    assert len(results) == 1
    result = json.loads(results[0].read_text(encoding="utf-8"))
    output = result["outputs"]["audio_out"]
    path = results[0].parent / output["path"]
    assert sha(path) == output["sha256"]
    y = np.load(path, allow_pickle=False)
    expected = 0.1 * np.sin(0.03 * (np.arange(513, dtype=np.float64) + 1))
    if ramp_step:
        declaration = json.loads((package / "manifest.json").read_text(encoding="utf-8"))["nodes"][0]
        assert declaration["authoring_template"] == {"id": "stateful-ramp", "version": 1}
        assert declaration["execution"]["stateful"] is True
        assert next(p["default"] for p in declaration["parameters"] if p["id"] == "step") == ramp_step
        expected += ramp_step * np.arange(513, dtype=np.float64)
    assert y.dtype == np.float32 and y.shape == (1, 513)
    error = np.abs(y[0].astype(np.float64) - expected)
    assert np.all(error <= 2e-6 + 1e-5 * np.abs(expected))
    assert output["valid_interval"] == [0, 513] and output["sample_rate_hz"] == 16000
    assert output["channel_ids"] == ["authoring-channel-0"]
    for name, digest in result["code_files"].items():
        assert sha(probe / result["code_snapshot"] / name) == digest
    assert r["syntax_pid"] > 0 and r["probe_pid"] > 0 and r["syntax_pid"] != r["probe_pid"]
    return {"receipt": str(receipt.resolve()), "receipt_sha256": sha(receipt),
            "output": str(path.resolve()), "output_sha256": sha(path),
            "maximum_absolute_error": float(error.max()), "package_sha256": r["package_sha256"],
            "environment": result["environment"], "syntax_pid": r["syntax_pid"], "probe_pid": r["probe_pid"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ramp-step", type=float, default=0.0, help="independent stateful-template oracle coefficient")
    parser.add_argument("--command-record", type=Path)
    args = parser.parse_args()
    receipts = list(args.root.rglob("receipt.json"))
    if args.command_record:
        command = json.loads(args.command_record.read_text(encoding="utf-8")); assert command["exit_code"] == 0
        start = dt.datetime.fromisoformat(command["started_utc"]).timestamp()
        end = dt.datetime.fromisoformat(command["finished_utc"]).timestamp()
        receipts = [p for p in receipts if start <= p.stat().st_mtime <= end]
    assert receipts, "no actual probe receipt"
    reports = [inspect(path, args.ramp_step) for path in receipts]
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(reports, stream, ensure_ascii=False, indent=2)
    print(f"Independently verified {len(reports)} real NPY/package/source sets")
