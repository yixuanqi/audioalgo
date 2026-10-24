"""Independent old/new project locks and installed bytes after the commit barrier test."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
from inspect_authoring_probe import sha

parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("root", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
record = json.loads(args.command_record.read_text(encoding="utf-8"))
assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "new.aaproj").is_file()
         and start <= (p / "new.aaproj").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = roots[0]
old = json.loads((root / "old.aaproj").read_text(encoding="utf-8"))
new = json.loads((root / "new.aaproj").read_text(encoding="utf-8"))
assert old["project_id"] != new["project_id"] and old["schema_version"] == new["schema_version"] == 2
old_lock, = [n["managed_version"] for n in old["nodes"]]
new_lock, = [n["managed_version"] for n in new["nodes"]]
assert old_lock["package_id"] == new_lock["package_id"]
assert old_lock["content_sha256"] != new_lock["content_sha256"]
index = json.loads((root / "index.json").read_text(encoding="utf-8"))
entry, = index["packages"]
assert entry["content_sha256"] == new_lock["content_sha256"]
assert entry["package_id"] == new_lock["package_id"]
verified = []
for lock in [old_lock, new_lock]:
    package = root / "packages" / lock["package_id"] / lock["content_sha256"]
    metadata = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
    signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(metadata["files"].items()))
    assert hashlib.sha256(signature.encode()).hexdigest() == lock["content_sha256"]
    for name, digest in metadata["files"].items():
        assert sha(package / name) == digest
    verified.append(lock)
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "command_record_sha256": sha(args.command_record),
               "old_project_sha256": sha(root / "old.aaproj"), "new_project_sha256": sha(root / "new.aaproj"),
               "verified_locks": verified, "default_hash": entry["content_sha256"]}, stream, indent=2)
print("Late Apply preserves separate project identities, old/new exact locks and immutable installed bytes")
