"""Run both exact versions from a copied library while its original is unavailable."""
import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import uuid


def io(path):
    value = str(Path(path).absolute())
    if os.name == "nt" and not value.startswith("\\\\?\\"):
        value = "\\\\?\\UNC\\" + value[2:] if value.startswith("\\\\") else "\\\\?\\" + value
    return Path(value)


def sha(path):
    return hashlib.sha256(io(path).read_bytes()).hexdigest()


parser = argparse.ArgumentParser()
parser.add_argument("command_record", type=Path)
parser.add_argument("root", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
record = json.loads(args.command_record.read_text(encoding="utf-8"))
assert record["exit_code"] == 0
start = dt.datetime.fromisoformat(record["started_utc"]).timestamp()
end = dt.datetime.fromisoformat(record["finished_utc"]).timestamp()
candidates = [p for p in io(args.root).glob("*/chosen-library/index.json") if start <= p.stat().st_mtime <= end]
assert len(candidates) == 1, candidates
copied = candidates[0].parent
test_root = copied.parent
original = test_root / "library"
unavailable = test_root / ("original-unavailable-" + uuid.uuid4().hex)
assert original.parent == test_root and not unavailable.exists()
index_before = sha(original / "index.json")
versions = sorted(copied.glob("packages/*/*"))
assert len(versions) == 2
work = test_root / ("independent-runs-" + uuid.uuid4().hex)
work.mkdir()
samples = [.2 * math.sin(.03 * (n + 1)) for n in range(513)]
pcm = struct.pack("<" + "f" * len(samples), *samples)
fmt = struct.pack("<HHIIHH", 3, 1, 16000, 64000, 4, 32)
wav = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm
(work / "input.wav").write_bytes(b"RIFF" + struct.pack("<I", len(wav)) + wav)
env = dict(os.environ, AUDIOALGO_AUTHORING_ROOT=str(copied), AUDIOALGO_PLUGIN_PATH=str(work / "empty-plugins"))
source_cli = Path("build/windows-debug/bin/audio_cli.exe").absolute()
# Launch the identical bytes as a normal deployment file. Executables in this
# managed workspace inherit Low Integrity even from a normal parent process.
# Keep the selected library/output paths unchanged, including their long names.
runtime = tempfile.TemporaryDirectory(prefix="AudioAlgo-CLI-proof-")
cli = Path(runtime.name) / source_cli.name
cli.write_bytes(source_cli.read_bytes())
assert sha(cli) == sha(source_cli)
reports = []
original.rename(unavailable)
try:
    for version in versions:
        metadata = json.loads((version / "snapshot.json").read_text(encoding="utf-8"))
        for name, digest in metadata["files"].items():
            assert sha(version / name) == digest
        signature = "".join(name + "\0" + digest + "\n" for name, digest in sorted(metadata["files"].items()))
        assert hashlib.sha256(signature.encode()).hexdigest() == version.name == metadata["content_sha256"]
        project = json.loads(Path("examples/gain_demo/gain_demo.aaproj").read_text(encoding="utf-8"))
        project["schema_version"] = 2
        project["project_id"] = "independent-library-" + version.name[:12]
        project["nodes"][1]["type_id"] = "user.custom_node"
        project["nodes"][1]["parameters"] = {"gain": .5}
        project["nodes"][1]["managed_version"] = {"package_id": version.parent.name, "content_sha256": version.name,
            "package_path": "node_packages/" + version.parent.name + "/" + version.name, "wrapper_version": "python-function-v1"}
        path, run = work / (version.name + ".aaproj"), work / (version.name + "-run")
        path.write_text(json.dumps(project), encoding="utf-8")
        result = subprocess.run([str(cli), "run", str(path), str(run), "127"], env=env, capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        manifests = list(run.glob("python-task-*/python-result-*/result.json"))
        assert len(manifests) == 1
        executed = json.loads(manifests[0].read_text(encoding="utf-8"))
        assert executed["code_files"] == metadata["files"]
        for name, digest in executed["code_files"].items():
            assert sha(manifests[0].parent.parent / executed["code_snapshot"] / name) == digest
        raw = (run / "output.wav").read_bytes()
        assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
        chunks, offset = {}, 12
        while offset + 8 <= len(raw):
            kind, size = struct.unpack_from("<4sI", raw, offset)
            chunks[kind] = raw[offset + 8:offset + 8 + size]
            offset += 8 + size + size % 2
        tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", chunks[b"fmt "])
        assert (tag, channels, rate, bits) == (3, 1, 16000, 32)
        output = struct.unpack("<" + "f" * (len(chunks[b"data"]) // 4), chunks[b"data"])
        assert len(output) == 513
        maximum = 0
        for n, value in enumerate(output):
            expected = .1 * math.sin(.03 * (n + 1))
            assert math.isfinite(value) and abs(value - expected) <= 2e-6 + 1e-5 * abs(expected)
            maximum = max(maximum, abs(value - expected))
        resource, = json.loads((run / "run.json").read_text(encoding="utf-8"))["resources"]
        assert sha(run / resource["snapshot_path"]) == sha(work / "input.wav") == resource["sha256"]
        reports.append({"version": version.name, "project": str(path), "result": str(manifests[0]), "wav_sha256": sha(run / "output.wav"), "maximum_absolute_error": maximum})
finally:
    unavailable.rename(original)
assert sha(original / "index.json") == index_before
with args.output.open("x", encoding="utf-8") as stream:
    json.dump({"result": "passed", "cli_sha256": sha(cli), "source_cli": str(source_cli), "normal_runtime": str(cli), "original_index_sha256": index_before,
               "original_restored": True, "runs": reports}, stream, indent=2)
print("Copied library independently ran both exact versions, original unavailable, sources/input hashes and two 513-frame WAVs verified")
