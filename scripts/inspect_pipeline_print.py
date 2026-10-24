"""Inspect actual PC-02 graph, executed code, Run identities, logs and PCM."""
import hashlib
import json
import os
import struct
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wav(path):
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
    position, fmt, samples = 12, None, None
    while position + 8 <= len(raw):
        tag, size = struct.unpack_from("<4sI", raw, position)
        block = raw[position + 8:position + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", block)
        elif tag == b"data":
            samples = np.frombuffer(block, dtype="<f4")
        position += 8 + size + size % 2
    assert fmt[0] == 3 and fmt[1] == 1 and fmt[2] == 16000 and fmt[5] == 32
    assert samples is not None and samples.size == 513
    return samples


def main():
    base = ROOT / "build/reports/loops/PC0201-print/artifacts"
    if os.name == "nt":
        base = Path("\\\\?\\" + str(base))
    chosen = max(base.glob("*/ui-log-1.txt"), key=lambda p: p.stat().st_mtime_ns).parent
    project = json.loads((chosen / "project.aaproj").read_text())
    custom, = [n for n in project["nodes"] if n["type_id"] == "user.pc02_print"]
    lock = custom["managed_version"]
    installed = chosen / "authoring/packages" / lock["package_id"] / lock["content_sha256"]
    source = (installed / "user_source.py").read_bytes()
    assert b"print('aaaaaaa', flush=True)" in source
    assert len(project["nodes"]) == 3 and len(project["connections"]) == 2
    original = wav(chosen / "input.wav")
    oracle = np.asarray([np.float32(.2 * np.sin(.03 * (n + 1))) for n in range(513)])
    assert np.max(np.abs(original - oracle)) <= 2e-8
    manifests = list(chosen.glob("runs/*/run.json"))
    assert len(manifests) == 2
    requests, rows = set(), []
    logs = [(chosen / f"ui-log-{n}.txt").read_text(encoding="utf-8") for n in range(2)]
    for manifest in manifests:
        run = manifest.parent
        data = json.loads(manifest.read_text())
        request = data["request_id"]
        assert data["run_id"] == request and data["project_id"] == project["project_id"]
        assert request not in requests
        requests.add(request)
        displayed, = [v for v in logs if request in v]
        assert displayed.count("aaaaaaa") == 2 and custom["id"] in displayed
        provenance, = [a["provenance"] for a in data["task_artifacts"]]
        assert provenance["node_id"] == custom["id"] and provenance["request_id"] == request
        assert provenance["logs"]["stdout_tail"].splitlines() == ["aaaaaaa", "aaaaaaa"]
        task = run / data["task_artifacts"][0]["directory"]
        executed = task / provenance["code_snapshot"] / "user_source.py"
        assert digest(executed) == digest(installed / "user_source.py") == provenance["code_files"]["user_source.py"]
        output = wav(run / "output.wav")
        assert np.array_equal(original, output)
        port = provenance["outputs"]["audio_out"]
        npy = task / port["path"]
        assert digest(npy) == port["sha256"]
        assert port["shape"] == [1, 513] and port["valid_interval"] == [0, 513] and port["channel_ids"] == ["ch0"]
        assert np.array_equal(np.load(npy, allow_pickle=False)[0], original)
        assert data["output_sha256"] == digest(run / "output.wav")
        rows.append({"run_id": request, "manifest": str(manifest), "output_sha256": data["output_sha256"], "max_identity_error": 0})
    print(json.dumps({"scope": "development_subset", "source_sha256": digest(installed / "user_source.py"), "package": lock, "runs": rows,
                      "desktop": "not_verified", "OS_drag_routing": "not_verified"}, indent=2))


if __name__ == "__main__":
    main()
