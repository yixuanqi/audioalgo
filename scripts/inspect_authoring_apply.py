"""Independent read of an installed authoring package and authoritative index."""
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
receipts = [p / "receipt.json" for p in args.root.iterdir()
            if p.is_dir() and (p / "receipt.json").is_file()
            and start <= (p / "receipt.json").stat().st_mtime <= end]
assert len(receipts) == 1, receipts
receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
package = filesystem(Path(receipt["package_root"]))
index_path = receipts[0].parent / "index.json"
index = json.loads(index_path.read_text(encoding="utf-8"))
assert index["schema_version"] == 1 and len(index["packages"]) == 1
entry = index["packages"][0]
assert entry["package_id"] == package.parent.name
assert entry["content_sha256"] == receipt["content_sha256"] == package.name
assert entry["wrapper_version"] == "python-function-v1"
snapshot = json.loads((package / "snapshot.json").read_text(encoding="utf-8"))
assert snapshot["content_sha256"] == package.name
for name, digest in snapshot["files"].items():
    assert sha(package / name) == digest
signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(snapshot["files"].items()))
assert hashlib.sha256(signature.encode()).hexdigest() == package.name
assert sha(package / "user_source.py") == receipt["source_sha256"]
assert len(list(package.parent.iterdir())) == 1
with args.output.open("x", encoding="utf-8") as output:
    json.dump({"command_record_sha256": sha(args.command_record), "index_sha256": sha(index_path),
               "package_sha256": package.name, "files": snapshot["files"],
               "result": "passed"}, output, indent=2)
print("Installed bytes, content identity, single version and authoritative index verified independently")
