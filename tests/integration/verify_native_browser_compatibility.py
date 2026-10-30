"""Run a frozen pre-browser ABI v1 DLL in current component/CLI hosts; never compile the DLL."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--config', default='Debug')
    args = parser.parse_args()
    binary = args.build_dir.resolve() / 'bin'
    output = ROOT / 'build/reports/loops/CNL0001/nlc07a/artifacts' / uuid.uuid4().hex
    output.mkdir(parents=True)
    print('NLC07A_ARTIFACT_ROOT=' + output.as_posix(), flush=True)
    baseline_path = ROOT / 'build/reports/loops/PC0601-native-log-queue/baseline-identity.json'
    baseline = json.loads(baseline_path.read_text(encoding='utf-8'))
    old = [x for x in baseline['artifacts'] if x['path'].endswith('external_stateful.dll')]
    assert len(old) == 1
    original = Path(old[0]['path']); assert sha(original) == old[0]['sha256']
    old_manifest = [x for x in baseline['artifacts'] if Path(x['path']) == original.parent / 'manifest.json']
    assert len(old_manifest) == 1 and sha(original.parent / 'manifest.json') == old_manifest[0]['sha256']
    assert sha(ROOT / baseline['old_report']) == baseline['old_report_sha256']
    manifest_bytes = (original.parent / 'manifest.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    assert manifest['abi_version'] == 1 and all('browser' not in x for x in manifest['nodes'])
    package = output / 'old-package'; package.mkdir()
    shutil.copyfile(original, package / original.name)
    (package / 'manifest.json').write_bytes(manifest_bytes)
    host_hashes = {str(p): sha(p) for p in [binary / 'audio_cli.exe', binary / 'audio_native_plugin_tests.exe', binary / 'AudioAlgo Editor.exe'] if p.is_file()}
    env = os.environ.copy()
    env.update(AUDIOALGO_AUTHORING_ROOT=str(output / 'empty-authoring'), AUDIOALGO_PLUGIN_PATH='',
               AUDIOALGO_UPN_PACKAGE=str(package), AUDIOALGO_UPN_OUTPUT=str(output / 'audio'))
    (output / 'audio').mkdir()
    commands = []

    def run(argv):
        result = subprocess.run([str(x) for x in argv], cwd=ROOT, env=env, capture_output=True, timeout=45)
        log = output / f'{len(commands):02}.log'; log.write_bytes(result.stdout + result.stderr)
        commands.append(dict(argv=[str(x) for x in argv], cwd=str(ROOT), exit_code=result.returncode,
                             log_path=log.relative_to(ROOT).as_posix(), log_sha256=sha(log)))
        assert result.returncode == 0, (argv, result.returncode, log, result.stdout[-2500:])
        return result.stdout

    spec = importlib.util.spec_from_file_location('native_oracle', ROOT / 'tests/integration/verify_native_plugin.py')
    oracle = importlib.util.module_from_spec(spec); spec.loader.exec_module(oracle)
    a = [[oracle.f32(.12*math.sin((i+1)*(c+1)*.037)) for i in range(513)] for c in range(2)]
    b = [[oracle.f32(.07*math.cos((i+3)*(c+1)*.021)) for i in range(513)] for c in range(2)]
    for name, samples in [('a', a), ('b', b)]: oracle.write_wave(output / 'audio' / (name + '.wav'), samples, 48000)
    error = None
    try:
        catalog_bytes = run([binary / 'audio_cli.exe', '--list-nodes', '--plugin-dir', package])
        catalog = json.loads(catalog_bytes)
        (output / 'cli-catalog.json').write_bytes(catalog_bytes)
        assert not catalog['diagnostics']
        assert len([x for x in catalog['nodes'] if x['type_id'] == 'example.external_stateful']) == 1
        run([binary / 'audio_native_plugin_tests.exe', '--reporter', 'junit', '--out', output / 'component.xml'])
        junit = ET.parse(output / 'component.xml').getroot()
        assert len(list(junit.iter('testcase'))) == 2 and not list(junit.iter('failure')) and not list(junit.iter('skipped'))
        assert json.loads((output / 'audio/component-catalog.json').read_text(encoding='utf-8')) == catalog
        errors = {}; sample_count = 0; run_ids = []
        for port in ['main', 'side']:
            cli_output = output / 'audio' / ('cli-' + port)
            run([binary / 'audio_cli.exe', 'run', output / 'audio' / (port + '.aaproj'), cli_output, 127, '--plugin-dir', package])
            for folder in [output / 'audio' / ('component-' + port), cli_output]:
                rate, samples = oracle.read_wave(folder / 'output.wav')
                assert rate == 48000 and len(samples) == 2 and all(len(x) == 513 for x in samples)
                maximum = 0.
                for c in range(2):
                    for i in range(513):
                        first = oracle.f32(1.5*a[c][i] + b[c][i] + .00001*i)
                        expected = .5*first + b[c][i] - .00002*i if port == 'main' else first-b[c][i]
                        maximum = max(maximum, abs(samples[c][i] - expected)); sample_count += 1
                assert maximum < 1e-6, (folder, maximum)
                report = json.loads((folder / 'run.json').read_text(encoding='utf-8'))
                assert {p['node_id'] for p in report['plugins']} == {'first', 'second'}
                assert all(p['library_sha256'] == old[0]['sha256'] and p['manifest_sha256'] == sha(package / 'manifest.json') and
                           p['plugin_version'] == manifest['version'] for p in report['plugins'])
                assert sha(folder / 'output.wav') == report['output_sha256'] and (folder / 'complete.json').is_file()
                errors[folder.relative_to(ROOT).as_posix()] = maximum; run_ids.append(report['run_id'])
        assert len(set(run_ids)) == 4
        assert sha(original) == sha(package / original.name) == old[0]['sha256']
        assert (original.parent / 'manifest.json').read_bytes() == manifest_bytes == (package / 'manifest.json').read_bytes()
        assert all(sha(p) == expected for p, expected in host_hashes.items())
        result = dict(status='passed', scope='I current component/CLI with frozen old DLL; no real desktop',
                      old_dll_path=str(original), old_dll_sha256=old[0]['sha256'],
                      baseline_identity_path=baseline_path.relative_to(ROOT).as_posix(), baseline_identity_sha256=sha(baseline_path),
                      old_manifest_sha256=sha(original.parent / 'manifest.json'), no_browser=True, dll_recompiled=False,
                      host_hashes=host_hashes, commands=commands, output_maximum_errors=errors, sample_count=sample_count,
                      component_test_count=2, run_count=4, config=args.config)
    except BaseException as failure:
        error = failure
        result = dict(status='failed', error=str(failure), commands=commands, old_dll_path=str(original), old_dll_sha256=old[0]['sha256'])
    (output / 'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if error is not None: raise error
    print(json.dumps({k: result[k] for k in ['status', 'dll_recompiled', 'component_test_count', 'run_count', 'sample_count']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
