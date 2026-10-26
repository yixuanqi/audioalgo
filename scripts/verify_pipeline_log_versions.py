"""Read saved projects, raw logs, executed source and WAVs without importing a node."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def f32(value: float) -> float:
    return struct.unpack('<f', struct.pack('<f', value))[0]


def verify(root: Path, active_apply: bool = False) -> dict:
    root = io_path(root)
    demo = json.loads((root / 'identity-demo.json').read_text(encoding='utf-8'))
    if [row['version'] for row in demo] != [1, 2, 1] or len({r['request_id'] for r in demo}) != 3:
        raise ValueError('expected three distinct real runs v1 -> v2 -> v1')
    input_format, input_bytes = read_float_wav(root / 'input.wav')
    samples = struct.unpack('<' + 'f' * (len(input_bytes) // 4), input_bytes)
    if input_format != (1, 16000, 32) or len(samples) != 513:
        raise ValueError('unexpected independent input format')
    all_events = [json.loads(line) for p in (root / 'run-logs').glob('*/events.jsonl')
                  for line in p.read_text(encoding='utf-8').splitlines()]
    results = []
    for receipt in demo:
        project_file = root / receipt['project_file']
        project = json.loads(project_file.read_text(encoding='utf-8'))
        nodes = {node['id']: node for node in project['nodes']}
        if project['revision'] != 7 or project['project_id'] != f"pc03-project-v{receipt['version']}":
            raise ValueError('projects must have different IDs with the same revision')
        if len(project['connections']) != 3 or [nodes[n]['parameters']['gain'] for n in ('first', 'second')] != [.5, .25]:
            raise ValueError('saved graph does not contain the two independently configured instances')
        for node in ('first', 'second'):
            if nodes[node]['managed_version']['content_sha256'] != receipt['content_sha256']:
                raise ValueError('project exact lock differs from the executed receipt')
        run = root / receipt['run_directory']
        record = json.loads((run / 'run.json').read_text(encoding='utf-8'))
        if record['request_id'] != receipt['request_id'] or record['project_id'] != project['project_id'] or record['project_revision'] != 7:
            raise ValueError('run/project identity mismatch')
        fmt, data = read_float_wav(run / 'output.wav')
        if fmt != input_format or len(data) != len(input_bytes) or record['frames'] != 513 or record['valid_frame_count'] != 513:
            raise ValueError('output format/frame/valid interval changed')
        actual = struct.unpack('<' + 'f' * len(samples), data)
        multiplier = receipt['version']
        expected = [f32(f32(f32(f32(x * .5) * multiplier) * .25) * multiplier) for x in samples]
        errors = [abs(a - b) for a, b in zip(actual, expected)]
        if any(e > 2e-6 + 1e-5 * abs(b) for e, b in zip(errors, expected)):
            raise ValueError('independent per-stage float32 formula mismatch')
        if sha256(run / 'output.wav') != record['output_sha256']:
            raise ValueError('output receipt hash mismatch')
        events = [e for e in all_events if e['request_id'] == receipt['request_id']]
        if {e['node_id'] for e in events} != {'first', 'second'}:
            raise ValueError('log instance set mismatch')
        ui = (root / receipt['ui_log']).read_text(encoding='utf-8')
        if 'late-old-project' in ui or receipt['request_id'] not in ui or receipt['content_sha256'] not in ui:
            raise ValueError('UI accepted an old-project event or lost identity')
        raw_checks = []
        for node, gain in [('first', .5), ('second', .25)]:
            stream = [e for e in events if e['node_id'] == node]
            if any(e['project_id'] != project['project_id'] or e['revision'] != 7 or
                   e['session_epoch'] != record['session_epoch'] or e['content_sha256'] != receipt['content_sha256'] or
                   e['package_id'] != nodes[node]['managed_version']['package_id'] or e['type_id'] != nodes[node]['type_id'] for e in stream):
                raise ValueError('node/type/package/run identity mismatch')
            if any(a['sequence'] >= b['sequence'] for a, b in zip(stream, stream[1:])):
                raise ValueError('node log sequence is not strictly increasing')
            raw_paths = list(dict.fromkeys(io_path(e['raw_log_file']) for e in stream))
            raw = b''.join(p.read_bytes() for p in raw_paths)
            text = ''.join(e['text'] for e in stream)
            marker = f"pc03-v{multiplier} gain={gain}"
            if raw.decode('utf-8') != text or raw.decode('utf-8').splitlines() != [marker, marker]:
                raise ValueError('raw/event marker content or block count differs')
            if stream[-1]['total_bytes'] != len(raw) or any(a['total_bytes'] > b['total_bytes'] for a, b in zip(stream, stream[1:])):
                raise ValueError('raw byte accounting mismatch')
            task = next(a for a in record['task_artifacts'] if a['provenance']['node_id'] == node)
            artifact = task['provenance']
            # Runner run_root is the private task directory. Host-added frozen
            # package paths are relative to the outer pipeline run directory.
            code = run / task['directory'] / artifact['code_snapshot'] / 'user_source.py'
            frozen = run / artifact['frozen_package_root'] / 'user_source.py'
            if sha256(code) != artifact['code_files']['user_source.py'] or code.read_bytes() != frozen.read_bytes():
                raise ValueError('actually executed source differs from frozen package')
            if f"pc03-v{multiplier}" not in code.read_text(encoding='utf-8'):
                raise ValueError('executed source belongs to another version')
            raw_checks.append({'node': node, 'bytes': len(raw), 'source_sha256': sha256(code),
                               'raw_sha256': [sha256(p) for p in raw_paths]})
        results.append({'request_id': receipt['request_id'], 'project_sha256': sha256(project_file),
                        'content_sha256': receipt['content_sha256'], 'max_error': max(errors),
                        'output_sha256': record['output_sha256'], 'nodes': raw_checks})
    mutation = None
    if active_apply:
        content, original, changed = (root / 'mutation-receipt.txt').read_text(encoding='ascii').splitlines()
        first_project = json.loads((root / demo[0]['project_file']).read_text(encoding='utf-8'))
        node = next(n for n in first_project['nodes'] if n['id'] == 'first')
        installed = root / 'library/packages' / node['managed_version']['package_id'] / content / 'user_source.py'
        if content != demo[0]['content_sha256'] or sha256(installed) != original or original != results[0]['nodes'][0]['source_sha256']:
            raise ValueError('original installed code was not restored or execution did not use the pre-mutation source')
        if changed != hashlib.sha256(b"raise AssertionError('installed source changed after freeze')\n").hexdigest() or changed == original:
            raise ValueError('mutation receipt does not identify the actual distinct fault source')
        index = json.loads((root / 'library/index.json').read_text(encoding='utf-8'))
        if index['packages'][0]['content_sha256'] != demo[1]['content_sha256']:
            raise ValueError('published default did not advance to v2')
        if (root / 'run-gate.txt').exists() or (root / 'run-entered.txt').read_text(encoding='ascii') != 'entered':
            raise ValueError('active-run barrier was not entered/released')
        mutation = {'original_source_sha256': original, 'changed_source_sha256': changed, 'installed_restored': True,
                    'note': 'Active barrier order and mutated-file byte readback are primary component assertions; this reads the preserved post-run artifacts.'}
    return {'verified': True, 'scope': 'independent_artifact_subset', 'runs': results, 'mutation': mutation,
            'limitations': ['Apply is via the existing service while production Run is active; UI Apply and slow old-Run/new-Run matrix are separate.' if active_apply else
                            'Sequential project changes; Apply-during-active-Run and slow old-Run/new-Run matrix are not covered.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('artifact_root', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--active-apply', action='store_true')
    args = parser.parse_args()
    result = verify(args.artifact_root.resolve(), args.active_apply)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verified': True, 'runs': len(result['runs']), 'max_errors': [r['max_error'] for r in result['runs']]}))
