"""Independently read native queue close/recovery WAVs and archived log bytes."""
import argparse
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET

from verify_python_mixed import f32, read_wav, sha
from verify_native_log_tail import inspect_capture

NAME = 'PC-03.c active native View and queue close reject queued logs drain owned modules and permit recovery'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('junit', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    folder = repo / 'build/reports/loops/PC0613-native-close'
    case = next(c for c in ET.parse(args.junit).iter('testcase') if c.get('name') == NAME)
    assert not any(case.findall(k) for k in ['failure', 'error', 'skipped'])
    root = Path(next(line.split('=', 1)[1] for line in case.findtext('system-out', '').splitlines()
                     if line.startswith('PC0613_CLOSE_ROOT='))).resolve()
    assert root.parent == folder / 'artifacts'
    identity = load(root / 'dll-identity.json')
    dll = Path(identity['dll'])
    assert sha(dll) == identity['sha256'] == load(folder.parent / 'PC0611-detach-drain/independent-data.json')['dll_sha256']
    project = load(root / 'native-close.aaproj')
    assert project['project_id'] == 'pc0613-native-close' and project['revision'] == 37
    assert {n['id'] for n in project['nodes']} == {'file', 'native-A', 'native-B', 'output'}
    rate, source = read_wav(root / 'input.wav')
    assert rate == 48000 and [list(c) for c in source] == [
        [f32(.125 * math.sin(.03 * (n + 1)) + .02 * c) for n in range(513)] for c in range(2)]
    rows = load(root / 'close-demo.json')
    assert [r['mode'] for r in rows] == ['view', 'application']
    assert len({r[k] for r in rows for k in ['request', 'recovery_request']}) == 4
    captures, outputs = [], []
    for row in rows:
        assert row['old_logs'] == row['old_completions'] == 0
        assert all(row[k] is True for k in ['expired_dispatch', 'expired_callbacks', 'expired_catalog', 'module_unloaded'])
        assert row['shutdown_callbacks'] == (1 if row['mode'] == 'application' else 0)
        assert row['shutdown_timed_out'] is False and 0 < row['close_ms'] <= 5000
        assert row['recovery_logs'] == 4 and row['recovery_completions'] == 1
        assert row['active'] == row['pending'] == row['jobs'] == 0 and row['worker_claimed'] is False
        for recovery in [False, True]:
            path = Path(row['recovery_archive' if recovery else 'archive']).resolve()
            assert path.parent.parent == root / 'run-logs'
            events = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
            assert len(events) == 4 and all(
                e['source'] == 'native' and e['level'] == 'info' and e['project_id'] == project['project_id']
                and e['session_epoch'] == 59 and e['revision'] == 37
                and e['request_id'] == row['recovery_request' if recovery else 'request']
                and e['type_id'] == 'example.sdk_logging' and e['package_id'] == 'example.sdk_logging.detach'
                and e['content_sha256'] == sha(dll) for e in events)
            raw_paths = set()
            for node in ['native-A', 'native-B']:
                stream = [e for e in events if e['node_id'] == node]
                assert [e['text'] for e in stream] == ['F' * 16, 'sdk-detach accepted\n']
                assert [e['sequence'] for e in stream] == [1, 2] and [e['total_bytes'] for e in stream] == [16, 36]
                raw = Path(stream[0]['raw_log_file']).resolve()
                assert raw.parent == path.parent and raw not in raw_paths
                raw_paths.add(raw)
                assert raw.read_bytes() == b'F' * 16 + b'sdk-detach accepted\n'
                capture = inspect_capture(raw, events)
                summary = load(raw.with_suffix('.summary.json'))
                assert summary['accepted_bytes'] == summary['persisted_bytes'] == 36
                assert summary['accepted_messages'] == summary['persisted_messages'] == 2
                assert summary['raw_complete'] and summary['all_messages_accepted'] and not summary['failure_code']
                captures.append(dict(mode=row['mode'], recovery=recovery, node=node, **capture))
        assert not (root / 'runs' / (row['mode'] + '-closed')).exists()
        assert not (root / 'runs' / (row['mode'] + '-rejected')).exists()
        run = root / 'runs' / (row['mode'] + '-recovery')
        record, complete = load(run / 'run.json'), load(run / 'complete.json')
        out_rate, output = read_wav(run / 'output.wav')
        assert out_rate == 48000 and [list(c) for c in output] == [[f32(4 * x) for x in c] for c in source]
        assert record['frames'] == 513 and record['channels'] == 2 and record['block_size'] == 127
        assert record['valid_start_frame'] == 0 and record['valid_frame_count'] == 513
        assert record['request_id'] == row['recovery_request'] and record['project_revision'] == 37 and record['session_epoch'] == 59
        assert len(record['plugins']) == 2 and all(p['library_sha256'] == sha(dll) for p in record['plugins'])
        assert record['output_sha256'] == complete['output_sha256'] == sha(run / 'output.wav')
        for resource in record['resources']:
            assert resource['sha256'] == resource['snapshot_sha256'] == sha(root / 'input.wav') == sha(run / resource['snapshot_path'])
        outputs.append(dict(mode=row['mode'], sha256=sha(run / 'output.wav')))
    assert len(outputs) == 2 and outputs[0]['sha256'] == outputs[1]['sha256']
    assert len(list((root / 'runs').iterdir())) == 2
    report = dict(status='passed', root=str(root), successful_wavs=2, closed_runs=2, native_raw_streams=8,
                  maximum_error=0, close_ms={r['mode']: r['close_ms'] for r in rows}, captures=captures,
                  outputs=outputs, dll_sha256=sha(dll),
                  scope='Actual queue worker/dispatch View destruction and application-style pool drain; real queued stale native logs rejected, callback/catalog/module leases released, fresh queue recovery. Not outer EditorComponent/Main window, OS process/canary/crash isolation, flood or desktop.')
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ['status', 'successful_wavs', 'closed_runs', 'native_raw_streams', 'maximum_error', 'close_ms']}))


if __name__ == '__main__':
    main()
