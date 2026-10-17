"""Create the deterministic, self-contained File -> Gain -> Output example."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "examples" / "gain_demo"
SAMPLE_RATE = 48_000
FRAMES = SAMPLE_RATE * 2
FREQUENCIES = (440.0, 660.0)
AMPLITUDE = 0.18


def create_example(output_dir: Path, force: bool) -> tuple[Path, Path, Path]:
    input_path = output_dir / "input.wav"
    project_path = output_dir / "gain_demo.aaproj"
    manifest_path = output_dir / "demo_manifest.json"
    targets = (input_path, project_path, manifest_path)
    existing = [path for path in targets if path.exists()]
    if existing and not force:
        raise FileExistsError("Refusing to replace existing demo files: " + ", ".join(map(str, existing)))

    output_dir.mkdir(parents=True, exist_ok=True)
    samples = bytearray()
    for frame in range(FRAMES):
        for channel, frequency in enumerate(FREQUENCIES):
            phase = (2.0 * math.pi * frequency * frame) / SAMPLE_RATE
            value = AMPLITUDE * math.sin(phase)
            integer = max(-32768, min(32767, round(value * 32767.0)))
            samples.extend(struct.pack("<h", integer))

    with wave.open(str(input_path), "wb") as output:
        output.setnchannels(len(FREQUENCIES))
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(samples)

    project = {
        "schema_version": 1,
        "project_id": "audioalgo-demo-gain-001",
        "name": "Stereo Gain Demo",
        "revision": 1,
        "selected_output_node_id": "output",
        "nodes": [
            {"id": "file", "type_id": "builtin.file_source", "x": 60.0, "y": 150.0,
             "parameters": {"path": "input.wav"}},
            {"id": "gain", "type_id": "builtin.gain", "x": 340.0, "y": 150.0,
             "parameters": {"gain_db": 6.0}},
            {"id": "output", "type_id": "builtin.output", "x": 620.0, "y": 150.0,
             "parameters": {}},
        ],
        "connections": [
            {"from_node": "file", "from_port": "audio", "to_node": "gain", "to_port": "audio_in"},
            {"from_node": "gain", "from_port": "audio_out", "to_node": "output", "to_port": "audio"},
        ],
    }
    project_path.write_text(json.dumps(project, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    manifest = {
        "schema_version": 1,
        "generator": "scripts/create_demo_project.py",
        "sample_rate_hz": SAMPLE_RATE,
        "channels": len(FREQUENCIES),
        "frames": FRAMES,
        "encoding": "PCM16",
        "tone_frequencies_hz": list(FREQUENCIES),
        "amplitude": AMPLITUDE,
        "source_sha256": hashlib.sha256(input_path.read_bytes()).hexdigest(),
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return input_path, project_path, manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--force", action="store_true", help="replace the three generated demo files")
    args = parser.parse_args()
    try:
        input_path, project_path, manifest_path = create_example(args.output_dir, args.force)
    except FileExistsError as exc:
        parser.error(str(exc))
    print(input_path)
    print(project_path)
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
