"""Independently inspect the real three-scenario live Run identity evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from verify_pipeline_filter_demo import read_float_wav
from verify_python_mixed import io_path


def sha(path: Path) -> str:
    return hashlib.sha256(io_path(path).read_bytes()).hexdigest()


def inspect(command_record: Path) -> dict:
    record = json.loads(command_record.read_text(encoding='utf-8'))
    junit = ET.parse(command_record.with_suffix('.xml'))
    if record['exit_code'] != 0 or junit.findall('.//failure') or junit.findall('.//skipped'):
        raise ValueError('primary component command must pass without skipped cases')
    prefix = 'PC0309_ARTIFACT_ROOTS='
    case_name = 'PC-03.a slow production Run cannot pollute a new project revision or replacement Run'
    cases = [case for case in junit.findall('.//testcase') if case.get('name') == case_name]
    if len(cases) != 1:
        raise ValueError('primary JUnit must execute exactly one live-identity case')
    sets = [json.loads(line[len(prefix):]) for output in cases[0].findall('system-out')
            for line in (output.text or '').splitlines() if line.startswith(prefix)]
    if len(sets) != 1:
        raise ValueError('primary JUnit must contain exactly one test-owned artifact set')
    artifacts = [io_path(Path(path)) for path in sets[0]]
    base = io_path(Path(record['cwd']) / 'build/reports/loops/PC0309-live-identity/artifacts')
    if any(path.parent != base for path in artifacts) or len(set(artifacts)) != 3:
        raise ValueError('artifact set must contain three distinct, confined scenario directories')
    if len(artifacts) != 3:
        raise ValueError(f'expected three complete scenarios from this exact command, found {len(artifacts)}')
    results = []
    for root in artifacts:
        scenario = json.loads((root / 'scenario.json').read_text(encoding='utf-8'))
        mode = scenario['mode']
        if mode not in (0, 1, 2) or scenario['old_request'] == scenario['current_request']:
            raise ValueError('invalid scenario or reused request identity')
        old = json.loads((root / 'old-event.json').read_text(encoding='utf-8'))
        if old['request_id'] != scenario['old_request'] or old['project_id'] != 'live-A' or old['revision'] != 7:
            raise ValueError('captured old identity is not the real A revision 7 event')
        successes = list((root / 'runs').glob('*/run.json'))
        if len(successes) != 1:
            raise ValueError('cancelled A must not publish another completed Run')
        run = successes[0].parent
        current = json.loads(successes[0].read_text(encoding='utf-8'))
        expected_project = 'live-B' if mode == 0 else 'live-A'
        expected_revision = 8 if mode == 1 else 7
        if current['request_id'] != scenario['current_request'] or current['project_id'] != expected_project or current['project_revision'] != expected_revision:
            raise ValueError('replacement Run identity does not match the scenario')
        wrong_session = current['session_epoch'] <= old['session_epoch'] if mode == 0 else current['session_epoch'] != old['session_epoch']
        if wrong_session:
            raise ValueError('project session did not advance/persist as required')
        snapshot = json.loads((run / current['project_snapshot']).read_text(encoding='utf-8'))
        custom = next(n for n in snapshot['nodes'] if n['id'] == 'custom')
        gain = .5 if mode == 2 else .25
        if custom['parameters']['gain'] != gain:
            raise ValueError('actual executed snapshot parameter differs from independent target')
        input_format, input_data = read_float_wav(root / 'input.wav')
        output_format, output_data = read_float_wav(run / 'output.wav')
        if input_format != (1, 16000, 32) or output_format != input_format or len(input_data) != 513 * 4 or len(output_data) != len(input_data):
            raise ValueError('audio shape/format mismatch')
        if current['frames'] != 513 or current['valid_start_frame'] != 0 or current['valid_frame_count'] != 513:
            raise ValueError('Run frame/valid interval mismatch')
        inputs = struct.unpack('<513f', input_data)
        output = struct.unpack('<513f', output_data)
        expected = [struct.unpack('<f', struct.pack('<f', x * gain))[0] for x in inputs]
        errors = [abs(a - b) for a, b in zip(output, expected)]
        if any(error > 2e-6 + 1e-5 * abs(value) for error, value in zip(errors, expected)):
            raise ValueError('output differs from independent float32 gain formula')
        if sha(run / 'output.wav') != current['output_sha256']:
            raise ValueError('output WAV hash does not match receipt')
        ui = (root / 'current-ui.log').read_text(encoding='utf-8')
        if scenario['current_request'] not in ui or scenario['old_request'] in ui or 'late-A-must-not-leak' in ui:
            raise ValueError('UI current log identity was polluted')
        playback = Path((root / 'playback-file.txt').read_text(encoding='utf-8'))
        if io_path(playback) != run / 'output.wav':
            raise ValueError('preview service received a stale output file')
        events = [json.loads(line) for path in (root / 'run-logs').glob('*/events.jsonl')
                  for line in path.read_text(encoding='utf-8').splitlines()]
        old_diagnostics = [e for e in events if e['request_id'] == scenario['old_request'] and e['source'] == 'diagnostic']
        if not old_diagnostics or not any('run_cancelled' in e['text'] for e in old_diagnostics):
            raise ValueError('old task cancellation diagnostic was not preserved')
        raw_rows = []
        for request, count, expected_gain in [(scenario['old_request'], 1, .5), (scenario['current_request'], 2, gain)]:
            owned = [e for e in events if e['request_id'] == request]
            stream = [e for e in owned if e['source'] == 'stdout']
            if not stream or {e['node_id'] for e in stream} != {'custom'}:
                raise ValueError('event node ownership mismatch')
            if any(e['content_sha256'] != custom['managed_version']['content_sha256'] or e['type_id'] != custom['type_id'] for e in owned):
                raise ValueError('event package/type ownership mismatch')
            raw_paths = list(dict.fromkeys(io_path(e['raw_log_file']) for e in stream))
            raw = b''.join(p.read_bytes() for p in raw_paths)
            if raw.decode('utf-8') != ''.join(e['text'] for e in stream):
                raise ValueError('raw stdout differs from archived events')
            lines = raw.decode('utf-8').splitlines()
            allowed_counts = (1, 2) if mode == 2 and request == scenario['old_request'] else (count,)
            if len(lines) not in allowed_counts or any(line != f'pc03-live gain={expected_gain}' for line in lines):
                raise ValueError('raw execution marker count or parameter differs')
            if stream[-1]['total_bytes'] != len(raw) or any(a['total_bytes'] > b['total_bytes'] for a, b in zip(stream, stream[1:])):
                raise ValueError('raw stdout byte accounting differs')
            raw_rows.append({'request': request, 'bytes': len(raw), 'sha256': [sha(p) for p in raw_paths]})
        task = next(a for a in current['task_artifacts'] if a['provenance']['node_id'] == 'custom')
        provenance = task['provenance']
        source = run / task['directory'] / provenance['code_snapshot'] / 'user_source.py'
        if sha(source) != provenance['code_files']['user_source.py']:
            raise ValueError('actual executed code hash mismatch')
        results.append({'mode': mode, 'artifact_root': str(root), 'old_request': scenario['old_request'],
                        'current_request': scenario['current_request'], 'project_id': expected_project,
                        'revision': expected_revision, 'gain': gain, 'max_error': max(errors),
                        'output_sha256': current['output_sha256'], 'executed_source_sha256': sha(source), 'raw_logs': raw_rows})
    if {r['mode'] for r in results} != {0, 1, 2}:
        raise ValueError('scenario matrix is incomplete')
    return {'verified': True, 'scope': 'independent_artifact_subset', 'primary_command': str(command_record),
            'scenarios': sorted(results, key=lambda row: row['mode']),
            'limitations': ['Process exit and before/after UI rejection are primary component assertions.',
                            'Playback destination is checked through an injected preview service; device playback and OS desktop remain unverified.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('command_record', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.command_record.resolve())
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'verified': True, 'modes': [r['mode'] for r in result['scenarios']],
                      'max_errors': [r['max_error'] for r in result['scenarios']]}))
