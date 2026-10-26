"""Independently verify the persisted logs and source snapshots for a Check cancel/retry demo."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact_root", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--deadline-ms", type=int, help="Verify a timed-out Check's recorded per-task deadline")
    args = parser.parse_args()
    root = args.artifact_root.resolve()
    staging = root / "work" / "staging"
    if not staging.is_dir():
        raise SystemExit(f"missing authoring staging directory: {staging}")

    observed: list[dict[str, object]] = []
    for request in sorted(staging.iterdir()):
        if not request.is_dir():
            continue
        source = request / "candidate" / "user_source.py"
        if not source.is_file():
            continue
        source_bytes = source.read_bytes()
        streams = sorted(request.glob("python-log-*/stdout.log"))
        raw = b"".join(stream.read_bytes() for stream in streams if stream.is_file())
        observed.append({
            "request_id": request.name,
            "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
            "source": source_bytes.decode("utf-8", errors="strict"),
            "stdout_logs": [
                {"path": str(stream.relative_to(root).as_posix()), "bytes": stream.stat().st_size,
                 "sha256": sha256(stream)}
                for stream in streams if stream.is_file()
            ],
            "stdout": raw,
        })
        if args.deadline_ms is not None:
            metadata = json.loads((request / "check-request.json").read_text(encoding="utf-8"))
            if metadata != {"request_id": request.name, "execution_timeout_ms": args.deadline_ms}:
                raise SystemExit("Check request deadline/identity does not match the independent expectation")
            observed[-1]["request_metadata"] = metadata

    cancelled = [row for row in observed if "pc03-check-processes.txt" in str(row["source"])
                 and b"pc03-check-entered" in row["stdout"]]
    recovered = [row for row in observed if "pc03-check-recovered" in str(row["source"])
                 and b"pc03-check-recovered" in row["stdout"]]
    if len(cancelled) != 1 or len(recovered) != 1 or cancelled[0]["request_id"] == recovered[0]["request_id"]:
        raise SystemExit("expected exactly one cancelled Check log/source and one distinct successful recovery log/source")

    result = {
        "schema_version": 1,
        "verified": True,
        "artifact_root": str(root),
        "timed_out_check" if args.deadline_ms is not None else "cancelled_check": {
            key: value for key, value in cancelled[0].items() if key != "stdout" and key != "source"},
        "recovered_check": {key: value for key, value in recovered[0].items() if key != "stdout" and key != "source"},
        "limits": ["Process handle exit state and cancel latency are asserted by the production-component test, not inferred from these files.",
                   "The saved raw stdout logs independently confirm the user-code markers for the cancelled and recovered Check requests."],
    }
    report = args.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"verified": True, "cancelled_request": cancelled[0]["request_id"],
                      "recovered_request": recovered[0]["request_id"], "report": str(report)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
