"""Fresh-process, standard-library inspection of persisted authoring paths."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(receipt):
    root = receipt.resolve().parent
    record = json.loads(receipt.read_text("utf-8"))
    settings = root / record["settings"]
    payload = settings.read_bytes()
    value = json.loads(payload)
    assert set(value) == {"schema_version", "editor_executable", "last_project_directory"}
    assert value["schema_version"] == 1
    executable = Path(value["editor_executable"])
    project = Path(value["last_project_directory"])
    assert executable == root / "editor.EXE"
    assert project == root / "last 工程 & $() `"
    assert executable.is_file() and project.is_dir()
    node = json.loads((project / "node.json").read_text("utf-8"))
    assert node["python"]["entrypoint"] == "remember_me.process:process"
    assert (project / "src/remember_me/process.py").is_file()
    capture = (root / "editor-argv.bin").read_bytes()
    assert struct.unpack_from("<I", capture)[0] == 2
    offset, arguments = 4, []
    for _ in range(2):
        size = struct.unpack_from("<I", capture, offset)[0]
        offset += 4
        assert size % 2 == 0 and size <= 65536 and offset + size <= len(capture)
        arguments.append(capture[offset:offset + size].decode("utf-16-le"))
        offset += size
    assert offset == len(capture)
    assert [Path(arg) for arg in arguments] == [executable, project]
    return {"settings": str(settings), "sha256": hashlib.sha256(payload).hexdigest(),
            "restored": value, "actual_editor_argv": arguments,
            "scope": "disk settings independently decoded in a fresh Python process; no source in settings",
            "OS_desktop_restart": "not_verified"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("receipt", type=Path)
    print(json.dumps(verify(parser.parse_args().receipt), indent=2))
