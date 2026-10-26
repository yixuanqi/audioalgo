"""Read PC0405's owned JUnit root, archives, frozen code and float WAVs independently."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path

NAME = "PC-04.b production flood enforces complete and UI budgets while remaining responsive and recovering"
BUDGET = 16 * 1024 * 1024


def read_json(path):
    return json.loads(io_path(path).read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    case = next(c for c in ET.parse(args.junit).iter("testcase") if c.attrib["name"] == NAME)
    assert case.find("failure") is None and case.find("error") is None
    line = next(s for s in case.findtext("system-out", "").splitlines() if s.startswith("PC0405_ARTIFACT_ROOT="))
    root = Path(line.split("=", 1)[1]).resolve()
    managed = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0405-live-flood/artifacts"
    assert root.parent == managed.resolve(), root
    receipts = read_json(root / "flood-demo.json")
    assert [r["mode"] for r in receipts] == [1, 2, 3]
    heartbeat = receipts[0]
    assert heartbeat["window_ms"] >= 10000 and heartbeat["interval_ms"] == 20
    assert heartbeat["p95_ms"] <= 100 and heartbeat["max_ms"] <= 500
    for r in receipts:
        assert r["maximum_ui_bytes"] <= 256 * 1024
        ui = (root / f'ui-{r["mode"]}.log').read_text(encoding="utf-8")
        assert len(ui.encode()) <= 256 * 1024
        assert "stream tail discarded" in ui and "pc04-flood" in ui
        assert "Completed run" in r["terminal"] if r["mode"] == 1 else "log_limit" in r["terminal"]
        if r["mode"] != 1:
            assert r["window_ms"] <= 5000
    archives = []
    for path in (root / "run-logs").glob("*/events.jsonl"):
        events = [json.loads(s) for s in path.read_text(encoding="utf-8").splitlines()]
        streams = [e for e in events if e["source"] == "stdout"]
        if not streams:
            continue
        revisions = {e["revision"] for e in events}
        requests = {e["request_id"] for e in events}
        hashes = {e["content_sha256"] for e in events}
        assert len(revisions) == len(requests) == len(hashes) == 1
        assert all(e["node_id"] == "custom" and e["type_id"] == "user.custom_node" and e["project_id"] == "pc04-flood" for e in events)
        assert all(b["sequence"] > a["sequence"] for a, b in zip(streams, streams[1:]))
        assert all(b["total_bytes"] >= a["total_bytes"] for a, b in zip(streams, streams[1:]))
        raw_paths = {Path(e["raw_log_file"]).resolve() for e in streams}
        assert len(raw_paths) == 1
        raw_path = next(iter(raw_paths))
        assert raw_path.is_relative_to(root / "run-logs")
        raw = io_path(raw_path).read_bytes()
        assert len(raw) == BUDGET and raw == b"F" * BUDGET
        assert streams[-1]["discarded_bytes"] == streams[-1]["total_bytes"] - 65536
        assert sum(len(e["text"].encode()) + e["dropped_text_bytes"] for e in streams) == streams[-1]["total_bytes"]
        revision = next(iter(revisions))
        diagnostics = [e for e in events if e["source"] == "diagnostic"]
        assert (not diagnostics) if revision == 1 else any("log_limit" in e["text"] for e in diagnostics)
        assert streams[-1]["total_bytes"] == BUDGET if revision == 1 else streams[-1]["total_bytes"] > BUDGET
        if revision == 2:
            assert streams[-1]["total_bytes"] == BUDGET + 1
        archives.append({"revision": revision, "request": next(iter(requests)), "content_sha256": next(iter(hashes)),
                         "raw_bytes": len(raw), "observed_bytes": streams[-1]["total_bytes"], "raw_sha256": hashlib.sha256(raw).hexdigest(),
                         "events": len(events), "archive": str(path.relative_to(root))})
    archives.sort(key=lambda a: a["revision"])
    assert [a["revision"] for a in archives] == [1, 2, 4]
    fmt, expected = read_float_wav(root / "input.wav")
    assert fmt == (1, 16000, 32) and len(expected) == 513 * 4
    successes = []
    source_hashes = set()
    for path in (root / "runs").glob("*/run.json"):
        record = read_json(path)
        run = path.parent
        actual_fmt, actual = read_float_wav(run / "output.wav")
        assert actual_fmt == fmt and actual == expected
        assert record["frames"] == 513 and record["valid_frame_count"] == 513
        for task in record["task_artifacts"]:
            provenance = task["provenance"]
            source = run / task["directory"] / provenance["code_snapshot"] / "user_source.py"
            frozen = run / provenance["frozen_package_root"] / "user_source.py"
            source_bytes = io_path(source).read_bytes()
            digest = hashlib.sha256(source_bytes).hexdigest()
            assert digest == provenance["code_files"]["user_source.py"]
            assert io_path(frozen).read_bytes() == source_bytes
            assert b"range(4096)" in source_bytes and b"os.write(1,b'X')" in source_bytes
            source_hashes.add(digest)
            logs = provenance["logs"]
            if record["project_revision"] == 1:
                assert logs["stdout_bytes"] == BUDGET and logs["stdout_discarded_bytes"] == BUDGET - 65536
                assert logs["stdout_tail"] == "F" * 65536 and logs["stderr_bytes"] == 0 and not logs["limit_exceeded"]
            else:
                assert logs["stdout_bytes"] == logs["stderr_bytes"] == 0
        successes.append({"revision": record["project_revision"], "request": record["request_id"], "run": str(run.relative_to(root)), "max_error": 0})
    successes.sort(key=lambda a: a["revision"])
    assert [a["revision"] for a in successes] == [1, 3, 5] and len(source_hashes) == 1
    assert {a["request"] for a in archives[1:]}.isdisjoint(a["request"] for a in successes)
    report = {"status": "passed", "root": str(root), "heartbeat": heartbeat, "archives": archives, "successes": successes,
              "executed_source_sha256": next(iter(source_hashes)), "limits": {"raw_task_bytes": BUDGET, "stream_tail_bytes": 65536, "ui_bytes": 262144},
              "scope": "component/real processes/artifacts; OS desktop and full 20-round queue exit not covered"}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "archives": len(archives), "outputs": len(successes), "max_error": 0}))


if __name__ == "__main__":
    main()
