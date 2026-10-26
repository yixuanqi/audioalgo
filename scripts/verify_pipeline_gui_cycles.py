"""Independently verify all PC0408 GUI-cycle request archives and actual outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path

NAME = "PC-04.c production twenty success error cancel rounds drain queues processes and log views"


def read(path):
    return json.loads(io_path(path).read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    case = next(c for c in ET.parse(args.junit).iter("testcase") if c.attrib["name"] == NAME)
    assert case.find("failure") is None and case.find("error") is None
    line = next(s for s in case.findtext("system-out", "").splitlines() if s.startswith("PC0408_ARTIFACT_ROOT="))
    root = Path(line.split("=", 1)[1]).resolve()
    managed = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0408-gui-cycles/artifacts"
    assert root.parent == managed.resolve()
    tasks, rounds, limits = read(root / "cycles.json"), read(root / "cycles-progress.json"), read(root / "cycle-limits.json")
    assert limits["rounds"] == len(rounds) == 20 and limits["tasks"] == len(tasks) == 81
    expected_order = [(-1, "warmup")] + [(n, phase) for n in range(20) for phase in ("success", "error", "cancel", "recovery")]
    assert [(t["round"], t["phase"]) for t in tasks] == expected_order
    assert len({t["request_id"] for t in tasks}) == 81
    for number, row in enumerate(rounds):
        assert row["round"] == number and row["handles"] <= limits["baseline_handles"]
        assert row["active"] == row["pending"] == row["worker_jobs"] == 0
        assert row["ui_bytes"] <= limits["maximum_ui_bytes"] <= 256 * 1024
    fmt, expected = read_float_wav(root / "input.wav")
    assert fmt == (1, 16000, 32) and len(expected) == 513 * 4
    project = read(root / "project.aaproj")
    node = next(n for n in project["nodes"] if n["id"] == "custom")
    lock = node["managed_version"]["content_sha256"]
    outcomes = {}
    for task in tasks:
        path = (root / task["archive"]).resolve(); assert path.is_relative_to(root / "run-logs")
        events = [json.loads(s) for s in path.read_text(encoding="utf-8").splitlines()]
        assert events and all(e["request_id"] == task["request_id"] and e["project_id"] == "pc04-gui-cycles" and e["node_id"] == "custom" and e["type_id"] == "user.custom_node" and e["content_sha256"] == lock for e in events)
        stdout = [e for e in events if e["source"] == "stdout"]
        assert stdout and all(b["sequence"] > a["sequence"] for a, b in zip(stdout, stdout[1:]))
        paths = {Path(e["raw_log_file"]).resolve() for e in stdout}; assert len(paths) == 1
        raw_path = next(iter(paths)); assert raw_path.is_relative_to(root / "run-logs")
        raw = io_path(raw_path).read_bytes()
        assert raw == "".join(e["text"] for e in stdout).encode() and stdout[-1]["total_bytes"] == len(raw)
        assert all(e["discarded_bytes"] == e["dropped_text_bytes"] == 0 for e in stdout)
        match = re.fullmatch(rb"cycle-log gain=(-?\d+(?:\.\d+)?) pid=(\d+)\r\n", raw); assert match
        expected_gain = -2 if task["phase"] == "cancel" else -1 if task["phase"] == "error" else .5
        assert float(match[1]) == expected_gain and int(match[2]) == task["parent_pid"]
        diagnostics = [e for e in events if e["source"] == "diagnostic"]
        if task["phase"] == "cancel":
            assert task["child_pid"] > 0 and any("run_cancelled" in e["text"] for e in diagnostics)
        elif task["phase"] == "error":
            assert task["child_pid"] == 0 and any("cycle-failure" in e["text"] for e in diagnostics)
        else:
            assert task["child_pid"] == 0 and not diagnostics
        outcomes[task["request_id"]] = task
    outputs, source_hashes = [], set()
    for file in (root / "runs").glob("*/run.json"):
        record = read(file); run = file.parent; task = outcomes[record["request_id"]]
        assert task["phase"] in ("warmup", "success", "recovery")
        actual_fmt, actual = read_float_wav(run / "output.wav")
        assert actual_fmt == fmt and actual == expected and record["frames"] == record["valid_frame_count"] == 513
        assert len(record["task_artifacts"]) == 1
        artifact = record["task_artifacts"][0]; p = artifact["provenance"]
        assert p["process_id"] == task["parent_pid"]
        data = io_path(run / artifact["directory"] / p["code_snapshot"] / "user_source.py").read_bytes()
        digest = hashlib.sha256(data).hexdigest(); assert digest == p["code_files"]["user_source.py"]
        assert io_path(run / p["frozen_package_root"] / "user_source.py").read_bytes() == data
        assert b"state['entered']=True" in data and b"cycle-failure" in data and b"creationflags=0x08000000" in data
        source_hashes.add(digest)
        outputs.append({"request": record["request_id"], "run": str(run.relative_to(root)), "max_error": 0})
    assert len(outputs) == limits["success_outputs"] == 41 and len(source_hashes) == 1
    assert len(list((root / "run-logs").glob("*/events.jsonl"))) == 81
    report = {"status": "passed", "root": str(root), "rounds": 20, "tasks": 81, "errors": 20, "cancelled": 20,
              "baseline_handles": limits["baseline_handles"], "round_end_handles": [r["handles"] for r in rounds], "maximum_ui_bytes": limits["maximum_ui_bytes"],
              "source_sha256": next(iter(source_hashes)), "outputs": outputs, "max_error": 0,
              "scope": "production component repeated Runs plus observed queue/handle/process proof; no real desktop/full platform claim"}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "rounds": 20, "tasks": 81, "outputs": 41, "max_error": 0}))


if __name__ == "__main__":
    main()
