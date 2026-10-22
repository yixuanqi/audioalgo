"""Independent oracle for AR-14.c selected-port pruning through the CLI composition root.

Builds a two-branch project by hand (JSON schema 1 plus selected_output_port_id), runs it with
audio_cli.exe, and verifies with a pure Python RIFF parser that:
  - the run succeeds although the unselected branch's source file (pruned.wav) never exists;
  - run.json materializes exactly one resource (the closure's source "a");
  - output.wav equals the mixer's default-gain sum a + a = 2a sample by sample.

The oracle never touches the C++ implementation under test.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

SAMPLE_RATE = 48000
FRAMES = 300


def write_float32_wav(path: Path, channels: list[list[float]]) -> None:
    values = [sample for channel in channels for frame, sample in enumerate(channel) for channel in [channel]][0:0]
    # Interleave channel-major stereo frames.
    values = []
    for frame in range(FRAMES):
        for channel in channels:
            values.append(channel[frame])
    audio = struct.pack("<%df" % len(values), *values)
    fmt = struct.pack("<HHIIHH", 3, len(channels), SAMPLE_RATE, SAMPLE_RATE * 4 * len(channels),
                      4 * len(channels), 32)
    chunks = b""
    for identifier, payload in ((b"fmt ", fmt), (b"data", audio)):
        chunks += identifier + struct.pack("<I", len(payload)) + payload
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def read_float32_wav(path: Path) -> list[float]:
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")
    offset, audio, fmt = 12, None, None
    while offset + 8 <= len(data):
        chunk_id = data[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        payload = data[offset + 8:offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", payload)
        elif chunk_id == b"data":
            audio = payload
        offset += 8 + chunk_size + (chunk_size & 1)
    if fmt is None or audio is None or fmt[0] != 3 or fmt[1] != 2 or fmt[5] != 32:
        raise ValueError(f"{path} must be stereo IEEE float32 WAV")
    return [sample for (sample,) in struct.iter_unpack("<f", audio)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", type=Path, required=True, help="audioalgo repository root")
    parser.add_argument("--cli", type=Path, required=True, help="audio_cli.exe path")
    parser.add_argument("--output", type=Path, required=True, help="report JSON path")
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    root = workspace / "build" / "reports" / "loops" / "RF0006" / "A3-selected-port-pruning" / "oracle-work"
    if root.exists():
        raise SystemExit(f"{root} already exists; remove it for a fresh oracle run")
    root.mkdir(parents=True)

    left = [0.25 * math.sin(0.05 * frame) for frame in range(FRAMES)]
    right = [-0.2 * math.cos(0.04 * frame) for frame in range(FRAMES)]
    write_float32_wav(root / "a.wav", [left, right])

    project = {
        "schema_version": 1,
        "project_id": "ar14c-cli-pruning",
        "name": "Selected port pruning oracle",
        "revision": 1,
        "selected_output_node_id": "mix",
        "selected_output_port_id": "sum",
        "nodes": [
            {"id": "a", "type_id": "builtin.file_source", "x": 10.0, "y": 20.0,
             "parameters": {"path": "a.wav"}},
            {"id": "pruned", "type_id": "builtin.file_source", "x": 10.0, "y": 160.0,
             "parameters": {"path": "pruned.wav"}},
            {"id": "mix", "type_id": "builtin.mixer", "x": 240.0, "y": 80.0, "parameters": {}},
            {"id": "gain", "type_id": "builtin.gain", "x": 240.0, "y": 200.0, "parameters": {}},
            {"id": "out2", "type_id": "builtin.output", "x": 480.0, "y": 200.0, "parameters": {}},
        ],
        "connections": [
            {"from_node": "a", "from_port": "audio", "to_node": "mix", "to_port": "primary"},
            {"from_node": "a", "from_port": "audio", "to_node": "mix", "to_port": "secondary"},
            {"from_node": "pruned", "from_port": "audio", "to_node": "gain", "to_port": "audio_in"},
            {"from_node": "gain", "from_port": "audio_out", "to_node": "out2", "to_port": "audio"},
        ],
    }
    project_path = root / "pruning.aaproj"
    project_path.write_text(json.dumps(project, indent=2, ensure_ascii=False), encoding="utf-8")

    run_dir = root / "run"
    completed = subprocess.run(
        [str(args.cli.resolve()), "run", str(project_path), str(run_dir), "64"],
        capture_output=True, text=True, timeout=120)
    failures: list[str] = []
    if completed.returncode != 0:
        failures.append(f"cli exit {completed.returncode}: {completed.stdout} {completed.stderr}")
    if not (root / "pruned.wav").exists() and not (run_dir / "resources" / "1").exists():
        pass  # expected: the pruned source was never materialized
    else:
        failures.append("pruned source was materialized unexpectedly")

    report = {"schema_version": 1, "cli_exit": completed.returncode, "stdout": completed.stdout.strip()}
    if completed.returncode == 0:
        manifest = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
        resources = manifest.get("resources", [])
        if len(resources) != 1:
            failures.append(f"run.json lists {len(resources)} resources, expected 1")
        elif resources[0].get("node_id") != "a":
            failures.append(f"run.json resource node_id is {resources[0].get('node_id')!r}, expected 'a'")
        output = read_float32_wav(run_dir / "output.wav")
        if len(output) != 2 * FRAMES:
            failures.append(f"output.wav has {len(output)} samples, expected {2 * FRAMES}")
        else:
            worst = 0.0
            for frame in range(FRAMES):
                for channel, expected in ((0, 2.0 * left[frame]), (1, 2.0 * right[frame])):
                    worst = max(worst, abs(output[2 * frame + channel] - expected))
                    if not math.isfinite(output[2 * frame + channel]):
                        failures.append(f"output sample {frame}/{channel} is not finite")
            report["max_abs_error"] = worst
            if worst > 1.0e-6:
                failures.append(f"output deviates from 2a by {worst}")
    report["failures"] = failures
    report["status"] = "passed" if not failures else "failed"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(args.output)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
