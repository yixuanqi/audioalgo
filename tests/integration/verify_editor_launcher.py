"""Read owned launcher captures without importing or launching the application."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(receipt, helper):
    helper_hash = hashlib.sha256(helper.read_bytes()).hexdigest()
    observations = []
    for row in json.loads(receipt.read_text(encoding="utf-8")):
        capture = Path(row["capture"])
        data = capture.read_bytes()
        offset = 0

        def count():
            nonlocal offset
            assert offset + 4 <= len(data), "truncated argument length"
            value = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            return value

        argc = count()
        assert 1 <= argc <= 16, "invalid argument count"
        actual = []
        for _ in range(argc):
            size = count()
            assert size % 2 == 0 and size <= 131072 and offset + size <= len(data)
            actual.append(data[offset:offset + size].decode("utf-16-le"))
            offset += size
        assert offset == len(data), "trailing capture bytes"
        assert actual == row["expected"], (actual, row["expected"])
        assert hashlib.sha256(Path(actual[0]).read_bytes()).hexdigest() == helper_hash
        observations.append({"capture": str(capture), "sha256": hashlib.sha256(data).hexdigest(), "argv": actual})
    assert len(observations) == 2
    return {"scope": "production launcher to owned hidden helper; real editor and desktop not_verified",
            "helper_sha256": helper_hash, "observations": observations}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.receipt, args.helper), ensure_ascii=True, indent=2))
