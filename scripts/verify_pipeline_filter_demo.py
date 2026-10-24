#!/usr/bin/env python3
"""Independently verify the saved PC-04 node-filter demo artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct


def read_float_wav(path: pathlib.Path) -> tuple[tuple[int, int, int], bytes]:
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"not a RIFF/WAVE file: {path}")
    offset = 12
    fmt = None
    samples = None
    while offset + 8 <= len(data):
        tag = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", chunk)
        elif tag == b"data":
            samples = chunk
        offset += 8 + size + (size & 1)
    if fmt is None or samples is None:
        raise ValueError(f"missing fmt/data chunk: {path}")
    format_tag, channels, rate, byte_rate, block_align, bits = fmt
    if format_tag != 3 or bits != 32 or byte_rate != rate * block_align:
        raise ValueError(f"expected IEEE float32 WAV: {path}")
    return (channels, rate, bits), samples


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("demo_root", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()
    root = args.demo_root.resolve()
    demo = json.loads((root / "filter-demo.json").read_text(encoding="utf-8"))
    project_path = root / demo["project_file"]
    project = json.loads(project_path.read_text(encoding="utf-8"))
    node_ids = demo["node_ids"]
    if len(node_ids) != 2 or node_ids[0] == node_ids[1]:
        raise ValueError("demo must contain two distinct node IDs")
    nodes = {node["id"]: node for node in project["nodes"]}
    if any(node_id not in nodes for node_id in node_ids):
        raise ValueError("saved project does not contain both filtered nodes")
    if len(project["connections"]) != 3:
        raise ValueError("saved demo project must contain the File → Print → Print → Output chain")

    all_log = (root / "filter-all-nodes.log").read_text(encoding="utf-8")
    selected_logs = [
        (root / "filter-first-node.log").read_text(encoding="utf-8"),
        (root / "filter-second-node.log").read_text(encoding="utf-8"),
    ]
    request_id = demo["request_id"]
    if request_id not in all_log or any(request_id not in value for value in selected_logs):
        raise ValueError("filtered UI text lost the current Run request identity")
    if not all(node_id in all_log for node_id in node_ids):
        raise ValueError("all-node view does not contain both node identities")
    for index, text in enumerate(selected_logs):
        if node_ids[index] not in text or node_ids[1 - index] in text:
            raise ValueError("a node-filtered view contains the wrong node identity")
        if text.count("aaaaaaa") != demo["expected_print_records_per_node"]:
            raise ValueError("filtered node record count does not match the saved demo contract")
    if all_log.count("aaaaaaa") != 2 * demo["expected_print_records_per_node"]:
        raise ValueError("all-node view does not contain both nodes' records")

    matching_runs = []
    for run_dir in (root / "runs").iterdir():
        run_record_path = run_dir / "run.json"
        output_path = run_dir / "output.wav"
        if not run_record_path.is_file() or not output_path.is_file():
            continue
        record = json.loads(run_record_path.read_text(encoding="utf-8"))
        if record.get("request_id") == request_id:
            matching_runs.append((record, output_path))
    if len(matching_runs) != 1:
        raise ValueError(f"expected one completed run receipt for {request_id}, found {len(matching_runs)}")
    run_record, output_path = matching_runs[0]
    input_info, input_samples = read_float_wav(root / "input.wav")
    output_info, output_samples = read_float_wav(output_path)
    if run_record.get("frames") != demo["samples"] or input_info != output_info or input_samples != output_samples:
        raise ValueError("run output is not a sample-identical copy of the demo input")
    if run_record.get("output_sha256") != sha256(output_path):
        raise ValueError("run receipt output SHA-256 does not match the WAV file")

    events = []
    for event_file in (root / "run-logs").glob("*/events.jsonl"):
        for line in event_file.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event.get("request_id") == request_id:
                events.append(event)
    if {event.get("node_id") for event in events} != set(node_ids):
        raise ValueError("archived events do not contain exactly the two demo node identities")
    raw_checks = []
    for node_id in node_ids:
        node_events = [event for event in events if event.get("node_id") == node_id]
        if not node_events or sum(event.get("total_bytes", 0) for event in node_events) != 18:
            raise ValueError(f"unexpected raw byte count for node {node_id}")
        raw_text = b"".join(event.get("text", "").encode("utf-8") for event in node_events)
        raw_path = pathlib.Path(node_events[0]["raw_log_file"])
        raw_bytes = raw_path.read_bytes()
        if raw_text != raw_bytes:
            raise ValueError(f"archived event text differs from raw stdout for node {node_id}")
        raw_checks.append({"node_id": node_id, "raw_bytes": len(raw_bytes), "raw_sha256": hashlib.sha256(raw_bytes).hexdigest()})

    result = {
        "status": "passed",
        "scope": "independent_saved_demo_artifact_check",
        "request_id": request_id,
        "project_sha256": sha256(project_path),
        "nodes": node_ids,
        "connections": len(project["connections"]),
        "frames": run_record["frames"],
        "sample_rate_hz": run_record["sample_rate_hz"],
        "audio_data_identical": True,
        "output_wav_sha256": sha256(output_path),
        "all_view_record_count": all_log.count("aaaaaaa"),
        "filtered_record_count_per_node": [text.count("aaaaaaa") for text in selected_logs],
        "raw_log_checks": raw_checks,
    }
    serialized = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(serialized, encoding="utf-8", newline="")
    print(serialized, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
