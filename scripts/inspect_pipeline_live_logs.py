"""Read live log archives and the actual ten-second component heartbeat."""
import json
import os
from pathlib import Path

root = Path(__file__).resolve().parents[1]
base = root / "build/reports/loops/PC0201-print/artifacts"
if os.name == "nt":
    base = Path("\\\\?\\" + str(base))
receipt = max(base.glob("*/heartbeat.json"), key=lambda p: p.stat().st_mtime_ns)
timing = json.loads(receipt.read_text())
assert timing["window_ms"] >= 10000 and timing["interval_ms"] == 20
assert timing["p95_ms"] <= 100 and timing["max_ms"] <= 500 and timing["ui_text_bytes"] <= 256 * 1024
parent = receipt.parent
project = json.loads((parent / "project.aaproj").read_text())
custom, = [v for v in project["nodes"] if v["type_id"] == "user.pc02_print"]
successes = {json.loads(p.read_text())["request_id"] for p in parent.glob("runs/*/run.json")}
assert len(successes) == 2
live = []
for path in parent.glob("run-logs/*/events.jsonl"):
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    if any("live-heartbeat-record-" in row["text"] for row in rows):
        live.append(rows)
rows, = live
assert rows[0]["request_id"] not in successes
assert all(r["node_id"] == custom["id"] and r["content_sha256"] == custom["managed_version"]["content_sha256"] for r in rows)
assert all(a["sequence"] < b["sequence"] for a, b in zip(rows, rows[1:]))
stdout = [r for r in rows if r["source"] == "stdout"]
raw_path = Path(stdout[0]["raw_log_file"])
if os.name == "nt" and not str(raw_path).startswith("\\\\?\\"):
    raw_path = Path("\\\\?\\" + str(raw_path))
raw = raw_path.read_bytes()
assert b"live-before-terminal" in raw and b"live-heartbeat-record-" in raw
assert "".join(r["text"] for r in stdout) == raw.decode("utf-8")
assert stdout[-1]["total_bytes"] == len(raw)
assert any(r["source"] == "diagnostic" and "run_cancelled" in r["text"] for r in rows)
ui = (parent / "live-ui-log.txt").read_text(encoding="utf-8")
assert "live-heartbeat-record-" in ui and all(request not in ui for request in successes)
evidence = root / "build/reports/loops/PC0403-ui-bound"
if os.name == "nt":
    evidence = Path("\\\\?\\" + str(evidence))
malformed = [p for p in (evidence / "artifacts").rglob("stdout.log") if p.stat().st_size == 15]
raw_malformed = max(malformed, key=lambda p: p.stat().st_mtime_ns)
assert raw_malformed.read_bytes() == bytes([255, 0, 228]) * 5
cycle_receipt = max((evidence / "cycles").glob("*/receipt.json"), key=lambda p: p.stat().st_mtime_ns)
cycle = json.loads(cycle_receipt.read_text())
assert cycle == {"rounds": 20, "tasks": 60, "live_processes": 0}
contents = [p.read_bytes() for p in cycle_receipt.parent.rglob("stdout.log")]
assert len(contents) == 61 and contents.count(b"") == 1
assert contents.count(b"round-success\r\n") == 20 and contents.count(b"round-error\r\n") == 20
assert contents.count("中文".encode("utf-8")) == 20
print(json.dumps({"scope": "development_subset", "receipt": str(receipt), "timing": timing, "live_request": rows[0]["request_id"],
                  "raw_bytes": len(raw), "stdout_batches": len(stdout), "archive_matches_raw_stream": True,
                  "malformed_raw_bytes": 15, "rounds": cycle["rounds"], "cycle_tasks": cycle["tasks"], "desktop": "not_verified"}, indent=2))
