"""Independent resource package and float64 numerical oracle; never imports user code."""
import argparse
import json
import math
from pathlib import Path
import struct
from verify_workspace_audio import wav, digest
from verify_workspace_package import verify


def run(receipt):
    rows = json.loads(receipt.read_bytes());root = receipt.parent;observations = []
    assert len(rows) == 2
    for index, row in enumerate(rows):
        package = list((root / 'library/packages').glob('*/' + Path(row['package']).name))
        assert len(package) == 1
        inventory = verify(Path(row['author']), package[0], count=5)
        installed = Path(inventory['package'])
        assert (installed / 'weights.txt').read_text() == ('0.25' if index == 0 else '0.75')
        assert (installed / 'workspace_src/assets/local.txt').read_text() == '0.5'
        rate, channels, samples = wav(root / f'v{index+1}.wav')
        assert (rate, channels, len(samples)) == (16000, 1, 513)
        inputs = [struct.unpack('<f', struct.pack('<f', .2 * math.sin(.03 * (n + 1))))[0] for n in range(513)]
        expected = [v * (.125 if index == 0 else .375) for v in inputs]
        errors = [abs(a-b) for a,b in zip(samples, expected)]
        assert all(e <= 2e-6 + 1e-5 * abs(v) for e,v in zip(errors, expected))
        observations.append(dict(version=index+1, **inventory, max_abs_error=max(errors),
            audio_sha256=digest(root / f'v{index+1}.wav')))
    assert observations[0]['audio_sha256'] != observations[1]['audio_sha256']
    return dict(desktop='not_verified', observations=observations)


if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('receipt', type=Path)
    print(json.dumps(run(parser.parse_args().receipt), indent=2))
