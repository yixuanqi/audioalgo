"""Independently verify T25 dual-source run artifacts against the 2A+3B+f/4096 oracle.

Reads only the exported a.wav/b.wav originals and the run/replay output WAV files with a
pure Python RIFF parser; never touches the C++ implementation under test.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path


def read_float32_wav(path: Path) -> tuple[int, int, list[float]]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")
    offset = 12
    fmt = None
    audio_data = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            raise ValueError(f"{path} contains a truncated chunk")
        chunk = data[chunk_start:chunk_end]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", chunk)
        elif chunk_id == b"data":
            audio_data = chunk
        offset = chunk_end + (chunk_size & 1)
    if fmt is None or audio_data is None:
        raise ValueError(f"{path} is missing fmt or data")
    encoding, channels, rate, _, block_align, bits = fmt
    if encoding != 3 or bits != 32 or block_align != channels * 4:
        raise ValueError(f"{path} must be IEEE float32 WAV")
    values = [sample for (sample,) in struct.iter_unpack("<f", audio_data)]
    return rate, channels, values


def channels_of(values: list[float], channels: int) -> list[list[float]]:
    return [values[index::channels] for index in range(channels)]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_case(case_dir: Path, swapped: bool) -> dict:
    failures: list[str] = []
    rate_a, channels_a, a_values = read_float32_wav(case_dir / "a.wav")
    rate_b, channels_b, b_values = read_float32_wav(case_dir / "b.wav")
    if (rate_a, channels_a) != (rate_b, channels_b) or (rate_a, channels_a) != (48000, 2):
        failures.append(f"source shape mismatch: a=({rate_a},{channels_a}) b=({rate_b},{channels_b})")
    a = channels_of(a_values, channels_a)
    b = channels_of(b_values, channels_b)
    frames = len(a[0])
    primary, reference = (b, a) if swapped else (a, b)
    expected = [
        [2.0 * primary[channel][frame] + 3.0 * reference[channel][frame] + frame / 4096.0
         for frame in range(frames)]
        for channel in range(2)
    ]
    max_error = 0.0
    worst = -1
    for label in ("run", "replay"):
        output_path = case_dir / label / "output.wav"
        rate_o, channels_o, output = read_float32_wav(output_path)
        if (rate_o, channels_o) != (48000, 2):
            failures.append(f"{label} output shape is ({rate_o},{channels_o}), expected (48000,2)")
            continue
        produced = channels_of(output, 2)
        for channel in range(2):
            for frame in range(frames):
                error = abs(produced[channel][frame] - expected[channel][frame])
                if error > max_error:
                    max_error, worst = error, frame
                if not math.isfinite(produced[channel][frame]):
                    failures.append(f"{label} output sample {frame}/{channel} is not finite")
    run_dir = case_dir / "run"
    replay_dir = case_dir / "replay"
    for required in (run_dir / "run.json", run_dir / "complete.json", run_dir / "replay.aaproj",
                     run_dir / "project.aaproj", run_dir / "output.wav",
                     run_dir / "resources" / "0" / "a.wav", run_dir / "resources" / "1" / "b.wav",
                     replay_dir / "output.wav", replay_dir / "run.json", replay_dir / "complete.json"):
        if not required.is_file():
            failures.append(f"missing artifact: {required.relative_to(case_dir)}")
    if not failures:
        # The captured resource copies must be byte-identical to the exported originals.
        if sha(run_dir / "resources" / "0" / "a.wav") != sha(case_dir / "a.wav"):
            failures.append("captured resource 0 is not a byte-identical copy of a.wav")
        if sha(run_dir / "resources" / "1" / "b.wav") != sha(case_dir / "b.wav"):
            failures.append("captured resource 1 is not a byte-identical copy of b.wav")
        manifest = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
        resources = manifest.get("resources", [])
        if len(resources) != 2:
            failures.append(f"run.json lists {len(resources)} resources, expected 2")
    return {"case": case_dir.name, "status": "passed" if not failures else "failed",
            "frames": frames, "max_abs_error": max_error, "worst_sample_index": worst,
            "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, required=True,
                        help="exported artifact root produced by the T25 test")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"schema_version": 1, "artifacts": str(args.artifacts.resolve()), "cases": []}
    failures: list[str] = []
    for case_dir in sorted(path for path in args.artifacts.iterdir() if path.is_dir()):
        swapped = case_dir.name.endswith("swapped")
        result = verify_case(case_dir, swapped)
        report["cases"].append(result)
        failures.extend(f"{result['case']}: {failure}" for failure in result["failures"])
    report["status"] = "passed" if not failures else "failed"
    report["failures"] = failures
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(args.output)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
