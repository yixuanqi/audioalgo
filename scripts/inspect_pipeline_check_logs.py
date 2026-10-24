"""Bind the saved draft bytes to the separately displayed check log."""
import hashlib
import json
import os
import re
from pathlib import Path
from inspect_pipeline_root_save import fields

base = Path(__file__).resolve().parents[1] / "build/reports/loops/PC0201-print/artifacts"
if os.name == "nt":
    base = Path("\\\\?\\" + str(base))
log = max(base.glob("*/check-ui-log.txt"), key=lambda p: p.stat().st_mtime_ns)
text = log.read_text(encoding="utf-8")
draft, = list((log.parent / "authoring/drafts").glob("*.aadraft"))
values = fields(draft.read_bytes(), b"AudioAlgoDraft/1\n")
source_hash = hashlib.sha256(values[4]).hexdigest()
declaration_hash = hashlib.sha256(values[3]).hexdigest()
assert source_hash in text and declaration_hash in text
assert values[0].decode() in text and "generation=" + values[1].decode() in text
request, = set(re.findall(r"\[Check ([a-z0-9]+) ", text))
assert (log.parent / "authoring/staging" / request).is_dir()
assert "contract.stdout" in text and text.count("aaaaaaa") == 5
run_logs = [(log.parent / f"ui-log-{n}.txt").read_text(encoding="utf-8") for n in range(2)]
assert all("Check" not in value and request not in value and value.count("aaaaaaa") == 2 for value in run_logs)
print(json.dumps({"scope": "development_subset", "draft": values[0].decode(), "generation": int(values[1]),
                  "source_sha256": source_hash, "declaration_sha256": declaration_hash, "check_request": request,
                  "check_calls": 5, "each_canvas_run_calls": 2, "separate_identities": True}, indent=2))
