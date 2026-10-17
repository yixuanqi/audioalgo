# Stereo Gain demo

This folder contains a deterministic two-second, 48 kHz stereo PCM16 source and a schema-v1 project that applies +6 dB.

Regenerate the source WAV and its SHA-256 manifest with Python 3.11 or later:

```powershell
E:\0qyx\res\miniconda\python.exe scripts\create_demo_project.py
```

Run the project with a fresh output directory:

```powershell
New-Item -ItemType Directory -Force build\runs | Out-Null
build\windows-headless\bin\audio_cli.exe run examples\gain_demo\gain_demo.aaproj build\runs\gain-demo 127
```

The run writes `output.wav`, `run.json`, and publishes `complete.json` last. The output is float32 WAV; the project-relative source path remains `input.wav`.
