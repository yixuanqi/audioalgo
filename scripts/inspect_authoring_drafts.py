"""Independent byte reader for draft component artifacts; does not import product code."""
import argparse
import hashlib
import json
from pathlib import Path


def inspect(path):
    raw = path.read_bytes()
    magic = b"AudioAlgoDraft/1\n"
    assert raw.startswith(magic)
    offset = len(magic)
    fields = []
    for _ in range(5):
        end = raw.index(b"\n", offset)
        length = int(raw[offset:end])
        offset = end + 1
        value = raw[offset:offset + length]
        assert len(value) == length
        fields.append(value.decode("utf-8"))
        offset += length
    assert offset == len(raw)
    identity, generation, language, declaration, source = fields
    assert identity == path.stem and int(generation) > 0 and language == "python"
    assert json.loads(declaration) == {"type_id": "user.test_saved", "parameters": [
        {"id": "gain", "type": "float", "default": 0.25}]}
    assert source == "# 中文 自定义节点\ndef process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain']}\n"
    before, after = path.parent / "before.aaproj", path.parent / "after.aaproj"
    assert before.read_bytes() == after.read_bytes()
    return {"path": str(path.resolve()), "draft_id": identity, "generation": int(generation),
            "sha256": hashlib.sha256(raw).hexdigest(), "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
            "declaration_sha256": hashlib.sha256(declaration.encode()).hexdigest(),
            "project_sha256": hashlib.sha256(before.read_bytes()).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    artifacts = list(args.root.rglob("*.aadraft"))
    assert artifacts, "no actual draft artifacts"
    report = [inspect(path) for path in artifacts]
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
    print(f"Independently read {len(report)} draft(s); exact Unicode/source/declaration/project bytes match")
