"""Create the explicit AR-01..20 case inventory from the versioned acceptance table."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ACCEPTANCE = ROOT / "doc" / "testing_architecture_refactor.md"
OUTPUT = ROOT / "tests" / "architecture_refactor_manifest.json"
STAGE_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6"]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_cases(document: str) -> tuple[dict[str, str], dict[str, str]]:
    stages: dict[str, str] = {}
    legacy: dict[str, str] = {}
    for line in document.splitlines():
        if not line.startswith("| R"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        stage, declaration, old = cells[:3]
        for number, first, last in re.findall(r"AR-(\d+)\.([a-z])～([a-z])", declaration):
            for letter_code in range(ord(first), ord(last) + 1):
                case_id = f"AR-{number}.{chr(letter_code)}"
                stages[case_id] = stage
                legacy[case_id] = old
        for number, letter in re.findall(r"AR-(\d+)\.([a-z])(?=、|$)", declaration):
            case_id = f"AR-{number}.{letter}"
            stages[case_id] = stage
            legacy[case_id] = old
    return stages, legacy


def main() -> None:
    document = ACCEPTANCE.read_text(encoding="utf-8")
    stages, legacy = stage_cases(document)
    details: dict[str, str] = {}
    for match in re.finditer(r"^### AR-(\d+) ([^\n]+)\n(.*?)(?=^### AR-|\Z)", document, re.M | re.S):
        number, _title, body = match.groups()
        for letter, description in re.findall(r"^- ([a-z])：([^\n]+)", body, re.M):
            details[f"AR-{number}.{letter}"] = description.strip()

    registered = ({f"AR-03.{letter}" for letter in "abcdefg"} |
                  {f"AR-04.{letter}" for letter in "abcd"} |
                  {f"AR-05.{letter}" for letter in "abcd"} |
                  {"AR-06.a", "AR-06.b", "AR-06.c", "AR-06.d", "AR-07.a"})
    pure = {f"AR-03.{letter}" for letter in "abcdef"}
    verifier_cases = {
        "AR-02.a": ("test_manifest_exact_110_case_coverage_and_hash", "test_missing_duplicate_and_document_hash_fail_closed"),
        "AR-02.b": ("test_zero_tests_missing_registration_and_unexecuted_are_not_passed",
                    "test_wrong_configuration_binary_and_crashed_process_are_rejected",
                    "test_later_stage_does_not_pass_when_required_case_is_missing"),
        "AR-02.c": ("test_failure_and_skip_are_not_passed", "test_timeout_failure_is_distinct_and_not_passed"),
        "AR-02.d": ("test_missing_expired_wrong_hash_and_forged_evidence_are_rejected",),
        "AR-02.e": ("test_main_gate_inventory_keeps_quality_build_and_loop_requirements",),
        "AR-02.f": ("test_report_shape_requires_timestamp_source_commands_and_case_reasons",),
        "AR-02.g": ("test_failure_probe_proof_requires_red_gate_and_clean_restore",),
    }
    baseline_cases = {
        f"AR-01.{letter}": (f"test_{letter}_{description}",) for letter, description in (
            ("a", "current_environment_configs_and_real_baselines"),
            ("b", "frozen_fixture_and_independent_cli_oracle"),
            ("c", "every_legacy_registration_maps_without_dropping_ned_desktop"),
            ("d", "fresh_build_commands_bind_current_sources_and_binary_hashes"),
        )
    }
    infrastructure_cases = {**verifier_cases, **baseline_cases}
    cases = []
    for case_id in sorted(stages, key=lambda value: (int(value[3:5]), value[-1])):
        parent = case_id[:5]
        layer = "unassigned"
        if parent == "AR-01":
            layer = {"a": "build_consumer", "b": "native_integration", "c": "infrastructure", "d": "build_consumer"}[case_id[-1]]
        elif parent == "AR-02":
            layer = "infrastructure"
        elif parent == "AR-03":
            layer = "pure" if case_id in pure else "native_integration"
        elif case_id == "AR-04.a":
            layer = "build_consumer"
        elif case_id in {"AR-04.b", "AR-04.c", "AR-04.d"}:
            layer = "build_consumer"
        elif case_id == "AR-05.a":
            layer = "pure"
        elif case_id == "AR-05.b":
            layer = "native_integration"
        elif case_id == "AR-05.c":
            layer = "native_integration"
        elif case_id == "AR-05.d":
            layer = "native_integration"
        elif case_id == "AR-06.a":
            layer = "native_integration"
        elif case_id == "AR-06.b":
            layer = "native_integration"
        elif case_id == "AR-06.c":
            layer = "native_integration"
        elif case_id == "AR-06.d":
            layer = "native_integration"
        elif case_id == "AR-07.a":
            layer = "native_integration"
        cases.append({
            "id": case_id,
            "stage": stages[case_id],
            "required": True,
            "layer": layer,
            "runner": "gate_failure_probe" if case_id == "AR-02.g" else
                      "ctest" if case_id in registered or case_id in infrastructure_cases else "unassigned",
            "test_names": [case_id] if case_id in registered or case_id in infrastructure_cases else [],
            "test_methods": list(infrastructure_cases[case_id]) if case_id in infrastructure_cases else [],
            "evidence_requirements": [details.get(case_id, "Exact behavior is defined in the acceptance document")],
            "legacy_requirements": [legacy[case_id]],
            "desktop_required": any(term in details.get(case_id, "") for term in ("真实桌面", "桌面完成", "桌面证据", "截图与", "桌面交互")),
        })
    if len(cases) != 110 or set(stages) != {case["id"] for case in cases}:
        raise SystemExit(f"acceptance expansion mismatch: found {len(cases)} cases")
    manifest = {
        "schema_version": 1,
        "acceptance_source": "doc/testing_architecture_refactor.md",
        "acceptance_document_sha256": sha(ACCEPTANCE),
        "test_name_pattern": "{requirement_id}.{subcase_id}",
        "stages": STAGE_ORDER,
        "legacy_sources": ["doc/testing_audio_pipeline_editor.md", "doc/testing_editor_node_interaction.md"],
        "requirements": cases,
    }
    OUTPUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(cases)} required subcases: {OUTPUT}")


if __name__ == "__main__":
    main()
