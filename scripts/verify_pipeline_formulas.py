"""Stdlib stage-float32 oracle for real PC-05 GUI/CLI WAV, NPY and code closures."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from verify_python_mixed import f32, io_path, read_wav, sha
from verify_python_snapshot import close, npy, snapshot

BASIC = "PC-05.a production Run and fresh CLI execute both documented mixed formulas"
MATRIX = "PC-05.a GUI CLI mixed formula matrix preserves state across formats frames and host partitions"


def read(path):
    return json.loads(io_path(path).read_text(encoding="utf-8"))


def stages(source, direction):
    ramp = [f32(1e-5 * n) for n in range(len(source[0]))]
    if direction == 0:
        a = [[f32(x * 2) for x in channel] for channel in source]
        b = [[f32(f32(x * .25) + ramp[n]) for n, x in enumerate(channel)] for channel in a]
        c = [[f32(x * .5) for x in channel] for channel in b]
    else:
        a = [[f32(f32(x * .5) + ramp[n]) for n, x in enumerate(channel)] for channel in source]
        b = [[f32(x * 2) for x in channel] for channel in a]
        c = [[f32(x * .25) for x in channel] for channel in b]
    return {"A": a, "B": b, "C": c}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("junit", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--matrix", action="store_true")
    args = parser.parse_args()
    case = next(c for c in ET.parse(args.junit).iter("testcase") if c.attrib["name"] == (MATRIX if args.matrix else BASIC))
    assert case.find("failure") is None and case.find("error") is None
    line = next(s for s in case.findtext("system-out", "").splitlines() if s.startswith("PC05_FORMULA_ROOT="))
    root = Path(line.split("=", 1)[1]).resolve()
    folder = "PC0502-mixed-matrix" if args.matrix else "PC0501-mixed-formulas"
    managed = Path(__file__).resolve().parents[1] / "build/reports/loops" / folder / "artifacts"
    assert root.parent == managed.resolve()
    rows = read(root / "formula-demo.json")
    expected_profiles = {(direction, 48000, 2, 513, 127) for direction in (0, 1)}
    if args.matrix:
        expected_profiles = {(d, sr, ch, frames, 127) for d in (0, 1) for sr in (16000, 48000) for ch in (1, 2, 4) for frames in (1, 513, 16037)}
        expected_profiles |= {(d, 48000, 2, 16037, host) for d in (0, 1) for host in (1, 512, 1024)}
    key = lambda row: tuple(row[k] for k in ("direction", "sample_rate", "channels", "frames", "host"))
    assert {key(row) for row in rows} == expected_profiles
    for profile in expected_profiles:
        actual = {(r["origin"], r["repeat"]) for r in rows if key(r) == profile}
        assert actual == ({("gui", 0), ("cli", 0)} if args.matrix else {("gui", 0), ("gui", 1), ("cli", 0)})
    assert len(rows) == (84 if args.matrix else 6) and len({r["request_id"] for r in rows}) == len(rows)
    command = read(args.junit.with_suffix(".json"))
    hosts = read(root / "host-hashes.json")
    for filename, field in (("audioalgo_tests.exe", "test_host"), ("audio_cli.exe", "cli")):
        assert next(a["sha256"] for a in command["artifacts"] if Path(a["path"]).name == filename) == hosts[field]
    records, source_hashes, code_hashes, runner_hashes = [], set(), set(), set()
    for row in rows:
        direction, rate, channels, frames, host = key(row)
        project_file = (root / row["project_file"]).resolve(); run = (root / row["run"]).resolve()
        assert project_file.is_relative_to(root) and run.is_relative_to(root)
        project = read(project_file); nodes = {n["id"]: n for n in project["nodes"]}
        input_file = project_file.parent / "input.wav"
        input_rate, source = read_wav(input_file)
        assert input_rate == rate and len(source) == channels and all(len(c) == frames for c in source)
        pattern = [[f32(.2 * math.sin(.03 * (n + 1)) + .02 * c) for n in range(frames)] for c in range(channels)]
        close(source, pattern)
        expected = stages(source, direction)
        record = read(run / "run.json"); complete = read(run / "complete.json")
        assert record["request_id"] == row["request_id"] and record["project_id"] == project["project_id"]
        assert record["project_revision"] == project["revision"] == 7 and record["block_size"] == host
        assert record["sample_rate_hz"] == rate and record["channels"] == channels and record["frames"] == frames
        assert record["valid_start_frame"] == 0 and record["valid_frame_count"] == frames
        assert record["output_sha256"] == complete["output_sha256"] == sha(run / "output.wav")
        output_rate, output = read_wav(run / "output.wav")
        assert output_rate == rate
        maximum = close(output, expected["C"])
        if row["origin"] == "cli":
            cli_text = (project_file.parent / "cli-stdout.txt").read_text(encoding="utf-8")
            assert f'run_id={record["run_id"]}' in cli_text and f'output_sha256={record["output_sha256"]}' in cli_text
        for resource in record["resources"]:
            assert resource["sha256"] == resource["snapshot_sha256"] == sha(input_file) == sha(run / resource["snapshot_path"])
        kinds = [task["kind"] for task in record["tasks"]]
        assert kinds == (["source", "native_region", "offline", "native_region", "sink"] if direction == 0 else ["source", "offline", "native_region", "offline", "sink"])
        for task in record["tasks"]:
            assert all(i < task["index"] for i in task["dependencies"])
            for port in task["outputs"]:
                assert port["frames"] == frames and port["sample_rate_hz"] == rate and port["channel_ids"] == [f"ch{i}" for i in range(channels)]
                assert port["valid_start_frame"] == 0 and port["valid_frame_count"] == frames and port["dtype"] == "float32"
        python_nodes = {"B"} if direction == 0 else {"A", "C"}
        assert {a["provenance"]["node_id"] for a in record["task_artifacts"]} == python_nodes
        for artifact in record["task_artifacts"]:
            p = artifact["provenance"]; node = p["node_id"]; directory = run / artifact["directory"]
            assert p["request_id"] == record["request_id"] and p["process_id"] > 0
            assert p["interpreter_sha256"] == sha(Path(p["environment"]["executable"]))
            code = directory / p["code_snapshot"]
            actual_files = {path.relative_to(io_path(code)).as_posix(): sha(path) for path in io_path(code).rglob("*") if path.suffix == ".py" or path.name == "manifest.json"}
            assert actual_files == p["code_files"]
            digest = hashlib.sha256(json.dumps(actual_files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            assert digest == p["code_sha256"] == code.name
            code_hashes.add(digest)
            snapshot(run / p["frozen_package_root"]); runner_hashes.add(snapshot(run / p["runner_snapshot"]))
            for name, digest in p["code_files"].items():
                assert sha(directory / p["code_snapshot"] / name) == digest
            data = io_path(directory / p["code_snapshot"] / "user_source.py").read_bytes()
            assert io_path(run / p["frozen_package_root"] / "user_source.py").read_bytes() == data
            lock = nodes[node]["managed_version"]
            installed = root / "library/packages" / lock["package_id"] / lock["content_sha256"] / "user_source.py"
            assert io_path(installed).read_bytes() == data
            assert b"dtype=np.float64" in data and b"def reset(state,reason)" in data
            source_hashes.add(sha(directory / p["code_snapshot"] / "user_source.py"))
            input_meta = p["inputs"]["audio_in"]
            # Input copies remain relative to their result manifest directory;
            # output/code paths are already rebased to the task run root.
            input_npy = directory / Path(p["result_manifest"]).parent / input_meta["file"]
            assert input_meta["sha256"] == sha(input_npy)
            before = expected["A"] if direction == 0 else source if node == "A" else expected["B"]
            maximum = max(maximum, close(npy(input_npy), before))
            meta = p["outputs"]["audio_out"]; output_npy = directory / meta["path"]
            assert meta["sha256"] == sha(output_npy) and meta["byte_size"] == len(io_path(output_npy).read_bytes())
            assert meta["shape"] == [channels, frames] and meta["sample_rate_hz"] == rate and meta["channel_ids"] == [f"ch{i}" for i in range(channels)]
            assert meta["valid_interval"] == [0, frames] and meta["dtype"] == "float32"
            maximum = max(maximum, close(npy(output_npy), expected[node]))
        matched = []
        for path in (project_file.parent / "run-logs").glob("*/events.jsonl"):
            events = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
            if events and events[0]["request_id"] == record["request_id"]:
                matched.append(events)
        assert len(matched) == 1
        events = matched[0]
        assert {e["node_id"] for e in events} == python_nodes
        for node in python_nodes:
            selected = [e for e in events if e["node_id"] == node]
            lock = nodes[node]["managed_version"]
            assert all(e["project_id"] == record["project_id"] and e["revision"] == record["project_revision"] and e["session_epoch"] == record["session_epoch"] and e["request_id"] == record["request_id"] and e["type_id"] == "user.pc05_transform" and e["content_sha256"] == lock["content_sha256"] and e["package_id"] == lock["package_id"] for e in selected)
            assert all(e["source"] == "stdout" and e["level"] == "info" for e in selected)
            paths = {Path(e["raw_log_file"]).resolve() for e in selected}; assert len(paths) == 1
            raw_path = next(iter(paths)); assert raw_path.is_relative_to(project_file.parent / "run-logs")
            raw = io_path(raw_path).read_bytes()
            assert raw == "".join(e["text"] for e in selected).encode() and selected[-1]["total_bytes"] == len(raw)
            assert all(e["discarded_bytes"] == e["dropped_text_bytes"] == 0 for e in selected)
            trace = re.fullmatch(rb"pc05-start gain=(\S+) step=(\S+)\r\npc05-end frames=(\d+) calls=(\d+) max_block=(\d+) reason=run_complete\r\n", raw); assert trace
            params = nodes[node]["parameters"]
            assert float(trace[1]) == params["gain"] and float(trace[2]) == params["step"]
            assert int(trace[3]) == frames and int(trace[4]) == math.ceil(frames / host) and int(trace[5]) == min(frames, host)
        records.append({"profile": list(key(row)), "origin": row["origin"], "repeat": row["repeat"], "request": record["request_id"], "run": row["run"], "max_error": maximum})
    assert len(source_hashes) == len(code_hashes) == len(runner_hashes) == 1
    report = {"status": "passed", "root": str(root), "matrix": args.matrix, "profiles": len(expected_profiles), "runs": records,
              "source_sha256": next(iter(source_hashes)), "code_sha256": next(iter(code_hashes)), "runner_sha256": next(iter(runner_hashes)), "host_hashes": hosts,
              "maximum_error": max(r["max_error"] for r in records), "tolerance": "2e-6 + 1e-5 * abs(expected)",
              "scope": "each GUI/CLI Run independently against stage-rounded formula, actual WAV/NPY/code/resources/identity/reset trace; no desktop/full platform claim"}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "passed", "profiles": len(expected_profiles), "runs": len(records), "max_error": report["maximum_error"]}))


if __name__ == "__main__":
    main()
