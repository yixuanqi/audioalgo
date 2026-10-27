"""Read actual Win32 stream-write outcomes and argv without importing product code."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(root, helper, expected):
    root = root.resolve()
    report = (root / "stdio.bin").read_bytes()
    assert len(report) == 24
    values = struct.unpack("<6I", report)
    assert values == expected, values
    capture = (root / "editor-argv.bin").read_bytes()
    argc = struct.unpack_from("<I", capture)[0]
    assert argc == 2
    offset, arguments = 4, []
    for _ in range(argc):
        assert offset + 4 <= len(capture)
        size = struct.unpack_from("<I", capture, offset)[0]
        offset += 4
        assert size % 2 == 0 and size <= 65536 and offset + size <= len(capture)
        arguments.append(capture[offset:offset + size].decode("utf-16-le"))
        offset += size
    assert offset == len(capture)
    assert Path(arguments[0]) == root / "Trae CN.exe"
    assert Path(arguments[1]).parent == root
    assert (Path(arguments[1]) / "node.json").is_file()
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert digest(Path(arguments[0])) == digest(helper)
    return {"root": str(root), "stdout": values[:3], "stderr": values[3:], "argv": arguments,
            "stdio_sha256": hashlib.sha256(report).hexdigest(), "argv_sha256": hashlib.sha256(capture).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("red", type=Path)
    parser.add_argument("green", type=Path)
    parser.add_argument("--helper", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps({"red": inspect(args.red, args.helper, (3, 0, 232, 3, 0, 232)),
                      "green": inspect(args.green, args.helper, (2, 1, 0, 2, 1, 0)),
                      "scope": "owned delayed-write helper after production launcher and component destruction",
                      "installed_Trae_and_desktop": "not_verified"}, indent=2))
