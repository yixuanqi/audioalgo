"""Compare UI-edited saved draft with the applied declaration and source bytes."""
import argparse
import datetime as dt
import json
import sys
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
roots = [p for p in args.root.iterdir() if p.is_dir() and (p / "index.json").is_file()
         and start <= (p / "index.json").stat().st_mtime <= end]
assert len(roots) == 1, roots
root = filesystem(roots[0]); drafts = list((root / "drafts").glob("*.aadraft")); assert len(drafts) == 1
data = drafts[0].read_bytes(); magic = b"AudioAlgoDraft/1\n"; assert data.startswith(magic)
offset = len(magic); fields = []
for _ in range(5):
    newline = data.index(b"\n", offset); count = int(data[offset:newline]); offset = newline + 1
    fields.append(data[offset:offset + count].decode("utf-8")); offset += count
assert offset == len(data)
identity, generation, language, declaration, source = fields
assert identity == drafts[0].stem and int(generation) > 1 and language == "python"
node = json.loads(declaration)
assert node["type_id"] == "user.named_transform" and node["display_name"] == "中文 自定义增益"
assert node["parameters"][0]["default"] == 0.5 and node["inputs"][0]["id"] == "audio_in"
index = json.loads((root / "index.json").read_text(encoding="utf-8")); assert len(index["packages"]) == 1
entry = index["packages"][0]; package = root / "packages" / entry["package_id"] / entry["content_sha256"]
manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8")); assert len(manifest["nodes"]) == 1
generated = manifest["nodes"][0]; assert generated.pop("entrypoint") == "authoring_wrapper:Node"
assert generated == node and (package / "user_source.py").read_bytes() == source.encode()
with args.output.open("x", encoding="utf-8") as output:
    json.dump({"command": [sys.executable, *sys.argv], "reader_sha256": sha(Path(__file__)),
               "command_record_sha256": sha(args.command_record), "draft_sha256": sha(drafts[0]),
               "manifest_sha256": sha(package / "manifest.json"), "source_sha256": sha(package / "user_source.py"),
               "result": "passed"}, output, ensure_ascii=False, indent=2)
print("UI-edited name/type, UTF-8 draft framing, single declaration and applied source verified independently")
