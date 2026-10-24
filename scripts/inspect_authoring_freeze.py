"""Compare Ready package bytes to the bytes actually executed by the isolated probe."""
import argparse
import datetime as dt
import hashlib
import json
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
roots = [p for p in args.root.iterdir() if p.is_dir() and start <= p.stat().st_mtime <= end]
assert len(roots) == 1, f"Expected exactly this command's artifact root: {roots}"
root = filesystem(roots[0])
reports = []
for frozen in root.glob("staging/*/frozen/*"):
    stage = frozen.parent.parent
    candidate = stage / "candidate/user_source.py"
    metadata = json.loads((frozen / "snapshot.json").read_text(encoding="utf-8"))
    for name, digest in metadata["files"].items():
        assert sha(frozen / name) == digest
    assert frozen.name == metadata["content_sha256"]
    signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(metadata["files"].items()))
    assert hashlib.sha256(signature.encode()).hexdigest() == frozen.name
    assert (frozen / "user_source.py").read_bytes() != candidate.read_bytes()
    results = []
    for manifest in stage.glob("python-task-*/python-result-*/result.json"):
        actual = json.loads(manifest.read_text(encoding="utf-8"))
        task = manifest.parent.parent
        snapshot = task / actual["code_snapshot"]
        code_manifest = json.loads((snapshot / "manifest.json").read_text(encoding="utf-8"))
        if code_manifest["nodes"][0]["entrypoint"] != "authoring_wrapper:Node":
            continue
        for name, digest in actual["code_files"].items():
            assert sha(snapshot / name) == digest
        assert (snapshot / "user_source.py").read_bytes() == (frozen / "user_source.py").read_bytes()
        # The host signature and runner inventory use different documented byte
        # encodings; compare every file and independently validate both digests.
        assert actual["code_files"] == metadata["files"]
        runner_signature = json.dumps(actual["code_files"], sort_keys=True, separators=(",", ":")).encode()
        assert hashlib.sha256(runner_signature).hexdigest() == actual["code_sha256"]
        results.append({"actual_result_manifest": str(manifest), "result_sha256": sha(manifest),
                        "executed_source_sha256": sha(snapshot / "user_source.py")})
    assert len(results) == 1
    reports.append({"package_sha256": frozen.name, "frozen_source_sha256": sha(frozen / "user_source.py"),
                    "changed_candidate_sha256": sha(candidate), **results[0]})
assert reports
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"command_record_sha256": sha(args.command_record), "packages": reports}, stream, indent=2)
print(f"Compared {len(reports)} Ready package(s) with actual executed immutable snapshots; changed candidate excluded")
