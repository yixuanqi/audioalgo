"""Independently inspect PC0407's owned history UI, real Runs and reader fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path

WORKFLOW = "PC-04.d production historical Run logs do not replace current result or preview"
READER = "PC-04.d history reader bounds owned tail and rejects mixed or malformed archives"


def read(path):
    return json.loads(io_path(path).read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    cases = {c.attrib["name"]: c for c in ET.parse(args.junit).iter("testcase")}
    managed = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0407-log-history"

    def owned_root(name, marker, folder):
        case = cases[name]
        assert case.find("failure") is None and case.find("error") is None
        line = next(s for s in case.findtext("system-out", "").splitlines() if s.startswith(marker))
        result = Path(line.split("=", 1)[1]).resolve()
        assert result.parent == (managed / folder).resolve()
        return result

    root = owned_root(WORKFLOW, "PC0407_ARTIFACT_ROOT=", "artifacts")
    fixture = owned_root(READER, "PC0407_READER_ROOT=", "reader")
    demo = read(root / "history-demo.json")
    old, current = demo["old_request"], demo["current_request"]
    assert old != current and demo["stale_history_cleared"] is True
    old_ui = (root / "history-old-ui.log").read_text(encoding="utf-8")
    current_ui = (root / "history-current-ui.log").read_text(encoding="utf-8")
    assert old in old_ui and current not in old_ui and old_ui.count("aaaaaaa") == 2
    assert current in current_ui and old not in current_ui and current_ui.count("aaaaaaa") == 2
    assert (root / "history-cleared-ui.log").read_bytes() == b""
    assert read(root / "replacement.aaproj")["project_id"] == "history-replacement"
    fmt, expected = read_float_wav(root / "input.wav")
    assert fmt == (1, 16000, 32) and len(expected) == 513 * 4
    archives = {}
    for file in (root / "run-logs").glob("*/events.jsonl"):
        events = [json.loads(line) for line in file.read_text(encoding="utf-8").splitlines()]
        assert events and len({e["request_id"] for e in events}) == 1
        archives[events[0]["request_id"]] = (file, events)
    runs = []
    for file in (root / "runs").glob("*/run.json"):
        record = read(file); run = file.parent
        assert record["request_id"] in (old, current)
        actual_fmt, actual = read_float_wav(run / "output.wav")
        assert actual_fmt == fmt and actual == expected
        assert record["frames"] == record["valid_frame_count"] == 513
        if record["request_id"] == current:
            assert Path(demo["preview"]).resolve() == (run / "output.wav").resolve()
        assert len(record["task_artifacts"]) == 1
        task = record["task_artifacts"][0]; p = task["provenance"]
        source = io_path(run / task["directory"] / p["code_snapshot"] / "user_source.py").read_bytes()
        assert io_path(run / p["frozen_package_root"] / "user_source.py").read_bytes() == source
        digest = hashlib.sha256(source).hexdigest()
        assert digest == p["code_files"]["user_source.py"] and b"history-entered.txt" in source
        archive, events = archives[record["request_id"]]
        assert all(e["project_id"] == record["project_id"] and e["revision"] == record["project_revision"] for e in events)
        stdout = [e for e in events if e["source"] == "stdout"]
        assert stdout and all(e["level"] == "info" for e in stdout)
        raw_paths = {Path(e["raw_log_file"]).resolve() for e in stdout}; assert len(raw_paths) == 1
        raw_path = next(iter(raw_paths)); assert raw_path.is_relative_to(root / "run-logs")
        raw = io_path(raw_path).read_bytes()
        assert raw == b"aaaaaaa\r\naaaaaaa\r\n" == "".join(e["text"] for e in stdout).encode()
        assert stdout[-1]["total_bytes"] == len(raw)
        runs.append({"request": record["request_id"], "run": str(run.relative_to(root)), "archive": str(archive.relative_to(root)), "source_sha256": digest, "max_error": 0})
    assert len(runs) == 2 and {r["request"] for r in runs} == {old, current}
    cancelled = set(archives) - {old, current}; assert len(cancelled) == 1
    cancelled_events = archives[next(iter(cancelled))][1]
    assert any(e["source"] == "diagnostic" and "run_cancelled" in e["text"] for e in cancelled_events)
    valid_bytes = (fixture / "valid/events.jsonl").read_bytes()
    rows = [json.loads(line) for line in valid_bytes.splitlines()]
    assert len(valid_bytes) > 2 * 1024 * 1024 and len(rows) == 7000
    assert [r["sequence"] for r in rows] == list(range(1, 7001))
    reader = read(fixture / "reader-result.json")
    assert reader["owned_bytes"] <= 256 * 1024 and 1 < reader["first_sequence"] <= reader["last_sequence"] == 7000
    assert reader["events"] == reader["last_sequence"] - reader["first_sequence"] + 1
    mixed = (fixture / "mixed/events.jsonl").read_bytes()
    assert mixed.startswith(valid_bytes) and json.loads(mixed[len(valid_bytes):])["request_id"] == "different-request"
    try:
        read(fixture / "broken/events.jsonl")
    except json.JSONDecodeError:
        pass
    else:
        raise AssertionError("broken fixture was not malformed")
    report = {"status": "passed", "root": str(root), "reader_root": str(fixture), "runs": runs, "reader": reader,
              "cancelled_request": next(iter(cancelled)), "preview": demo["preview"], "stale_history_ui_empty": True,
              "scope": "logged Run browsing/component and bounded worker reader; no OS desktop or logs-free Run history claim"}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "runs": 2, "reader_rows": len(rows), "max_error": 0}))


if __name__ == "__main__":
    main()
