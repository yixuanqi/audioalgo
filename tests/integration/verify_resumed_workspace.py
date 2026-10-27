"""Inspect resumed project bytes and a real owned launcher capture, independently."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(files_receipt, launcher_receipt, helper):
    repository = Path(__file__).resolve().parents[2]
    receipt = json.loads(files_receipt.read_text("utf-8"))
    fixture = repository / "tests/fixtures/node_workspace" / receipt["fixture"]
    expected = {p.relative_to(fixture).as_posix(): p.read_bytes() for p in fixture.rglob("*") if p.is_file()}
    expected["notes.txt"] = b"keep my existing project notes\n"
    project = files_receipt.parent / receipt["project"]
    actual = {p.relative_to(project).as_posix(): p.read_bytes() for p in project.rglob("*") if p.is_file()}
    assert actual == expected, "existing project changed"
    launch = json.loads(launcher_receipt.read_text("utf-8"))
    expected_arguments = [str(Path(value)) for value in launch["expected"]]
    data = (launcher_receipt.parent / "editor-argv.bin").read_bytes()
    offset = 0

    def count():
        nonlocal offset
        assert offset + 4 <= len(data)
        value = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        return value

    assert count() == 2
    arguments = []
    for _ in range(2):
        size = count()
        assert size % 2 == 0 and size <= 65536 and offset + size <= len(data)
        arguments.append(data[offset:offset + size].decode("utf-16-le"))
        offset += size
    assert offset == len(data) and arguments == expected_arguments
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert digest(Path(arguments[0])) == digest(helper), "capture is not the owned helper"
    assert (Path(arguments[1]) / "node.json").is_file()
    return {"scope": "existing fixture and notes unchanged; production launcher to owned helper",
            "files": {name: hashlib.sha256(data).hexdigest() for name, data in actual.items()},
            "argv": arguments, "capture_sha256": hashlib.sha256(data).hexdigest(),
            "professional_editor_and_desktop": "not_verified"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("files_receipt", type=Path)
    parser.add_argument("launcher_receipt", type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.files_receipt, args.launcher_receipt, args.helper), indent=2))
