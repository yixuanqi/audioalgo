"""Read actual PC0503 outputs and closures with an independent stdlib f32 oracle."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32, io_path, read_wav, sha
from verify_python_snapshot import close, npy, snapshot

NAME = 'PC-05.b production Run and fresh CLI select named outputs with one shared upstream execution'


def read(path):
    return json.loads(io_path(path).read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('junit', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    case = next(c for c in ET.parse(args.junit).iter('testcase') if c.get('name') == NAME)
    assert case.find('failure') is None and case.find('error') is None
    line = next(s for s in case.findtext('system-out', '').splitlines() if s.startswith('PC05_NAMED_ROOT='))
    root = Path(line.split('=', 1)[1]).resolve()
    managed = Path(__file__).resolve().parents[1] / 'build/reports/loops/PC0503-named-shared/artifacts'
    assert root.parent == managed.resolve()
    rows = read(root / 'named-demo.json')
    assert len(rows) == 6 and len({r['request_id'] for r in rows}) == 6
    assert {(r['port'], r['origin'], r['repeat']) for r in rows} == {(p, o, n) for p in ('main', 'side') for o, n in (('gui', 0), ('gui', 1), ('cli', 0))}
    command = read(args.junit.with_suffix('.json'))
    hosts = read(root / 'host-hashes.json')
    for filename, field in (('audioalgo_tests.exe', 'test_host'), ('audio_cli.exe', 'cli')):
        assert next(a['sha256'] for a in command['artifacts'] if Path(a['path']).name == filename) == hosts[field]
    rate, source = read_wav(root / 'input.wav')
    assert rate == 48000 and len(source) == 2 and all(len(c) == 513 for c in source)
    maximum = close(source, [[f32(.2 * math.sin(.03 * (n + 1)) + .02 * c) for n in range(513)] for c in range(2)])
    scale = lambda audio, value: [[f32(x * value) for x in channel] for channel in audio]
    upstream = scale(source, .5)
    primary, reference = scale(upstream, 2), scale(upstream, .5)
    main_output = [[f32(f32(f32(p * 2) + f32(r * 3)) * .5) for p, r in zip(pc, rc)] for pc, rc in zip(primary, reference)]
    side = [[f32(p - r) for p, r in zip(pc, rc)] for pc, rc in zip(primary, reference)]
    expected_inputs = {'shared': {'audio_in': source}, 'named': {'primary': primary, 'reference': reference}}
    expected_outputs = {'shared': {'audio_out': upstream}, 'named': {'main': main_output, 'side': side}}
    source_hashes, runners, results = {}, set(), []
    expected_shared = ''.join(f'pc05-shared frame={n} count={min(127,513-n)}\r\n' for n in range(0, 513, 127)) + 'pc05-shared-reset frames=513 calls=5 reason=run_complete\r\n'
    expected_join = 'pc05-join count=127\r\n' * 4 + 'pc05-join count=5\r\n'
    archives = []
    for path in (root / 'run-logs').glob('*/events.jsonl'):
        events = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
        if events:
            archives.append((path, events))
    for row in rows:
        run, project_file = (root / row['run']).resolve(), (root / row['project']).resolve()
        assert run.is_relative_to(root) and project_file.is_relative_to(root)
        project = read(project_file); nodes = {n['id']: n for n in project['nodes']}
        assert project['selected_output_node_id'] == 'named' and project['selected_output_port_id'] == row['port']
        assert len([c for c in project['connections'] if c['from_node'] == 'shared']) == 2
        record, complete = read(run / 'run.json'), read(run / 'complete.json')
        assert read(run / record['project_snapshot']) == project
        assert record['request_id'] == row['request_id'] and record['project_id'] == project['project_id']
        assert record['project_revision'] == project['revision'] == 7 and record['block_size'] == 127
        assert record['sample_rate_hz'] == 48000 and record['channels'] == 2 and record['frames'] == 513
        assert record['valid_start_frame'] == 0 and record['valid_frame_count'] == 513
        assert record['output_sha256'] == complete['output_sha256'] == sha(run / 'output.wav')
        actual_rate, actual = read_wav(run / 'output.wav'); assert actual_rate == 48000
        maximum = max(maximum, close(actual, expected_outputs['named'][row['port']]))
        for resource in record['resources']:
            assert resource['sha256'] == resource['snapshot_sha256'] == sha(root / 'input.wav') == sha(run / resource['snapshot_path'])
        if row['origin'] == 'cli':
            text = (root / (row['port']+'-cli-stdout.txt')).read_text(encoding='utf-8')
            assert f"run_id={record['run_id']}" in text and f"output_sha256={record['output_sha256']}" in text
        artifacts = record['task_artifacts']
        assert len(artifacts) == 2 and {a['provenance']['node_id'] for a in artifacts} == {'shared', 'named'}
        for artifact in artifacts:
            p = artifact['provenance']; node = p['node_id']; directory = run / artifact['directory']
            assert p['request_id'] == row['request_id'] and p['process_id'] > 0
            assert p['interpreter_sha256'] == sha(Path(p['environment']['executable']))
            code = directory / p['code_snapshot']
            files = {path.relative_to(io_path(code)).as_posix(): sha(path) for path in io_path(code).rglob('*') if path.suffix == '.py' or path.name == 'manifest.json'}
            digest = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            assert files == p['code_files'] and digest == p['code_sha256'] == code.name
            snapshot(run / p['frozen_package_root']); runners.add(snapshot(run / p['runner_snapshot']))
            lock = nodes[node]['managed_version']
            installed = root / 'library/packages' / lock['package_id'] / lock['content_sha256']
            assert io_path(code / 'user_source.py').read_bytes() == io_path(run / p['frozen_package_root'] / 'user_source.py').read_bytes() == io_path(installed / 'user_source.py').read_bytes()
            source_hashes.setdefault(node, set()).add(sha(code / 'user_source.py'))
            assert set(p['inputs']) == set(expected_inputs[node]) and set(p['outputs']) == set(expected_outputs[node])
            for port, meta in p['inputs'].items():
                path = directory / Path(p['result_manifest']).parent / meta['file']
                assert sha(path) == meta['sha256']
                maximum = max(maximum, close(npy(path), expected_inputs[node][port]))
            for port, meta in p['outputs'].items():
                path = directory / meta['path']; assert sha(path) == meta['sha256']
                assert meta['dtype'] == 'float32' and meta['shape'] == [2, 513] and meta['sample_rate_hz'] == 48000
                assert meta['channel_ids'] == ['ch0', 'ch1'] and meta['valid_interval'] == [0, 513]
                maximum = max(maximum, close(npy(path), expected_outputs[node][port]))
        matched = [events for _, events in archives if events[0]['request_id'] == row['request_id']]
        assert len(matched) == 1 and {e['node_id'] for e in matched[0]} == {'shared', 'named'}
        for node, expected in (('shared', expected_shared), ('named', expected_join)):
            selected = [e for e in matched[0] if e['node_id'] == node]; lock = nodes[node]['managed_version']
            assert all(e['project_id'] == record['project_id'] and e['revision'] == 7 and e['session_epoch'] == record['session_epoch'] and e['request_id'] == row['request_id'] and e['type_id'] == nodes[node]['type_id'] and e['package_id'] == lock['package_id'] and e['content_sha256'] == lock['content_sha256'] for e in selected)
            assert all(e['source'] == 'stdout' and e['level'] == 'info' and e['discarded_bytes'] == e['dropped_text_bytes'] == 0 for e in selected)
            assert [e['sequence'] for e in selected] == sorted({e['sequence'] for e in selected})
            paths = {Path(e['raw_log_file']).resolve() for e in selected}; assert len(paths) == 1
            path = next(iter(paths)); assert path.is_relative_to(root / 'run-logs')
            raw = io_path(path).read_bytes()
            assert raw == expected.encode() == ''.join(e['text'] for e in selected).encode() and selected[-1]['total_bytes'] == len(raw)
        results.append(dict(port=row['port'], origin=row['origin'], repeat=row['repeat'], request=row['request_id'], run=row['run'], upstream_calls=5, upstream_frames=513))
    assert len(runners) == 1 and all(len(hashes) == 1 for hashes in source_hashes.values())
    report = dict(status='passed',root=str(root),runs=results,maximum_error=maximum,npy_boundaries=36,host_hashes=hosts,runner_sha256=next(iter(runners)),source_sha256={k:next(iter(v)) for k,v in source_hashes.items()},scope='named selected outputs/shared upstream only; no desktop/full PC-05 rejection matrix claim')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status='passed',runs=len(results),maximum_error=maximum,upstream_calls_per_run=5)))


if __name__ == '__main__':
    main()
