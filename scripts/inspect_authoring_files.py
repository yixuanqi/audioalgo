"""Independent byte reader for selected draft files and recovery sidecars."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path


def io(path):
    value = str(Path(path).absolute())
    if os.name == "nt" and not value.startswith("\\\\?\\"):
        value = "\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value
    return Path(value)


def fields(data, header, count):
    assert data.startswith(header)
    cursor, result = len(header), []
    for _ in range(count):
        end = data.index(b"\n", cursor)
        raw = data[cursor:end]
        assert raw.isdigit()
        length = int(raw)
        cursor = end + 1
        assert length <= len(data) - cursor
        result.append(data[cursor:cursor + length])
        cursor += length
    assert cursor == len(data)
    return result


parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("--root", type=Path, action="append", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
record = json.loads(args.command_record.read_text(encoding="utf-8"))
assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
reports, pairs, long_path = [], 0, False
for root in args.root:
    for path in io(root).rglob("*.aadraft"):
        if not start <= path.stat().st_mtime <= end:
            continue
        data = path.read_bytes()
        ident, generation, language, declaration, source = fields(data, b"AudioAlgoDraft/1\n", 5)
        assert ident and int(generation) >= 1 and language == b"python"
        declaration.decode("utf-8"); source.decode("utf-8")
        item = {"path": str(path), "id": ident.decode(), "generation": int(generation),
                "sha256": hashlib.sha256(data).hexdigest(), "source_sha256": hashlib.sha256(source).hexdigest()}
        if path.name == "renamed.aadraft":
            first = path.parent.parent / "用户选择/初稿.aadraft"
            assert first.read_bytes() == data
            pairs += 1
        if ident == b"logical-identity":
            assert len(str(path)) > 260
            assert source == b"external editor"  # original and failure preservation were checked before the explicit external update
            long_path = True
        binding = path.with_name(path.name + ".binding")
        if binding.exists():
            bid, gen, original, expected = fields(binding.read_bytes(), b"AudioAlgoRecoveryBinding/1\n", 4)
            assert bid == ident
            item["binding_generation"] = int(gen)
            item["binding_matches"] = gen == generation
            if original:
                original.decode("utf-8")
                assert fields(expected, b"AudioAlgoDraft/1\n", 5)[0] == ident
                item["original_file"] = original.decode("utf-8")
        reports.append(item)
assert reports and pairs and long_path
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"command_sha256": hashlib.sha256(args.command_record.read_bytes()).hexdigest(),
               "files": reports, "save_as_pairs": pairs, "long_path_verified": long_path}, stream, ensure_ascii=False, indent=2)
print(f"Independently read {len(reports)} actual drafts; Save As pairs={pairs}, long UTF-8 paths verified")
