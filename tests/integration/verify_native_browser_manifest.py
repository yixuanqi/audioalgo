"""Exercise browser-bearing native manifests through the production CLI loader."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--browser-dll', type=Path, required=True)
    args = parser.parse_args()
    output = ROOT / 'build/reports/loops/CNL0001/nlc07b/artifacts' / uuid.uuid4().hex
    output.mkdir(parents=True)
    print('NLC07B_ARTIFACT_ROOT=' + output.as_posix(), flush=True)
    header = (ROOT / 'tests/fixtures/native_browser_v1/manifest.h').read_text(encoding='utf-8')
    manifest = json.loads(re.search(r'R"json\((.*?)\)json"', header, re.S).group(1))
    original_hash = sha(args.browser_dll)
    package = output / 'good'; package.mkdir()
    shutil.copyfile(args.browser_dll, package / manifest['library'])
    (package / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
    env = os.environ.copy()
    env.update(AUDIOALGO_AUTHORING_ROOT=str(output / 'empty-authoring'), AUDIOALGO_PLUGIN_PATH='')
    commands = []
    observations = []

    def run(argv, expected=0, parse_json=True):
        result = subprocess.run([str(x) for x in argv], cwd=ROOT, env=env, capture_output=True, timeout=45)
        log = output / f'{len(commands):02}.log'; log.write_bytes(result.stdout + result.stderr)
        commands.append(dict(argv=[str(x) for x in argv], cwd=str(ROOT), exit_code=result.returncode,
                             log_path=log.relative_to(ROOT).as_posix(), log_sha256=sha(log)))
        assert result.returncode == expected, (argv, result.returncode, log)
        return json.loads(result.stdout) if parse_json else result.stdout

    error = None
    try:
        catalog = run([args.cli, '--list-nodes', '--plugin-dir', package])
        assert catalog['diagnostics'] == []
        node = [x for x in catalog['nodes'] if x['type_id'] == 'fixture.native_browser']
        assert len(node) == 1 and node[0]['library_sha256'] == original_hash
        assert node[0]['manifest_sha256'] == sha(package / 'manifest.json')
        observations.append(dict(case='matching_complete_manifest', status='loaded'))
        changes = {'category_id': 'audio.filters', 'subcategory_id': 'other', 'tags': ['changed'],
                   'description': 'changed', 'schema_version': 2, 'future_field': 'new'}
        variants = []
        for field, value in changes.items():
            changed = copy.deepcopy(manifest); changed['nodes'][0]['browser'][field] = value
            variants.append(('browser-' + field, changed, 'plugin_descriptor_mismatch'))
        for name, change, code in [
            ('abi', {'abi_version': 2}, 'plugin_abi_incompatible'),
            ('architecture', {'architecture': 'arm64'}, 'plugin_platform_incompatible'),
            ('platform', {'platform': 'linux'}, 'plugin_platform_incompatible'),
            ('missing-library', {'library': 'absent.dll'}, 'plugin_library_missing'),
            ('escape-library', {'library': '../escape.dll'}, 'plugin_manifest_invalid'),
            ('version', {'version': '99.0.0'}, 'plugin_descriptor_mismatch')]:
            variants.append((name, manifest | change, code))
        for name, changed, code in variants:
            target = output / 'negative' / name; shutil.copytree(package, target)
            (target / 'manifest.json').write_text(json.dumps(changed, ensure_ascii=False), encoding='utf-8')
            rejected = run([args.cli, '--list-nodes', '--plugin-dir', target], 1)
            assert [x['code'] for x in rejected['diagnostics']] == [code], (name, rejected)
            assert all(x['type_id'] != 'fixture.native_browser' for x in rejected['nodes'])
            assert any(x['type_id'] == 'builtin.gain' for x in rejected['nodes'])
            assert sha(target / manifest['library']) == original_hash
            observations.append(dict(case=name, expected_code=code, no_partial_node=True,
                                     manifest_sha256=sha(target / 'manifest.json')))
        duplicates = output / 'duplicates'
        for name in ['01-good', '02-duplicate']: shutil.copytree(package, duplicates / name)
        rejected = run([args.cli, '--list-nodes', '--plugin-dir', duplicates], 1)
        assert [x['code'] for x in rejected['diagnostics']] == ['plugin_id_duplicate']
        assert len([x for x in rejected['nodes'] if x['type_id'] == 'fixture.native_browser']) == 1
        observations.append(dict(case='duplicate-plugin-id', expected_code='plugin_id_duplicate', registered_count=1))

        spec = importlib.util.spec_from_file_location('native_oracle', ROOT / 'tests/integration/verify_native_plugin.py')
        oracle = importlib.util.module_from_spec(spec); spec.loader.exec_module(oracle)
        samples = [[oracle.f32(.12*math.sin((i+1)*(c+1)*.037)) for i in range(513)] for c in range(2)]
        oracle.write_wave(output / 'input.wav', samples, 48000)
        project = dict(schema_version=1, project_id='browser-native-current', name='Browser native', revision=1,
            selected_output_node_id='native', selected_output_port_id='audio_out',
            nodes=[dict(id='file', type_id='builtin.file_source', x=0, y=0, parameters=dict(path=str(output / 'input.wav'))),
                   dict(id='native', type_id='fixture.native_browser', x=250, y=0, parameters={})],
            connections=[dict(from_node='file', from_port='audio', to_node='native', to_port='audio_in')])
        (output / 'graph.aaproj').write_text(json.dumps(project), encoding='utf-8')
        run([args.cli, 'run', output / 'graph.aaproj', output / 'run', 127, '--plugin-dir', package], parse_json=False)
        rate, actual = oracle.read_wave(output / 'run/output.wav')
        assert rate == 48000 and [list(channel) for channel in actual] == samples
        report = json.loads((output / 'run/run.json').read_text(encoding='utf-8'))
        assert report['block_size'] == 127 and report['frames'] == 513 and report['channels'] == 2
        assert len(report['plugins']) == 1 and report['plugins'][0]['library_sha256'] == original_hash
        assert report['plugins'][0]['manifest_sha256'] == sha(package / 'manifest.json')
        assert sha(output / 'run/output.wav') == report['output_sha256']
        assert sha(args.browser_dll) == original_hash == sha(package / manifest['library'])
        result = dict(status='passed', scope='I production CLI loader; existing component classification observation separate',
                      dll_sha256=original_hash, manifest_sha256=sha(package / 'manifest.json'), observations=observations,
                      command_count=len(commands), commands=commands, audio_samples=1026, maximum_error=0.0)
    except BaseException as failure:
        error = failure
        result = dict(status='failed', error=str(failure), observations=observations, commands=commands)
    (output / 'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if error is not None: raise error
    print(json.dumps({k: result[k] for k in ['status', 'command_count', 'audio_samples', 'maximum_error']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
