"""Independently inspect complete raw logs from real supervised byte limits."""
import hashlib
import json
import os
from pathlib import Path

base = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0401-log-budget/artifacts"
if os.name == "nt":
    base = Path("\\\\?\\" + str(base))
receipt = max(base.glob("*/receipt.jsonl"), key=lambda p: p.stat().st_mtime_ns)
rows = [json.loads(line) for line in receipt.read_text().splitlines()]
assert len(rows) == 5
limit = 16 * 1024 * 1024
result = []
for row in rows:
    parent = Path(row["log_directory"])
    if os.name == "nt" and not str(parent).startswith("\\\\?\\"):
        parent = Path("\\\\?\\" + str(parent))
    stdout, stderr = (parent / "stdout.log").read_bytes(), (parent / "stderr.log").read_bytes()
    assert len(stdout) + len(stderr) == limit
    assert stdout == b"x" * len(stdout) and stderr == b"y" * len(stderr)
    assert row["stdout_bytes"] + row["stderr_bytes"] >= limit
    assert row["limit_exceeded"] == (row["mode"] not in ("logbytes-16777216", "logboth-16777216"))
    result.append({"mode": row["mode"], "retained_bytes": len(stdout) + len(stderr), "limit_exceeded": row["limit_exceeded"],
                   "stdout_sha256": hashlib.sha256(stdout).hexdigest(), "stderr_sha256": hashlib.sha256(stderr).hexdigest()})
print(json.dumps({"scope": "development_subset", "receipt": str(receipt), "cases": result}, indent=2))
