"""Verify PC0406's real stream/level ownership, filtered text, code and audio."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path

NAME = "PC-04.d production Run level filter separates real stdout and stderr without changing audio"


def read_json(path):
    return json.loads(io_path(path).read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    case = next(c for c in ET.parse(args.junit).iter("testcase") if c.attrib["name"] == NAME)
    assert case.find("failure") is None and case.find("error") is None
    line = next(s for s in case.findtext("system-out", "").splitlines() if s.startswith("PC0406_ARTIFACT_ROOT="))
    root = Path(line.split("=", 1)[1]).resolve()
    managed = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0406-level-filter/artifacts"
    assert root.parent == managed.resolve()
    demo = read_json(root / "level-demo.json")
    project = read_json(root / demo["project_file"])
    node = next(n for n in project["nodes"] if n["id"] == demo["node_id"])
    lock = node["managed_version"]["content_sha256"]
    assert node["type_id"] == "user.pc02_print" and len(project["connections"]) == 2
    texts = {level: (root / f"ui-level-{level}.log").read_text(encoding="utf-8") for level in ("info", "error", "all")}
    for value in texts.values():
        assert demo["request_id"] in value and demo["node_id"] in value and lock in value
    assert texts["info"].count("aaaaaaa") == 2 and "level-error-marker" not in texts["info"]
    assert texts["error"].count("level-error-marker") == 2 and "aaaaaaa" not in texts["error"]
    assert texts["all"].count("aaaaaaa") == texts["all"].count("level-error-marker") == 2
    input_fmt, expected = read_float_wav(root / "input.wav")
    assert input_fmt == (1, 16000, 32) and len(expected) == 513 * 4
    runs = []
    for path in (root / "runs").glob("*/run.json"):
        record = read_json(path)
        run = path.parent
        fmt, actual = read_float_wav(run / "output.wav")
        assert fmt == input_fmt and actual == expected
        assert record["frames"] == record["valid_frame_count"] == 513
        sources = []
        for task in record["task_artifacts"]:
            p = task["provenance"]
            data = io_path(run / task["directory"] / p["code_snapshot"] / "user_source.py").read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            assert digest == p["code_files"]["user_source.py"]
            assert io_path(run / p["frozen_package_root"] / "user_source.py").read_bytes() == data
            assert b"file=sys.stderr" in data and b"print('aaaaaaa'" in data
            sources.append(digest)
        assert len(sources) == 1
        matching = []
        for archive in (root / "run-logs").glob("*/events.jsonl"):
            events = [json.loads(s) for s in archive.read_text(encoding="utf-8").splitlines()]
            if events and events[0]["request_id"] == record["request_id"]:
                matching.append((archive, events))
        assert len(matching) == 1
        archive, events = matching[0]
        assert all(e["request_id"] == record["request_id"] and e["node_id"] == demo["node_id"] and e["type_id"] == node["type_id"] and e["content_sha256"] == lock for e in events)
        stream_receipts = []
        for stream, level, content in (("stdout", "info", "aaaaaaa\n"), ("stderr", "error", "level-error-marker\n")):
            selected = [e for e in events if e["source"] == stream]
            assert selected and all(e["level"] == level for e in selected)
            assert all(b["sequence"] > a["sequence"] for a, b in zip(selected, selected[1:]))
            paths = {Path(e["raw_log_file"]).resolve() for e in selected}
            assert len(paths) == 1
            raw_path = next(iter(paths)); assert raw_path.is_relative_to(root / "run-logs")
            raw = io_path(raw_path).read_bytes()
            # The Windows Python text stream translates print's LF to CRLF;
            # events preserve raw bytes, while the view normalizes to LF.
            assert p["environment"]["platform"].startswith("Windows")
            expected_raw = (content * 2).replace("\n", "\r\n").encode()
            assert raw == expected_raw == "".join(e["text"] for e in selected).encode()
            assert selected[-1]["total_bytes"] == len(raw)
            assert all(e["discarded_bytes"] == e["dropped_text_bytes"] == 0 for e in selected)
            stream_receipts.append({"source": stream, "level": level, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
        runs.append({"request": record["request_id"], "archive": str(archive.relative_to(root)), "run": str(run.relative_to(root)), "source_sha256": sources, "streams": stream_receipts, "max_error": 0})
    assert len(runs) == 2 and len({r["request"] for r in runs}) == 2
    assert demo["request_id"] in {r["request"] for r in runs}
    report = {"status": "passed", "root": str(root), "current_request": demo["request_id"], "node": demo["node_id"], "content_sha256": lock, "runs": runs,
              "scope": "production component level filter and real streams/audio; no desktop/history claim"}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "runs": len(runs), "streams": 4, "max_error": 0}))


if __name__ == "__main__":
    main()
