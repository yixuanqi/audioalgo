"""Independent standard-library WAV, source-byte and version-lock check (no plugin imports)."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def wav(path):
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    offset = 12
    fmt = payload = None
    while offset + 8 <= len(data):
        tag, count = struct.unpack_from('<4sI', data, offset)
        content = data[offset + 8:offset + 8 + count]
        if tag == b'fmt ': fmt = content
        if tag == b'data': payload = content
        offset += 8 + count + count % 2
    encoding, channels, rate = struct.unpack_from('<HHI', fmt)
    bits = struct.unpack_from('<H', fmt, 14)[0]
    if encoding == 0xfffe: encoding = struct.unpack_from('<I', fmt, 24)[0]
    assert encoding == 3 and bits == 32 and payload is not None
    samples = [item[0] for item in struct.iter_unpack('<f', payload)]
    assert len(samples) % channels == 0
    return rate, channels, samples

def verify(receipt):
    root = receipt.parent
    a = wav(root / 'a.wav'); b = wav(root / 'b.wav')
    assert a[:2] == b[:2] == (48000, 2)
    rows = json.loads(receipt.read_text(encoding='utf-8'))
    observations = []
    versions = {}
    for row in rows:
        run = Path(row['directory'])
        rate, channels, actual = wav(run / 'output.wav')
        assert (rate, channels, len(actual)) == (48000, 2, 1026)
        coefficient = .25 if row['version'] == 1 else .5
        expected = [.5 * x - coefficient * y for x, y in zip(a[2], b[2], strict=True)]
        errors = [abs(x - y) for x, y in zip(actual, expected, strict=True)]
        assert all(error <= 2e-6 + 1e-5 * abs(y) for error, y in zip(errors, expected, strict=True))
        record = json.loads((run / 'run.json').read_text(encoding='utf-8'))
        assert record['frames'] == 513 and record['channels'] == 2 and record['sample_rate_hz'] == 48000
        assert record['output_sha256'] == digest(run / 'output.wav')
        provenance = record['task_artifacts'][0]['provenance']
        helper = 'workspace_src/print_a/filter.py'
        fixture = ROOT / 'tests/fixtures/node_workspace' / ('python_two_files' if row['version'] == 1 else 'python_two_files_v2')
        assert provenance['code_files'][helper] == digest(fixture / 'src/print_a/filter.py')
        package = run / provenance['frozen_package_root']
        manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8'))
        assert manifest['workspace_content_sha256'] == json.loads((ROOT / 'tests/fixtures/node_workspace/expected/workspace_lock.json').read_text(encoding='utf-8'))['workspaces'][fixture.name]['content_sha256']
        versions.setdefault(row['version'], set()).add(digest(run / 'output.wav'))
        observations.append(dict(row, run_id=record['run_id'], output_sha256=record['output_sha256'], max_abs_error=max(errors), helper_sha256=provenance['code_files'][helper], workspace_content_sha256=manifest['workspace_content_sha256']))
    assert len(observations) == 5 and all(len(v) == 1 for v in versions.values()) and versions[1] != versions[2]
    return {'scope': 'component_GUI_and_real_CLI_independent_audio', 'desktop': 'not_verified', 'observations': observations}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('receipt', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.receipt), ensure_ascii=False, indent=2))
