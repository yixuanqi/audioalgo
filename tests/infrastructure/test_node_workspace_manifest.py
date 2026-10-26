#!/usr/bin/env python3
"""Fail-closed W0 checks for the custom node workspace contract.

Checks performed (all required):

* ``tests/node_workspace_manifest.json`` declares exactly the 30 NWS-01.a to
  NWS-10.c clauses that ``doc/testing_node_workspace_authoring.md`` contains,
  each with the frozen stage, a required flag, a layer, legacy NA/PC leads and a
  planned or bound test name.
* The manifest cannot claim progress it did not prove: a ``passed`` clause must
  name executed tests with existing evidence files, and a ``partial`` clause
  must name a passed execution plus its remaining clauses.
* The acceptance document, the ADR, the fixture tool and the fixture lock are
  bound by SHA-256; editing any of them without refreshing the binding fails.
* The independent fixture tool ``--check`` passes, so the frozen fixture bytes,
  the legacy draft and the expected manifests/hashes are internally consistent.
* The frozen budget numbers in the ADR, the lock and this file agree.

Evidence files live under the git-ignored ``build/reports`` tree by repository
convention. When that whole tree is absent (a clean checkout that has not run the
suite) this checker only verifies the reproducible contract structure and says so;
it never promotes a status. When the tree exists, every claimed evidence file
must exist, so a run that lost its evidence fails closed.

Real OS desktop items may only be ``not_verified``; component evidence can never
be reported as desktop evidence.
"""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests" / "node_workspace_manifest.json"
ACCEPTANCE = ROOT / "doc" / "testing_node_workspace_authoring.md"
ADR = ROOT / "doc" / "decisions" / "0040-node-workspace-schema-v1.md"
TOOL = ROOT / "tests" / "fixtures" / "node_workspace" / "tools" / "workspace_expectation.py"
LOCK = ROOT / "tests" / "fixtures" / "node_workspace" / "expected" / "workspace_lock.json"
EVIDENCE_ROOT = ROOT / "build" / "reports"

