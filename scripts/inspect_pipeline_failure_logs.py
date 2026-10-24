"""Read failed Run log archives independently of the product and UI."""
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    base = ROOT / "build/reports/loops/PC0301-run-log/artifacts"
    if os.name == "nt":
        base = Path("\\\\?\\" + str(base))
    chosen = max(base.glob("*/failed-ui-log.txt"), key=lambda p: p.stat().st_mtime_ns).parent
    project = json.loads((chosen / "project.aaproj").read_text())
    custom, = [v for v in project["nodes"] if v["type_id"] == "user.pc02_print"]
    successes = {json.loads(p.read_text())["request_id"] for p in chosen.glob("runs/*/run.json")}
    assert len(successes) == 3
    archives = list(chosen.glob("run-logs/*/events.jsonl"))
    assert len(archives) == 4
    failed = []
    for archive in archives:
        rows = [json.loads(line) for line in archive.read_text(encoding="utf-8").splitlines()]
        for row in rows:
            assert row["project_id"] == project["project_id"]
            assert row["node_id"] == custom["id"] and row["type_id"] == custom["type_id"]
            assert row["content_sha256"] == custom["managed_version"]["content_sha256"]
        if rows[0]["request_id"] not in successes:
            failed.append(rows)
    failure, = failed
    assert failure[0]["text"].splitlines() == ["aaaaaaa"]
    assert any(r["level"] == "error" and "pc03 failure after print" in r["text"] for r in failure), "Failed Run must archive its terminal diagnostic as well as stdout"
    print(json.dumps({"scope": "development_subset", "successful_runs": len(successes), "failed_request": failure[0]["request_id"],
                      "exact_hash": custom["managed_version"]["content_sha256"], "archives": len(archives), "failure_has_no_success_manifest": True}, indent=2))


if __name__ == "__main__":
    main()
