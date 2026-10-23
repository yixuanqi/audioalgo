"""Independent per-sample oracle for the 1024 showcase Mixer demo run.

Verifies a real run directory produced by audio_cli (or the editor, which uses
the same RunService) against the documented mixer formula:

    out[ch] = 10^(primary_db/20) * A[ch] + 10^(secondary_db/20) * B[ch]

Reads the gain values and the two source paths from the demo project JSON, so
recording only needs one argument: the run directory.

Checks:
  * output WAV is IEEE float32, same rate/channels/frame count as the sources
  * complete.json output_sha256 matches the actual output.wav bytes
  * every output sample matches the oracle within 2e-6 + 1e-5*|expected|

Usage:
  python verify_mixer_demo.py --run-dir <dir containing run.json/output.wav>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import wave
from pathlib import Path

TOLERANCE_BASE = 2.0e-6
TOLERANCE_SCALE = 1.0e-5


def read_pcm16(path: Path) -> tuple[int, int, list[float]]:
    with wave.open(str(path), "rb") as source:
        if source.getcomptype() != "NONE" or source.getsampwidth() != 2:
            raise ValueError(f"{path.name} must be uncompressed PCM16 WAV")
        channels = source.getnchannels()
        rate = source.getframerate()
        raw = source.readframes(source.getnframes())
    values = [sample / 32768.0 for (sample,) in struct.iter_unpack("<h", raw)]
    return rate, channels, values


def read_float32_wav(path: Path) -> tuple[int, int, list[float]]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("output is not a RIFF/WAVE file")
    offset = 12
    fmt = None
    audio_data = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", chunk)
        elif chunk_id == b"data":
            audio_data = chunk
        offset += 8 + chunk_size + (chunk_size & 1)
    if fmt is None or audio_data is None:
        raise ValueError("output WAV is missing fmt or data")
    encoding, channels, rate, _, block_align, bits = fmt
    if encoding != 3 or bits != 32 or block_align != channels * 4:
        raise ValueError("output must be IEEE float32 WAV")
    values = [sample for (sample,) in struct.iter_unpack("<f", audio_data)]
    return rate, channels, values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=None,
                        help="demo .aaproj (default: mixer_demo/mixer_demo.aaproj next to this script)")
    args = parser.parse_args()

    project_path = args.project or (Path(__file__).resolve().parent / "mixer_demo" / "mixer_demo.aaproj")
    project = json.loads(project_path.read_text(encoding="utf-8"))
    project_dir = project_path.resolve().parent

    nodes = {node["id"]: node for node in project["nodes"]}
    connections = {(c["to_node"], c["to_port"]): (c["from_node"], c["from_port"]) for c in project["connections"]}
    source_a_id = connections[("mixer", "primary")][0]
    source_b_id = connections[("mixer", "secondary")][0]
    primary_db = float(nodes["mixer"]["parameters"]["primary_gain_db"])
    secondary_db = float(nodes["mixer"]["parameters"]["secondary_gain_db"])
    path_a = project_dir / nodes[source_a_id]["parameters"]["path"]
    path_b = project_dir / nodes[source_b_id]["parameters"]["path"]

    run_dir = args.run_dir
    output_path = run_dir / "output.wav"
    complete = json.loads((run_dir / "complete.json").read_text(encoding="utf-8"))

    failures: list[str] = []
    output_hash = hashlib.sha256(output_path.read_bytes()).hexdigest()
    if complete.get("output_sha256") != output_hash:
        failures.append("output SHA-256 differs from complete.json")

    rate_a, channels, samples_a = read_pcm16(path_a)
    _, _, samples_b = read_pcm16(path_b)
    out_rate, out_channels, out_samples = read_float32_wav(output_path)

    if (rate_a, channels) != (out_rate, out_channels):
        failures.append(f"output format {out_rate} Hz x {out_channels} ch differs from sources {rate_a} Hz x {channels} ch")
    if len(samples_a) != len(samples_b):
        failures.append("paired sources must have identical frame counts")
    if len(out_samples) != len(samples_a):
        failures.append("output sample count differs from sources")

    primary_weight = 10.0 ** (primary_db / 20.0)
    secondary_weight = 10.0 ** (secondary_db / 20.0)

    max_abs_error = 0.0
    worst_index = -1
    if not failures:
        for index, (a, b, actual) in enumerate(zip(samples_a, samples_b, out_samples, strict=True)):
            expected = primary_weight * a + secondary_weight * b
            error = abs(actual - expected)
            tolerance = TOLERANCE_BASE + TOLERANCE_SCALE * abs(expected)
            if error > max_abs_error:
                max_abs_error = error
                worst_index = index
            if not math.isfinite(actual) or error > tolerance:
                failures.append(f"sample {index} differs by {error:g} (limit {tolerance:g})")
                break

    report = {
        "schema_version": 1,
        "status": "passed" if not failures else "failed",
        "oracle": "out = 10^(primary_db/20)*A + 10^(secondary_db/20)*B",
        "primary_gain_db": primary_db,
        "secondary_gain_db": secondary_db,
        "output_sha256": output_hash,
        "sample_count": len(out_samples),
        "channels": out_channels,
        "max_abs_error": max_abs_error,
        "worst_sample_index": worst_index,
        "failures": failures,
    }
    report_path = run_dir / "independent_verification.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"evidence: {report_path}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
