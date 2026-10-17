"""Independently check a PCM16 input -> float32 output Gain run."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import wave
from pathlib import Path


def read_pcm16(path: Path) -> tuple[int, int, list[float]]:
    with wave.open(str(path), "rb") as source:
        if source.getcomptype() != "NONE" or source.getsampwidth() != 2:
            raise ValueError("input must be uncompressed PCM16 WAV")
        channels = source.getnchannels()
        rate = source.getframerate()
        frames = source.getnframes()
        raw = source.readframes(frames)
    values = [sample / 32768.0 for (sample,) in struct.iter_unpack("<h", raw)]
    if len(values) != frames * channels:
        raise ValueError("input WAV data length does not match its header")
    return rate, channels, values


def read_float32_wav(path: Path) -> tuple[int, int, list[float]]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("output is not a RIFF/WAVE file")
    offset = 12
    fmt: tuple[int, int, int, int, int, int] | None = None
    audio_data: bytes | None = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > len(data):
            raise ValueError("output WAV contains a truncated chunk")
        chunk = data[chunk_start:chunk_end]
        if chunk_id == b"fmt ":
            if len(chunk) < 16:
                raise ValueError("output WAV fmt chunk is too short")
            fmt = struct.unpack_from("<HHIIHH", chunk)
        elif chunk_id == b"data":
            audio_data = chunk
        offset = chunk_end + (chunk_size & 1)
    if fmt is None or audio_data is None:
        raise ValueError("output WAV is missing fmt or data")
    encoding, channels, rate, _, block_align, bits = fmt
    if encoding != 3 or bits != 32 or block_align != channels * 4:
        raise ValueError("output must be IEEE float32 WAV")
    if len(audio_data) % 4:
        raise ValueError("output data size is not a whole number of float32 samples")
    values = [sample for (sample,) in struct.iter_unpack("<f", audio_data)]
    return rate, channels, values


def verify(input_path: Path, run_dir: Path, gain_db: float) -> dict:
    manifest_path = run_dir / "run.json"
    complete_path = run_dir / "complete.json"
    output_path = run_dir / "output.wav"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    complete = json.loads(complete_path.read_text(encoding="utf-8"))
    input_hash = hashlib.sha256(input_path.read_bytes()).hexdigest()
    output_hash = hashlib.sha256(output_path.read_bytes()).hexdigest()
    input_rate, input_channels, input_samples = read_pcm16(input_path)
    output_rate, output_channels, output_samples = read_float32_wav(output_path)

    expected_gain = 10.0 ** (gain_db / 20.0)
    failures: list[str] = []
    if manifest.get("input_sha256") != input_hash:
        failures.append("input SHA-256 differs from the run manifest")
    if manifest.get("output_sha256") != output_hash or complete.get("output_sha256") != output_hash:
        failures.append("output SHA-256 differs from the run or completion manifest")
    if complete.get("run_id") != manifest.get("run_id") or complete.get("manifest") != "run.json":
        failures.append("completion marker does not reference this run manifest")
    if (input_rate, input_channels, len(input_samples)) != (output_rate, output_channels, len(output_samples)):
        failures.append("input and output sample rate, channel count, or sample count differ")
    if manifest.get("frames") != len(input_samples) // input_channels:
        failures.append("run manifest frame count differs from the input WAV")

    max_abs_error = 0.0
    worst_index = -1
    if len(input_samples) == len(output_samples):
        for index, (source, actual) in enumerate(zip(input_samples, output_samples, strict=True)):
            expected = source * expected_gain
            error = abs(actual - expected)
            tolerance = 2.0e-6 + 1.0e-5 * abs(expected)
            if error > max_abs_error:
                max_abs_error = error
                worst_index = index
            if not math.isfinite(actual) or error > tolerance:
                failures.append(f"sample {index} differs by {error:g} (limit {tolerance:g})")
                break

    return {
        "schema_version": 1,
        "status": "passed" if not failures else "failed",
        "run_id": manifest.get("run_id"),
        "input_sha256": input_hash,
        "output_sha256": output_hash,
        "gain_db": gain_db,
        "sample_count": len(input_samples),
        "channels": input_channels,
        "max_abs_error": max_abs_error,
        "worst_sample_index": worst_index,
        "failures": failures,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--gain-db", type=float, required=True)
    args = parser.parse_args()
    report = verify(args.input, args.run_dir, args.gain_db)
    report_path = args.run_dir / "independent_verification.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"evidence: {report_path}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
