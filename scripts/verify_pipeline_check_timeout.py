"""Independently verify a production Check timeout artifact and recovery run."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect(root: Path, expected_timeout_ms: int) -> dict[str, object]:
    root = root.resolve()
    staging = root / "work" / "staging"
    if not staging.is_dir():
        raise ValueError(f"missing authoring staging directory: {staging}")

    requests: list[dict[str, object]] = []
    for request in sorted(staging.iterdir()):
        if not request.is_dir():
            continue
        metadata_path = request / "check-request.json"
        source_path = request / "candidate" / "user_source.py"
        if not metadata_path.is_file() or not source_path.is_file():
            continue
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if metadata.get("request_id") != request.name:
            raise ValueError(f"request id does not match staging directory: {request}")
        source_bytes = source_path.read_bytes()
        source = source_bytes.decode("utf-8", errors="strict")
        streams = sorted(request.glob("python-log-*/stdout.log"))
        stream_rows = [
            {"path": stream.relative_to(root).as_posix(),
             "bytes": stream.stat().st_size,
             "sha256": sha256(stream.read_bytes())}
            for stream in streams if stream.is_file()
        ]
        stdout = b"".join(stream.read_bytes() for stream in streams if stream.is_file())
        requests.append({
            "request_id": request.name,
            "execution_timeout_ms": metadata.get("execution_timeout_ms"),
            "source": source,
            "source_sha256": sha256(source_bytes),
            "stdout": stdout,
            "stdout_logs": stream_rows,
        })

    timed_out = [row for row in requests
                 if row["execution_timeout_ms"] == expected_timeout_ms
                 and "pc03-check-processes.txt" in str(row["source"])
                 and b"pc03-check-entered" in row["stdout"]]
    recovered = [row for row in requests
                 if "pc03-check-recovered" in str(row["source"])
                 and b"pc03-check-recovered" in row["stdout"]]
    if len(timed_out) != 1 or len(recovered) != 1:
        raise ValueError("expected one timed-out Check request and one distinct successful recovery request")
    if timed_out[0]["request_id"] == recovered[0]["request_id"]:
        raise ValueError("timeout and recovery must use distinct request ids")

    barrier_path = root / "check-process-barrier.txt"
    if not barrier_path.is_file():
        raise ValueError(f"missing test-observed PID barrier: {barrier_path}")
    barrier_lines = barrier_path.read_text(encoding="utf-8").splitlines()
    if len(barrier_lines) < 2:
        raise ValueError("PID barrier record is incomplete")
    parent_pid, child_pid = (int(value) for value in barrier_lines[0].split())
    marker_path = Path(barrier_lines[1])
    if parent_pid <= 0 or child_pid <= 0 or parent_pid == child_pid:
        raise ValueError("PID barrier does not contain two distinct positive process ids")
    if marker_path.name != "pc03-check-processes.txt" or not any(
            part.startswith("python-task-") for part in marker_path.parts):
        raise ValueError("PID barrier path is outside the managed Python task layout")

    return {
        "artifact_root": str(root),
        "timeout_request": {
            "request_id": timed_out[0]["request_id"],
            "execution_timeout_ms": timed_out[0]["execution_timeout_ms"],
            "source_sha256": timed_out[0]["source_sha256"],
            "stdout_logs": timed_out[0]["stdout_logs"],
            "contains_user_marker": True,
        },
        "recovered_request": {
            "request_id": recovered[0]["request_id"],
            "source_sha256": recovered[0]["source_sha256"],
            "stdout_logs": recovered[0]["stdout_logs"],
            "contains_recovery_marker": True,
        },
        "pid_barrier": {
            "parent_pid": parent_pid,
            "child_pid": child_pid,
            "recorded_marker_path": str(marker_path),
        },
        "limits": [
            "This verifier independently reads only request metadata, candidate source, direct request/log children, and the test-owned PID barrier.",
            "Process exit state, timeout latency, visible diagnostic and process-tree ownership are asserted by the production component test, not inferred from these files.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact_roots", nargs="+", type=Path)
    parser.add_argument("--timeout-ms", type=int, default=3000)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    rows = [inspect(root, args.timeout_ms) for root in args.artifact_roots]
    if len({row["timeout_request"]["request_id"] for row in rows}) != len(rows):
        raise SystemExit("artifact roots must contain distinct timeout request ids")
    report = {
        "schema_version": 1,
        "verified": True,
        "expected_timeout_ms": args.timeout_ms,
        "runs": rows,
    }
    destination = args.report.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"verified": True, "runs": len(rows), "report": str(destination)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
