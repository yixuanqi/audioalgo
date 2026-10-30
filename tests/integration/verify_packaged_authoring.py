"""Independently read this package probe's PCM and runner identities (stdlib only)."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct


def io_path(path: Path) -> Path:
    value = str(path.absolute())
    if os.name != "nt" or value.startswith("\\\\?\\"):
        return Path(value)
    return Path("\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value)


def verify(root: Path) -> dict:
    root = io_path(root)
    results = []
    for receipt_path in sorted(root.rglob("receipt.json")):
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        samples = struct.unpack("<513f", receipt_path.with_name("output.f32").read_bytes())
        error = max(abs(value - ((index % 31) - 15) / 128) for index, value in enumerate(samples))
        assert error == 0, (receipt_path, error)
        executable = io_path(Path(receipt["interpreter"]))
        assert hashlib.sha256(executable.read_bytes()).hexdigest() == receipt["interpreter_sha256"]
        modules = io_path(Path(receipt["runner"]))
        files = sorted((p for p in (modules / "audioalgo_runner").rglob("*")
                        if p.is_file() and (p.suffix == ".py" or p.name == "manifest.json")),
                       key=lambda p: p.relative_to(modules).as_posix().encode("utf-8"))
        signature = b"".join(p.relative_to(modules).as_posix().encode("utf-8") + b"\0" +
                             hashlib.sha256(p.read_bytes()).hexdigest().encode("ascii") + b"\n" for p in files)
        assert hashlib.sha256(signature).hexdigest() == receipt["runner_sha256"]
        snapshots = list((receipt_path.parent / "library/staging").glob("*/runner/*/snapshot.json"))
        assert snapshots, receipt_path
        for snapshot in snapshots:
            metadata = json.loads(snapshot.read_text(encoding="utf-8"))
            assert metadata["content_sha256"] == receipt["runner_sha256"]
            assert all(name.startswith("audioalgo_runner/") for name in metadata["files"])
            assert len(metadata["files"]) == len(files) <= 128
            for name, digest in metadata["files"].items():
                assert hashlib.sha256((snapshot.parent / name).read_bytes()).hexdigest() == digest
        results.append({"receipt": str(receipt_path), "samples": len(samples), "maximum_error": error,
                        "runner_files": len(files), "runner_sha256": receipt["runner_sha256"]})
    assert results, f"No package receipts under {root}"
    return {"scope": "development_subset", "status": "passed", "cases": results,
            "samples": sum(item["samples"] for item in results)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.root.resolve())
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"verified {len(result['cases'])} package runs, {result['samples']} samples; maximum error 0")
