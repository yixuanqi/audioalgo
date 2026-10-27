"""Independently inspect the foreign tree and successful retry after swap tests."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def verify(report):
    repository = Path(__file__).resolve().parents[2]
    xml = ET.parse(report)
    roots = {line.removeprefix("NWS0010_ROOT=") for output in xml.findall(".//system-out")
             for line in (output.text or "").splitlines() if line.startswith("NWS0010_ROOT=")}
    assert len(roots) == 3
    markers = {"node.json": b"foreign declaration", "src/swap/__init__.py": b"foreign init",
               "src/swap/process.py": b"foreign source"}
    observations = []
    for raw in sorted(roots):
        root = Path(raw).resolve()
        root.relative_to(repository / "build/reports")
        foreign = root / "foreign"
        actual = {p.relative_to(foreign).as_posix(): p.read_bytes() for p in foreign.rglob("*") if p.is_file()}
        assert actual == markers, raw
        retry = root / "retry"
        declaration = json.loads((retry / "node.json").read_text("utf-8"))
        assert declaration["python"]["entrypoint"] == "swap.process:process"
        assert (retry / "src/swap/process.py").read_bytes() == (
            b"def process(inputs, params, state):\n    return {'main': inputs['primary'] * 0.5}\n")
        assert not (root / "author").exists(), "test-owned junction cleanup did not complete"
        observations.append({"root": str(root), "foreign_sha256": {
            name: hashlib.sha256(data).hexdigest() for name, data in actual.items()},
            "parked_files_retained": sorted(p.relative_to(root / "parked").as_posix()
                                            for p in (root / "parked").rglob("*") if p.is_file())})
    return {"scope": "three actual Win32 junction swaps; foreign bytes and retry independently read",
            "remaining": "handle-bound I/O and finer physical-open races are not proved",
            "observations": observations}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.report), ensure_ascii=True, indent=2))
