"""Independently verify the A2 production mixer run artifacts against the dB-weighted sum oracle.

Expected formula: sum = 10^(primary_gain_db/20) * primary + 10^(secondary_gain_db/20) * secondary,
with primary_gain_db = -6 and secondary_gain_db = +6 and primary = a.wav, secondary = b.wav.

Reads only the exported a.wav/b.wav originals and the service-run/cli-run/replay-run output WAV
files with a pure Python RIFF parser; never touches the C++ implementation under test.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

PRIMARY_GAIN_DB = -6.0
SECONDARY_GAIN_DB = 6.0


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


def verify(artifacts: Path) -> dict:
    failures: list[str] = []
    rate_a, channels_a, a_values = read_float32_wav(artifacts / "a.wav")
    rate_b, channels_b, b_values = read_float32_wav(artifacts / "b.wav")
    if (rate_a, channels_a) != (rate_b, channels_b) or (rate_a, channels_a) != (48000, 2):
        failures.append(f"source shape mismatch: a=({rate_a},{channels_a}) b=({rate_b},{channels_b})")
        return {"status": "failed", "failures": failures}
    a = channels_of(a_values, 2)
    b = channels_of(b_values, 2)
    frames = len(a[0])
    primary_weight = 10.0 ** (PRIMARY_GAIN_DB / 20.0)
    secondary_weight = 10.0 ** (SECONDARY_GAIN_DB / 20.0)
    expected = [
        [primary_weight * a[channel][frame] + secondary_weight * b[channel][frame]
         for frame in range(frames)]
        for channel in range(2)
    ]

    max_error = 0.0
    worst = -1
    for label in ("service-run", "cli-run", "replay-run"):
        output_path = artifacts / label / "output.wav"
        if not output_path.is_file():
            failures.append(f"missing output: {output_path.relative_to(artifacts)}")
            continue
        rate_o, channels_o, output = read_float32_wav(output_path)
        if (rate_o, channels_o) != (48000, 2):
            failures.append(f"{label} output shape is ({rate_o},{channels_o}), expected (48000,2)")
            continue
        produced = channels_of(output, 2)
        if len(produced[0]) != frames:
            failures.append(f"{label} frame count {len(produced[0])} != {frames}")
            continue
        for channel in range(2):
            for frame in range(frames):
                error = abs(produced[channel][frame] - expected[channel][frame])
                if error > max_error:
                    max_error, worst = error, frame
                if not math.isfinite(produced[channel][frame]):
                    failures.append(f"{label} output sample {frame}/{channel} is not finite")

    cli_run = artifacts / "cli-run"
    for required in (cli_run / "run.json", cli_run / "complete.json", cli_run / "replay.aaproj",
                     cli_run / "resources" / "0" / "a.wav", cli_run / "resources" / "1" / "b.wav"):
        if not required.is_file():
            failures.append(f"missing artifact: {required.relative_to(artifacts)}")
    if not failures:
        # Captured resource copies must be byte-identical to the exported originals.
        if sha(cli_run / "resources" / "0" / "a.wav") != sha(artifacts / "a.wav"):
            failures.append("captured resource 0 is not a byte-identical copy of a.wav")
        if sha(cli_run / "resources" / "1" / "b.wav") != sha(artifacts / "b.wav"):
            failures.append("captured resource 1 is not a byte-identical copy of b.wav")
        manifest = json.loads((cli_run / "run.json").read_text(encoding="utf-8"))
        resources = manifest.get("resources", [])
        if len(resources) != 2:
            failures.append(f"cli run.json lists {len(resources)} resources, expected 2")
    return {"status": "passed" if not failures else "failed", "frames": frames,
            "max_abs_error": max_error, "worst_sample_index": worst, "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, required=True,
                        help="exported artifact root produced by the A2 mixer test")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"schema_version": 1, "artifacts": str(args.artifacts.resolve()),
              "primary_gain_db": PRIMARY_GAIN_DB, "secondary_gain_db": SECONDARY_GAIN_DB}
    report.update(verify(args.artifacts))
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(args.output)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