EXPECTED_IDS = [f"NWS-{group:02d}.{case}" for group in range(1, 11) for case in "abc"]
FROZEN_STAGES = {
    "NWS-01.a": "W1", "NWS-01.b": "W1", "NWS-01.c": "W1",
    "NWS-02.a": "W1", "NWS-02.b": "W1", "NWS-02.c": "W2",
    "NWS-03.a": "W2", "NWS-03.b": "W2", "NWS-03.c": "W2",
    "NWS-04.a": "W2", "NWS-04.b": "W2", "NWS-04.c": "W2",
    "NWS-05.a": "W3", "NWS-05.b": "W3", "NWS-05.c": "W3",
    "NWS-06.a": "W3", "NWS-06.b": "W3", "NWS-06.c": "W3",
    "NWS-07.a": "W4", "NWS-07.b": "W4", "NWS-07.c": "W4",
    "NWS-08.a": "W4", "NWS-08.b": "W5", "NWS-08.c": "W5",
    "NWS-09.a": "W2", "NWS-09.b": "W4", "NWS-09.c": "W5",
    "NWS-10.a": "W5", "NWS-10.b": "W5", "NWS-10.c": "W5",
}
FROZEN_BUDGETS = {
    "node_json_bytes": 1024 * 1024,
    "source_files": 128,
    "source_bytes": 8 * 1024 * 1024 - 16 * 1024,
    "resource_files": 64,
    "resource_bytes": 64 * 1024 * 1024,
    "relative_path_bytes": 512,
}
STATUSES = {"not_run", "partial", "passed", "failed", "blocked", "not_verified", "skipped", "timeout"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    problems: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            problems.append(message)

    for path in (MANIFEST, ACCEPTANCE, ADR, TOOL, LOCK):
        require(path.is_file(), f"missing required W0 artifact {path.relative_to(ROOT)}")
    if problems:
        return report(problems)

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    acceptance_text = ACCEPTANCE.read_text(encoding="utf-8")
    adr_text = ADR.read_text(encoding="utf-8")

    require(manifest.get("schema_version") == 1, "manifest schema_version must be 1")
    require(manifest.get("required_subcase_count") == len(EXPECTED_IDS),
            f"required_subcase_count must be {len(EXPECTED_IDS)}")
    require(manifest.get("scope") == "node_workspace", "manifest scope must be node_workspace")

    bindings = {
        "acceptance_source": (ACCEPTANCE, "acceptance_source_sha256"),
        "contract_adr": (ADR, "contract_adr_sha256"),
        "fixture_tool": (TOOL, "fixture_tool_sha256"),
        "fixture_lock": (LOCK, "fixture_lock_sha256"),
    }
    for key, (path, field) in bindings.items():
        recorded = manifest.get(field)
        require(recorded == sha256(path),
                f"{field} does not match {path.relative_to(ROOT)}; refresh the manifest binding deliberately")
        require(manifest.get(key) == path.relative_to(ROOT).as_posix(),
                f"manifest {key} must point at {path.relative_to(ROOT).as_posix()}")

    # The acceptance document itself must still enumerate exactly these clauses.
    documented = re.findall(r"\*\*(NWS-\d{2}\.[abc])\*\*", acceptance_text)
    require(sorted(documented) == sorted(EXPECTED_IDS),
            f"acceptance document enumerates {sorted(set(EXPECTED_IDS) - set(documented))} missing and "
            f"{sorted(set(documented) - set(EXPECTED_IDS))} unexpected clause ids")
    require(len(documented) == len(set(documented)), "acceptance document repeats a clause id")

    requirements = manifest.get("requirements", [])
    require(isinstance(requirements, list), "requirements must be a list")
    ids = [item.get("id") for item in requirements]
    require(ids == EXPECTED_IDS, f"requirements must be ordered {EXPECTED_IDS[0]}..{EXPECTED_IDS[-1]}")
    require(len(ids) == len(set(ids)), "duplicate requirement id")
    evidence_tree = EVIDENCE_ROOT.is_dir()

    for item in requirements:
        clause = item.get("id", "<missing>")
        stage = item.get("stage")
        require(stage == FROZEN_STAGES.get(clause), f"{clause}: stage {stage!r} is not the frozen mapping")
        require(item.get("required") is True, f"{clause}: must stay required")
        require(bool(item.get("layer")), f"{clause}: layer is required")
        require(isinstance(item.get("lead_na_pc"), list) and item["lead_na_pc"], f"{clause}: lead NA/PC mapping is required")
        require(bool(item.get("planned_tests") or item.get("test_names")), f"{clause}: needs planned or bound tests")
        status = item.get("status")
        require(status in STATUSES, f"{clause}: unknown status {status!r}")
        remaining = item.get("remaining_clauses")
        require(isinstance(remaining, list) and remaining, f"{clause}: remaining_clauses must list the uncovered clauses")
        results = item.get("test_results", [])
        names = item.get("test_names", [])
        require(isinstance(results, list), f"{clause}: test_results must be a list")
        for result in results:
            evidence = result.get("evidence", "")
            require(bool(evidence), f"{clause}: a result record needs an evidence path")
            if evidence_tree:
                require((ROOT / evidence).is_file(),
                        f"{clause}: evidence file {evidence!r} does not exist")
            require(result.get("status") in STATUSES, f"{clause}: bad result status {result.get('status')!r}")
            require(result.get("test_name") in names,
                    f"{clause}: result {result.get('test_name')!r} is not listed in test_names")
        if status in ("passed", "partial"):
            passed = [result for result in results if result.get("status") == "passed"]
            require(bool(names), f"{clause}: {status} needs bound test names")
            require(bool(passed), f"{clause}: {status} needs at least one passed execution record")
        if status == "passed":
            require(not remaining, f"{clause}: a fully passed clause may not keep remaining clauses")
        if status == "not_run":
            require(not results, f"{clause}: not_run must not carry execution results")
        if stage == "W5" and clause in ("NWS-10.a", "NWS-10.b"):
            require(status in ("not_run", "not_verified", "partial"),
                    f"{clause}: desktop evidence is paused, component tests cannot make it passed")

    require(lock.get("algorithm") == "AudioAlgoNodeWorkspace/1", "lock algorithm must stay AudioAlgoNodeWorkspace/1")
    require(lock.get("adr") == "doc/decisions/0040-node-workspace-schema-v1.md", "lock must bind the ADR")
    require(lock.get("budgets") == FROZEN_BUDGETS, "lock budgets drifted from the frozen values")
    for name, workspace in lock.get("workspaces", {}).items():
        require(len(workspace.get("runtime_files", [])) > 0, f"{name}: empty runtime manifest")
        require(workspace.get("content_sha256", "").__len__() == 64, f"{name}: content_sha256 missing")
        require("node.json" in [entry["path"] for entry in workspace["runtime_files"]],
                f"{name}: node.json must be in the runtime manifest")
    require({"python_two_files", "python_two_files_v2", "native_two_sources"} <= set(lock.get("workspaces", {})),
            "lock must contain the three frozen authoring fixtures")
    require(lock.get("legacy", {}).get("bytes", 0) > 0, "legacy draft fixture is missing")
    require(len(lock.get("invalid", [])) >= 6, "at least six frozen invalid fixtures are required")
    require(lock.get("conversion", {}).get("entrypoint", "").endswith(":process"),
            "conversion expectation must fix the generated entrypoint")

    # The ADR must keep the frozen literals so it cannot quietly drift from the lock.
    for literal in ("AudioAlgoNodeWorkspace/1", "1 MiB", "128", "8,372,224", "64 MiB", "67,108,864", "512"):
        require(literal in adr_text, f"ADR no longer documents the frozen literal {literal!r}")

    if not problems:
        completed = subprocess.run([sys.executable, str(TOOL), "--check"], cwd=ROOT,
                                   capture_output=True, text=True)
        print(completed.stdout.strip())
        if completed.returncode != 0:
            problems.append("independent fixture tool --check failed: " + completed.stderr.strip())

    if problems:
        return report(problems)
    counts: dict[str, int] = {}
    for item in requirements:
        counts[item["stage"]] = counts.get(item["stage"], 0) + 1
    print(f"W0 manifest OK: {len(requirements)} NWS clauses, stages "
          + ", ".join(f"{key}={counts[key]}" for key in sorted(counts))
          + f", statuses={sorted({item['status'] for item in requirements})}")
    if evidence_tree:
        print("Every claimed partial/passed execution record has an existing evidence file.")
    else:
        print("Evidence tree build/reports is absent in this checkout: contract structure only; "
              "no status was promoted and claimed evidence was not verified.")
    print("No clause is claimed passed without an executed, evidence-bound test.")
    return 0


def report(problems: list[str]) -> int:
    print("W0 workspace manifest check FAILED", file=sys.stderr)
    for problem in problems:
        print(f"  - {problem}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
