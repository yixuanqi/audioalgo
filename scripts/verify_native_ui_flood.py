"""Read actual production native flood artifacts and independent PCM expectations."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET

from verify_python_mixed import f32, io_path, read_wav, sha
from verify_python_snapshot import npy, snapshot
from verify_native_log_tail import inspect_capture

NAME = 'PC-04.d production native log flood obeys UI budgets during a ten second mixed Run and recovers'
LIMIT = 16 * 1024 * 1024


def load(path):
    return json.loads(io_path(path).read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('junit', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    folder = repo / 'build/reports/loops/PC0614-native-ui-flood'
    case = next(c for c in ET.parse(args.junit).iter('testcase') if c.get('name') == NAME)
    assert not any(case.findall(k) for k in ['failure', 'error', 'skipped'])
    root = Path(next(line.split('=', 1)[1] for line in case.findtext('system-out', '').splitlines()
                     if line.startswith('PC0614_FLOOD_ROOT='))).resolve()
    assert root.parent == folder / 'artifacts'
    identity = load(root / 'dll-identity.json')
    dll = Path(identity['dll'])
    assert sha(dll) == identity['sha256'] == load(folder.parent / 'PC0606-native-budget/independent-data.json')['dll_sha256']
    rows = load(root / 'flood-demo.json')
    assert [r['mode'] for r in rows] == ['exact', 'plus-one', 'flood']
    exact = rows[0]
    assert exact['window_ms'] >= 10000 and exact['interval_ms'] == 20 and exact['samples'] >= 100
    assert exact['p95_ms'] <= 100 and exact['max_ms'] <= 500 and 'Rendering revision' in exact['terminal']
    for row in rows:
        assert row['maximum_ui_bytes'] <= 256 * 1024
        if row['mode'] != 'exact': assert row['terminal'].startswith('log_limit:')
        ui = (root / (row['mode'] + '-ui.log')).read_bytes()
        copy = (root / (row['mode'] + '-copy.log')).read_bytes()
        assert ui == copy and len(ui) <= 256 * 1024 and b'native' in ui and b'F' * 1024 in ui
    rate, source = read_wav(root / 'input.wav')
    assert rate == 48000 and [list(c) for c in source] == [
        [f32(.125 * math.sin(.03 * (n + 1)) + .02 * c) for n in range(513)] for c in range(2)]
    expected = [[f32(2 * x) for x in c] for c in source]
    captures, archives = [], []
    for path in (root / 'run-logs').glob('*/events.jsonl'):
        events = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
        assert events and len({(e['project_id'], e['session_epoch'], e['request_id'], e['revision']) for e in events}) == 1
        project_id = events[0]['project_id']
        mode = project_id.removeprefix('pc0614-')
        assert mode in ['exact', 'plus-one', 'flood'] and all(e['revision'] == 41 for e in events)
        native = [e for e in events if e['source'] == 'native']
        assert len(native) == 16384 and all(
            e['node_id'] == 'native' and e['type_id'] == 'example.sdk_logging'
            and e['package_id'] == 'example.sdk_logging.budget' and e['content_sha256'] == sha(dll)
            and e['sequence'] == index + 1 and e['text'] == 'F' * 1024
            and e['total_bytes'] == (index + 1) * 1024 for index, e in enumerate(native))
        raw = Path(native[0]['raw_log_file'])
        assert all(e['raw_log_file'] == str(raw) for e in native)
        assert raw.resolve().parent == path.resolve().parent and raw.read_bytes() == b'F' * LIMIT
        summary = load(raw.with_suffix('.summary.json'))
        assert summary['persisted_bytes'] == LIMIT and summary['persisted_messages'] == 16384
        assert summary['failure_code'] == ('' if mode == 'exact' else 'log_limit')
        diagnostics = [e for e in events if e['source'] == 'diagnostic']
        if mode == 'exact':
            assert not diagnostics and any('pc0614-native-burst-finished' in e['text'] for e in events if e['source'] == 'stdout')
        else:
            assert diagnostics and all(e['node_id'] == 'native' and e['text'].startswith('log_limit:') for e in diagnostics)
            assert not any(e['node_id'] == 'python' for e in events)
        captures.append(dict(mode=mode, **inspect_capture(raw, events)))
        archives.append(dict(mode=mode, request=events[0]['request_id'], raw_sha256=sha(raw)))
    assert sorted(a['mode'] for a in archives) == ['exact', 'flood', 'plus-one']
    outputs, code_hashes, runner_hashes = [], set(), set()
    runs = list((root / 'runs').iterdir())
    assert len(runs) == 3
    for run in runs:
        record, complete = load(run / 'run.json'), load(run / 'complete.json')
        mode = record['project_id'].removeprefix('pc0614-')
        assert mode in ['exact', 'plus-one-recovery', 'flood-recovery']
        out_rate, output = read_wav(run / 'output.wav')
        assert out_rate == 48000 and [list(c) for c in output] == expected
        assert record['frames'] == 513 and record['channels'] == 2 and record['block_size'] == 1
        assert record['valid_start_frame'] == 0 and record['valid_frame_count'] == 513 and record['project_revision'] == 41
        assert record['output_sha256'] == complete['output_sha256'] == sha(run / 'output.wav')
        assert len(record['plugins']) == 1 and record['plugins'][0]['library_sha256'] == sha(dll)
        for resource in record['resources']:
            assert resource['sha256'] == resource['snapshot_sha256'] == sha(root / 'input.wav') == sha(run / resource['snapshot_path'])
        project = load(root / (mode + '.aaproj'))
        assert project['project_id'] == record['project_id'] and project['revision'] == 41
        assert len(record['task_artifacts']) == 1
        task = record['task_artifacts'][0]
        provenance, directory = task['provenance'], run / task['directory']
        assert provenance['node_id'] == 'python' and provenance['request_id'] == record['request_id'] and provenance['process_id'] > 0
        code = directory / provenance['code_snapshot']
        files = {p.relative_to(io_path(code)).as_posix(): sha(p) for p in io_path(code).rglob('*') if p.suffix == '.py' or p.name == 'manifest.json'}
        digest = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        assert files == provenance['code_files'] and digest == provenance['code_sha256'] == code.name
        code_hashes.add(digest)
        snapshot(run / provenance['frozen_package_root'])
        runner_hashes.add(snapshot(run / provenance['runner_snapshot']))
        node = next(n for n in project['nodes'] if n['id'] == 'python')
        lock = node['managed_version']
        installed = root / 'library/packages' / lock['package_id'] / lock['content_sha256']
        assert io_path(code / 'user_source.py').read_bytes() == io_path(installed / 'user_source.py').read_bytes()
        assert sha(Path(provenance['environment']['executable'])) == provenance['interpreter_sha256']
        meta = provenance['inputs']['audio_in']
        input_path = directory / Path(provenance['result_manifest']).parent / meta['file']
        assert sha(input_path) == meta['sha256'] and [list(c) for c in npy(input_path)] == expected
        meta = provenance['outputs']['audio_out']
        output_path = directory / meta['path']
        assert sha(output_path) == meta['sha256'] and [list(c) for c in npy(output_path)] == expected
        assert meta['shape'] == [2, 513] and meta['sample_rate_hz'] == 48000 and meta['channel_ids'] == ['ch0', 'ch1'] and meta['valid_interval'] == [0, 513]
        outputs.append(dict(mode=mode, output_sha256=sha(run / 'output.wav'), request=record['request_id']))
    assert len({o['output_sha256'] for o in outputs}) == len(code_hashes) == len(runner_hashes) == 1
    red_case = next(c for c in ET.parse(folder / 'target.xml').iter('testcase') if c.get('name') == NAME)
    assert any(f.get('message') == 'p95 <= 100' for f in red_case.findall('failure'))
    red_root = Path(next(line.split('=', 1)[1] for line in red_case.findtext('system-out', '').splitlines() if line.startswith('PC0614_FLOOD_ROOT=')))
    assert load(red_root / 'flood-demo.json')[0]['p95_ms'] > 100
    report = dict(status='passed', root=str(root), successful_wavs=3, failed_requests=2, npy_boundaries=6,
                  native_raw_streams=3, bytes_per_raw=LIMIT, maximum_error=0, heartbeat=exact,
                  captures=captures, archives=archives, outputs=outputs, code_sha256=next(iter(code_hashes)),
                  runner_sha256=next(iter(runner_hashes)), dll_sha256=sha(dll),
                  scope='Actual EditorComponent Run exact/+1/flood native burst then Python worker gate; ten-second heartbeat includes burst with mixed task active. Not continuous native events for all ten seconds, full Check/native lifecycle/desktop or final platform.')
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ['status', 'successful_wavs', 'failed_requests', 'npy_boundaries', 'native_raw_streams', 'maximum_error', 'heartbeat']}))


if __name__ == '__main__':
    main()
