"""Independently inspect owned PC0505 failures and numerical recoveries."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32, io_path, read_wav, sha
from verify_python_snapshot import close, npy, snapshot

NAME='PC-05.b mixed output metadata failures identify the exact port preserve success and recover'
def read(path):return json.loads(io_path(path).read_text(encoding='utf-8'))
def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args()
    case=next(c for c in ET.parse(args.junit).iter('testcase') if c.get('name')==NAME)
    assert case.find('failure') is None and case.find('error') is None
    root=Path(next(s.split('=',1)[1] for s in case.findtext('system-out','').splitlines() if s.startswith('PC05_METADATA_ROOT='))).resolve()
    repo=Path(__file__).resolve().parents[1];assert root.parent==repo/'build/runs' and root.name.startswith('mixed-fault-')
    rows=read(root/'metadata-demo.json');assert len(rows)==3 and {r['mode'] for r in rows}=={'rate','channels','interval'}
    package=read(root/'plugin/manifest.json');manifest_hash=sha(root/'plugin/manifest.json')
    assert io_path(root/'metadata-source-before.py').read_bytes()==io_path(root/'plugin/stateful.py').read_bytes()
    failed=[]
    for row in rows:
        assert row['node']=='python' and row['port']=='main'
        assert row['code']==('python_output_interval_invalid' if row['mode']=='interval' else 'python_output_format_invalid')
        assert row['failed']=='metadata-failed-'+row['mode'] and not (root/row['failed']).exists()
        archive=Path(row['archive']).resolve();assert archive.name=='events.jsonl' and archive.parent.parent==root.parent/'run-logs'
        events=[json.loads(s) for s in archive.read_text(encoding='utf-8').splitlines()]
        assert all(e['request_id']==row['request'] and e['project_id']=='python-stateful-demo' and e['revision']==1 and e['session_epoch']==71 and e['node_id']=='python' and e['type_id']=='example.python_stateful' and e['package_id']==package['plugin_id'] and e['content_sha256']==manifest_hash and e['version']==package['plugin_version'] for e in events)
        out=[e for e in events if e['source']=='stdout'];diagnostic=[e for e in events if e['source']=='diagnostic']
        assert out and len(diagnostic)==1 and row['code'] in diagnostic[0]['text'] and diagnostic[0]['level']=='error'
        raw_paths={Path(e['raw_log_file']).resolve() for e in out};assert len(raw_paths)==1;raw=next(iter(raw_paths));assert raw.is_relative_to(archive.parent)
        expected=('pc05-metadata='+row['mode']+'\r\n').encode();assert io_path(raw).read_bytes()==expected==''.join(e['text'] for e in out).encode()
        assert out[-1]['total_bytes']==len(expected) and all(e['discarded_bytes']==e['dropped_text_bytes']==0 for e in out)
        failed.append(dict(mode=row['mode'],code=row['code'],request=row['request'],archive=str(archive),archive_sha256=sha(archive),stdout_sha256=sha(raw)))
    rate,source=read_wav(root/'input.wav');frames=len(source[0]);assert rate==48000 and len(source)==2 and frames==1025
    before=[[f32(x*2) for x in c] for c in source]
    main_output=[[f32(f32(x*.75)+f32(f32(n)*f32(.00001))) for n,x in enumerate(c)] for c in before]
    side=[[-x for x in c] for c in main_output];expected=[[f32(x*.5) for x in c] for c in main_output]
    maximum=0;successes=[];requests=set();output_hashes=set();code_hashes=set();runners=set()
    for name in ['metadata-baseline']+[r['recovered'] for r in rows]:
        run=root/name;record=read(run/'run.json');complete=read(run/'complete.json');assert record['request_id'] not in requests;requests.add(record['request_id'])
        assert record['project_id']=='python-stateful-demo' and record['project_revision']==1 and record['block_size']==127
        assert record['frames']==1025 and record['sample_rate_hz']==48000 and record['channels']==2 and record['valid_start_frame']==0 and record['valid_frame_count']==1025
        output_rate,output=read_wav(run/'output.wav');assert output_rate==rate;maximum=max(maximum,close(output,expected))
        assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav');output_hashes.add(record['output_sha256'])
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        assert len(record['task_artifacts'])==1;artifact=record['task_artifacts'][0];p=artifact['provenance'];directory=run/artifact['directory'];assert p['node_id']=='python' and p['request_id']==record['request_id'] and p['process_id']>0
        code=directory/p['code_snapshot'];files={path.relative_to(io_path(code)).as_posix():sha(path) for path in io_path(code).rglob('*') if path.suffix=='.py' or path.name=='manifest.json'}
        digest=hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest();assert files==p['code_files'] and digest==p['code_sha256']==code.name;code_hashes.add(digest)
        assert io_path(code/'stateful.py').read_bytes()==io_path(root/'metadata-source-before.py').read_bytes()==io_path(run/p['frozen_package_root']/'stateful.py').read_bytes()
        assert sha(code/'manifest.json')==sha(root/'plugin/manifest.json');snapshot(run/p['frozen_package_root']);runners.add(snapshot(run/p['runner_snapshot']))
        assert p['interpreter_sha256']==sha(Path(p['environment']['executable']))
        meta=p['inputs']['audio_in'];path=directory/Path(p['result_manifest']).parent/meta['file'];assert sha(path)==meta['sha256'];maximum=max(maximum,close(npy(path),before))
        assert set(p['outputs'])=={'main','side'}
        for port,values in (('main',main_output),('side',side)):
            meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256'];assert meta['shape']==[2,1025] and meta['dtype']=='float32' and meta['sample_rate_hz']==48000 and meta['channel_ids']==['ch0','ch1'] and meta['valid_interval']==[0,1025];maximum=max(maximum,close(npy(path),values))
        lifecycle=read(directory/'lifecycle.json');assert lifecycle=={'prepares':1,'calls':9,'frames':1025}
        successes.append(dict(run=name,request=record['request_id'],output_sha256=record['output_sha256']))
    assert len(output_hashes)==len(code_hashes)==len(runners)==1 and requests.isdisjoint({r['request'] for r in rows})
    report=dict(status='passed',root=str(root),failures=failed,successes=successes,maximum_error=maximum,npy_boundaries=12,source_sha256=sha(root/'metadata-source-before.py'),code_sha256=next(iter(code_hashes)),runner_sha256=next(iter(runners)),scope='actual mixed output metadata rejection/recovery only; no input mismatch/desktop claim')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',failures=3,successes=4,maximum_error=maximum)))
if __name__=='__main__':main()
