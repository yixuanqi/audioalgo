"""Generate the dual-source Mixer demo project for the 1024 showcase.

Creates (deterministically, no dependencies beyond the stdlib):
  mixer_demo/tone_primary_440.wav    48 kHz stereo PCM16, 1 s, 440 Hz sine @ ~0.5 FS
  mixer_demo/tone_secondary_1320.wav 48 kHz stereo PCM16, 1 s, 1320 Hz sine @ ~0.25 FS
  mixer_demo/mixer_demo.aaproj       two file sources -> builtin.mixer -> output

Both sources intentionally share format/sample-rate/frame count because the
run service validates paired sources (format/frames/channels must match).
Relative resource paths resolve against the project file's directory, so the
project opens and runs from wherever this folder is placed.
"""

from __future__ import annotations

import json
import math
import struct
import wave
from pathlib import Path

SAMPLE_RATE_HZ = 48000
SECONDS = 1.0
FRAMES = int(SAMPLE_RATE_HZ * SECONDS)
CHANNELS = 2
SAMPWIDTH = 2  # PCM16


def write_tone(path: Path, frequency_hz: float, amplitude: float) -> None:
    samples = bytearray()
    for frame in range(FRAMES):
        value = amplitude * math.sin(2.0 * math.pi * frequency_hz * frame / SAMPLE_RATE_HZ)
        raw = int(round(value * 32767.0))
        raw = max(-32768, min(32767, raw))
        for _ in range(CHANNELS):
            samples += struct.pack("<h", raw)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPWIDTH)
        wav.setframerate(SAMPLE_RATE_HZ)
        wav.writeframes(bytes(samples))


def write_project(path: Path) -> None:
    project = {
        "schema_version": 1,
        "project_id": "audioalgo-showcase-mixer-1024",
        "name": "Dual Source Mixer Demo",
        "revision": 1,
        "selected_output_node_id": "output",
        "nodes": [
            {
                "id": "music",
                "type_id": "builtin.file_source",
                "x": 60.0,
                "y": 80.0,
                "parameters": {"path": "tone_primary_440.wav"},
            },
            {
                "id": "voice",
                "type_id": "builtin.file_source",
                "x": 60.0,
                "y": 280.0,
                "parameters": {"path": "tone_secondary_1320.wav"},
            },
            {
                "id": "mixer",
                "type_id": "builtin.mixer",
                "x": 400.0,
                "y": 180.0,
                "parameters": {"primary_gain_db": 0.0, "secondary_gain_db": -6.0},
            },
            {
                "id": "output",
                "type_id": "builtin.output",
                "x": 700.0,
                "y": 180.0,
                "parameters": {},
            },
        ],
        "connections": [
            {"from_node": "music", "from_port": "audio", "to_node": "mixer", "to_port": "primary"},
            {"from_node": "voice", "from_port": "audio", "to_node": "mixer", "to_port": "secondary"},
            {"from_node": "mixer", "from_port": "sum", "to_node": "output", "to_port": "audio"},
        ],
    }
    path.write_text(json.dumps(project, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    demo_dir = Path(__file__).resolve().parent / "mixer_demo"
    demo_dir.mkdir(parents=True, exist_ok=True)
    write_tone(demo_dir / "tone_primary_440.wav", 440.0, 0.5)
    write_tone(demo_dir / "tone_secondary_1320.wav", 1320.0, 0.25)
    write_project(demo_dir / "mixer_demo.aaproj")
    print(f"demo project ready: {demo_dir / 'mixer_demo.aaproj'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
