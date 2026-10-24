"""Independently inspect PC-01's real draft and recovery bytes (no product imports)."""
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fields(data, header):
    assert data.startswith(header)
    cursor, values = len(header), []
    while cursor < len(data):
        end = data.index(b"\n", cursor)
        length = int(data[cursor:end])
        cursor = end + 1
        value = data[cursor:cursor + length]
        assert len(value) == length
        values.append(value)
        cursor += length
    assert cursor == len(data)
    return values


def main():
    base = ROOT / "build/reports/loops/NA0002-save-recovery/storage"
    if os.name == "nt":
        base = Path("\\\\?\\" + str(base))
    candidates = list(base.rglob("用户 Print.aadraft"))
    assert candidates, "PC-01 must have written an actual draft"
    target = max(candidates, key=lambda p: p.stat().st_mtime_ns)
    saved = target.read_bytes()
    value = fields(saved, b"AudioAlgoDraft/1\n")
    assert value[:3] == [b"root-fix", b"7", b"python"]
    assert value[4].decode() == "# 中文\ndef incomplete("
    recovered = fields((target.parent / "recovered.aadraft").read_bytes(), b"AudioAlgoDraft/1\n")
    assert recovered[:3] == [b"root-fix", b"8", b"python"]
    assert recovered[3] == value[3] and recovered[4] == value[4] + b"# unsaved"
    recovery = target.parent / "recovery/root-fix.aadraft"
    assert fields(recovery.read_bytes(), b"AudioAlgoDraft/1\n") == recovered
    binding = fields(recovery.with_suffix(".aadraft.binding").read_bytes(), b"AudioAlgoRecoveryBinding/1\n")
    assert binding[:2] == [b"root-fix", b"8"] and binding[3] == saved
    assert Path(binding[2].decode()).resolve() == target.resolve()
    assert not list(target.parent.rglob("*.tmp*"))
    result = {"target": str(target), "path_characters": len(str(target)),
              "sha256": hashlib.sha256(saved).hexdigest(), "generation": 7,
              "recovery_generation": 8, "binding_preserves_original_bytes": True,
              "volume_root_write": "not_verified: outside workspace write scope"}
    assert result["path_characters"] > 260
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
