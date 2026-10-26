"""Independent PC0603 native/Python mixed WAV, closures, raw logs and allocation audit."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,io_path,read_wav,sha
from verify_python_snapshot import close,npy,snapshot

LIVE='PC-06.b native SDK logs reach production mixed Run before terminal with exact instance identity'
ALLOC='PC-06.c bound native SDK callback and reset allocate nothing while only the worker consumes logs'
def read(path):return json.loads(io_path(path).read_text(encoding='utf-8'))
def case_root(junit,name,marker):
    case=next(c for c in ET.parse(junit).iter('testcase')if c.get('name')==name);assert case.find('failure')is None and case.find('error')is None
    return Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith(marker+'='))).resolve()
def archives(root,request):
    matched=[]
    for path in (root/'run-logs').glob('*/events.jsonl'):
        rows=[json.loads(line)for line in path.read_text(encoding='utf-8').splitlines()]
        if rows and rows[0]['request_id']==request:matched.append((path,rows))
    assert len(matched)==1
    return matched[0]
def native_stream(events,node,request,dll_hash,project,revision,epoch,expected_calls=5):
    rows=[e for e in events if e['node_id']==node];assert rows
    assert all(e['source']=='native' and e['level']=='info' and e['type_id']=='example.sdk_logging' and e['version']=='1.0.0' and e['package_id']=='example.sdk_logging.good' and e['content_sha256']==dll_hash and e['request_id']==request and e['project_id']==project and e['revision']==revision and e['session_epoch']==epoch for e in rows)
    assert [e['sequence']for e in rows]==list(range(1,len(rows)+1))
    assert [e['text']for e in rows]==['sdk-log reset','sdk-log prepare']+['sdk-log process']*expected_calls+['sdk-log reset']
    paths={Path(e['raw_log_file']).resolve()for e in rows};assert len(paths)==1;path=next(iter(paths));assert path.name.startswith('native-log-')
    expected=b''.join(e['text'].encode()for e in rows);assert io_path(path).read_bytes()==expected
    running=0
    for event in rows:
        running+=len(event['text'].encode());assert event['total_bytes']==running and event['discarded_bytes']==event['dropped_text_bytes']==0
    return dict(node=node,path=str(path),sha256=sha(path),bytes=running,events=len(rows),process_calls=expected_calls)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--off-junit',type=Path,required=True);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0603-native-live'
    root=case_root(args.junit,LIVE,'PC06_NATIVE_ROOT');assert root.parent==folder/'artifacts'
    rows=read(root/'native-demo.json');assert len(rows)==3 and {(r['origin'],r['repeat'])for r in rows}=={('gui',0),('gui',1),('cli',0)} and len({r['request']for r in rows})==3
    dll=read(root/'native-dll.json');package=Path(dll['package']);dll_hash=sha(package/'native_logging.dll');assert dll_hash==dll['sha256'];manifest=read(package/'manifest.json')
    command=read(args.junit.with_suffix('.json'))
    for filename,key in (('audioalgo_tests.exe','test_host_sha256'),('audio_cli.exe','cli_sha256')):assert next(a['sha256']for a in command['artifacts']if Path(a['path']).name==filename)==dll[key]
    rate,source=read_wav(root/'input.wav');assert rate==48000 and len(source)==2 and all(len(c)==513 for c in source);close(source,[[f32(.2*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)])
    a=[[f32(x*2)for x in c]for c in source];b=[[f32(f32(x*.25)+f32(1e-5*n))for n,x in enumerate(c)]for c in a];expected=[[f32(x*2)for x in c]for c in b]
    project=read(root/'project.aaproj');nodes={n['id']:n for n in project['nodes']};assert nodes['python']['parameters']==dict(gain=.25,step=.00001,hold=True)
    code_hashes=set();runner_hashes=set();maximum=0;results=[];native_files=set()
    for row in rows:
        run=(root/row['run']).resolve();assert run.is_relative_to(root);record=read(run/'run.json');complete=read(run/'complete.json');assert record['request_id']==row['request'] and record['project_id']==project['project_id']=='pc06-sdk-mixed' and record['project_revision']==7
        assert record['block_size']==127 and record['frames']==513 and record['channels']==2 and record['sample_rate_hz']==48000 and record['valid_start_frame']==0 and record['valid_frame_count']==513
        output_rate,output=read_wav(run/'output.wav');assert output_rate==48000;maximum=max(maximum,close(output,expected));assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
        assert {p['node_id']for p in record['plugins']}=={'native-A','native-C'} and all(p['library_sha256']==dll_hash and p['manifest_sha256']==sha(package/'manifest.json')for p in record['plugins'])
        assert [t['kind']for t in record['tasks']]==['source','native_region','offline','native_region','sink']
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        assert len(record['task_artifacts'])==1;artifact=record['task_artifacts'][0];p=artifact['provenance'];directory=run/artifact['directory'];assert p['node_id']=='python' and p['request_id']==row['request'] and p['process_id']>0
        code=directory/p['code_snapshot'];files={path.relative_to(io_path(code)).as_posix():sha(path)for path in io_path(code).rglob('*')if path.suffix=='.py' or path.name=='manifest.json'};digest=hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest();assert files==p['code_files'] and digest==p['code_sha256']==code.name;code_hashes.add(digest)
        snapshot(run/p['frozen_package_root']);runner_hashes.add(snapshot(run/p['runner_snapshot']));lock=nodes['python']['managed_version'];installed=root/'library/packages'/lock['package_id']/lock['content_sha256'];assert io_path(code/'user_source.py').read_bytes()==io_path(run/p['frozen_package_root']/'user_source.py').read_bytes()==io_path(installed/'user_source.py').read_bytes();assert b'dtype=np.float64' in io_path(code/'user_source.py').read_bytes()
        assert p['interpreter_sha256']==sha(Path(p['environment']['executable']));meta=p['inputs']['audio_in'];path=directory/Path(p['result_manifest']).parent/meta['file'];assert sha(path)==meta['sha256'];maximum=max(maximum,close(npy(path),a));meta=p['outputs']['audio_out'];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['shape']==[2,513] and meta['sample_rate_hz']==48000 and meta['channel_ids']==['ch0','ch1'] and meta['valid_interval']==[0,513];maximum=max(maximum,close(npy(path),b))
        archive,events=archives(root,row['request']);assert {e['node_id']for e in events}=={'native-A','python','native-C'}
        native=[native_stream(events,node,row['request'],dll_hash,record['project_id'],7,record['session_epoch'])for node in ('native-A','native-C')]
        for stream in native:assert Path(stream['path']).is_relative_to(archive.parent);native_files.add(stream['path'])
        python=[e for e in events if e['node_id']=='python'];assert len(python)==1;event=python[0];assert event['text']=='pc06-python-start\r\n' and event['source']=='stdout' and event['type_id']=='user.pc06_transform' and event['package_id']==lock['package_id'] and event['content_sha256']==lock['content_sha256'];assert io_path(Path(event['raw_log_file'])).read_bytes()==event['text'].encode()
        results.append(dict(origin=row['origin'],repeat=row['repeat'],request=row['request'],run=row['run'],output_sha256=record['output_sha256'],native_streams=native))
    assert len(native_files)==6 and len(code_hashes)==len(runner_hashes)==1 and len({r['output_sha256']for r in results})==1
    live=(root/'live-native-log.txt').read_text(encoding='utf-8');assert 'node=native-A' in live and 'sdk-log prepare' in live and 'pc06-python-start' in live and 'node=native-C' not in live
    for n in range(2):
        text=(root/f'ui-log-{n}.txt').read_text(encoding='utf-8');request=next(r['request']for r in rows if r['origin']=='gui'and r['repeat']==n);assert request in text and 'node=native-A'in text and 'node=native-C'in text and dll_hash in text and 'sdk-log reset'in text
    allocation=[]
    for junit in (args.junit,args.off_junit):
        allocation_root=case_root(junit,ALLOC,'PC06_ALLOC_ROOT');assert allocation_root.parent==folder/'allocations';data=read(allocation_root/'allocation-probe.json');assert data['allocations']==0 and data['events']==12 and data['callbacks_worker_only']is True
        assert sha(Path(data['dll']))==dll_hash;raw=Path(data['raw_file']).resolve();assert raw.is_relative_to(allocation_root/'logs');assert io_path(raw).read_bytes()==b'sdk-log prepare'+b'sdk-log process'*10+b'sdk-log reset';allocation.append(dict(root=str(allocation_root),raw_sha256=sha(raw),bytes=len(io_path(raw).read_bytes()),allocations=0,events=12))
    sdk_cases=[];old_identity=read(folder.parent/'PC0601-native-log-queue/baseline-identity.json');old_dll=Path(old_identity['artifacts'][1]['path']);assert sha(old_dll)==old_identity['artifacts'][1]['sha256']
    for phase,prefix in (('live-green','sdk-cli-on'),('live-off','sdk-cli-off')):
        parent=folder.parent/'PC0602-log-negotiation';data=read(parent/(phase+'-data.json'));assert data['status']=='passed' and data['failures']==0 and len(data['cases'])==10
        receipt=read(folder/(prefix+'.json'));assert receipt['exit_code']==0 and data['host']['before_sha256']==data['host']['after_sha256']==next(a['sha256']for a in receipt['artifacts']if Path(a['path']).name=='audio_cli.exe')
        for row in data['cases']:
            if not row['test_name'].startswith('PC-06.a/b actual'):continue
            run=Path(row['run']);record=read(run/'run.json');input_file=Path(data['external'])/'audio/input.wav';rate,signal=read_wav(input_file);output_rate,actual=read_wav(run/'output.wav');assert rate==output_rate==48000
            expect=[[f32(1.5*x+x+.00001*n)if row['mode']=='legacy'else f32(2*x)for n,x in enumerate(c)]for c in signal];maximum=max(maximum,close(actual,expect));assert row['output_sha256']==record['output_sha256']==sha(run/'output.wav')
            native=[]
            if row['mode']=='good':
                path,events=archives(parent,record['request_id']);native=[native_stream(events,'native',record['request_id'],dll_hash,record['project_id'],1,record['session_epoch'])];assert Path(native[0]['path']).is_relative_to(path.parent)
            elif row['mode']=='legacy':assert record['plugins'][0]['library_sha256']==sha(old_dll)
            sdk_cases.append(dict(phase=phase,mode=row['mode'],run=str(run),output_sha256=record['output_sha256'],native_streams=native))
    legacy_environment=read(folder/'legacy-component-environment.json');legacy_root=Path(legacy_environment['output']);assert Path(legacy_environment['package'])==old_dll.parent and legacy_environment['dll_sha256']==sha(old_dll)
    tree=ET.parse(folder/'legacy-component.xml');assert len(tree.findall('.//testcase'))==2 and not tree.findall('.//failure') and not tree.findall('.//skipped')
    rate,primary=read_wav(legacy_root/'a.wav');other_rate,reference=read_wav(legacy_root/'b.wav');assert rate==other_rate==48000;first=[[f32(1.5*x+y+.00001*n)for n,(x,y)in enumerate(zip(a,b))]for a,b in zip(primary,reference)]
    legacy_outputs=[]
    for port in ('main','side'):
        run=legacy_root/('component-'+port);rate,actual=read_wav(run/'output.wav');assert rate==48000;expect=[[f32(.5*x+y-.00002*n)if port=='main'else f32(x-y)for n,(x,y)in enumerate(zip(a,b))]for a,b in zip(first,reference)];maximum=max(maximum,close(actual,expect));record=read(run/'run.json');assert len(record['plugins'])==2 and all(p['library_sha256']==sha(old_dll)for p in record['plugins']);legacy_outputs.append(dict(port=port,run=str(run),output_sha256=sha(run/'output.wav')))
    report=dict(status='passed',root=str(root),runs=results,native_raw_streams=6,native_process_messages=30,npy_boundaries=6,maximum_error=maximum,dll_sha256=dll_hash,source_code_sha256=next(iter(code_hashes)),runner_sha256=next(iter(runner_hashes)),allocation=allocation,sdk_cli=sdk_cases,legacy_component=legacy_outputs,independent_wavs=11,scope='basic real native log/live/reset/instance identity and successful mixed numerical flow; no full native flood/budget/NUL/close lifecycle/UI capability hint/desktop claim')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',mixed_runs=3,native_raw_streams=6,native_process_messages=30,maximum_error=maximum,callback_allocations=0)))
if __name__=='__main__':main()
