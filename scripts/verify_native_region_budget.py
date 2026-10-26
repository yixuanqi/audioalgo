"""Independently read near-limit separate native regions around actual frozen Python."""
import argparse,hashlib,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,io_path,read_wav,sha
from verify_python_snapshot import close,npy,snapshot
from verify_native_log_budget import inspect_archive

NAME='PC-06.c independent native regions retain separate sixteen MiB budgets through actual mixed Run and recovery'
LIMIT=16*1024*1024
def load(p):return json.loads(io_path(p).read_text(encoding='utf-8'))
def main():
    p=argparse.ArgumentParser();p.add_argument('junit',type=Path);p.add_argument('--report',type=Path,required=True);args=p.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0608-region-budget'
    case=next(c for c in ET.parse(args.junit).iter('testcase')if c.get('name')==NAME);assert not case.findall('failure')and not case.findall('error')and not case.findall('skipped')
    root=Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0608_REGION_ROOT='))).resolve();assert root.parent==folder/'artifacts';dll=load(root/'dll-identity.json');assert sha(Path(dll['dll']))==dll['sha256']
    original=load(repo/'build/reports/loops/PC0606-native-budget/independent-data.json');assert dll['sha256']==original['dll_sha256']
    rows=load(root/'region-demo.json');assert [r['name']for r in rows]==['exact','overflow-C','recovery']and len({r['request']for r in rows})==3
    rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)];a=[[f32(2*x)for x in c]for c in source];b=[[f32(.25*x)for x in c]for c in a]
    maximum=0;results=[];hashes=set();code_hashes=set();runners=set()
    for n,row in enumerate(rows):
        name=row['name'];succeeds=n!=1;assert row['success']==succeeds and row['revision']==14+n and row['code']==(''if succeeds else'log_limit')and row['node']==(''if succeeds else'native-C')
        project=load(root/(name+'.aaproj'));nodes={v['id']:v for v in project['nodes']};assert project['project_id']=='pc0608-region-budget'and project['revision']==row['revision'];assert nodes['native-A']['parameters']['log_bytes']==LIMIT and nodes['native-C']['parameters']['log_bytes']==LIMIT+(0 if succeeds else 1)
        path=Path(row['archive']).resolve();assert path.parent.parent==root/'run-logs';events=inspect_archive(path,dll['sha256'],project_id=project['project_id'],revision=row['revision'],native_nodes=('native-A','native-C'))
        assert events['identity'][1]==row['request']and events['identity'][3]==31 and events['raw_bytes']==2*LIMIT and {node:v['bytes']for node,v in events['streams'].items()}=={'native-A':LIMIT,'native-C':LIMIT}
        assert all(v['events']==16384 for v in events['streams'].values())
        run=(root/row['run']).resolve();assert run.is_relative_to(root/'runs')
        if not succeeds:
            assert not run.exists()and len(events['diagnostics'])==1 and events['diagnostics'][0]['node_id']=='native-C'and events['diagnostics'][0]['text'].startswith('log_limit:')
            results.append(dict(name=name,success=False,archive=str(path),native_raw_bytes=2*LIMIT,failed_node='native-C'));continue
        assert not events['diagnostics'];record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');assert out_rate==48000;maximum=max(maximum,close(output,source));assert[list(c)for c in output]==[list(c)for c in source]
        assert record['frames']==513 and record['channels']==2 and record['block_size']==1 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['project_revision']==row['revision']and record['request_id']==row['request']and record['session_epoch']==31
        assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav');hashes.add(record['output_sha256']);assert {v['node_id']for v in record['plugins']}=={'native-A','native-C'}and all(v['library_sha256']==dll['sha256']for v in record['plugins'])
        assert[v['kind']for v in record['tasks']]==['source','native_region','offline','native_region','sink']
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        assert len(record['task_artifacts'])==1;artifact=record['task_artifacts'][0];provenance=artifact['provenance'];directory=run/artifact['directory'];assert provenance['node_id']=='python'and provenance['request_id']==row['request']and provenance['process_id']>0
        code=directory/provenance['code_snapshot'];files={path.relative_to(io_path(code)).as_posix():sha(path)for path in io_path(code).rglob('*')if path.suffix=='.py'or path.name=='manifest.json'};digest=hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest();assert files==provenance['code_files']and digest==provenance['code_sha256']==code.name;code_hashes.add(digest)
        snapshot(run/provenance['frozen_package_root']);runners.add(snapshot(run/provenance['runner_snapshot']));lock=nodes['python']['managed_version'];installed=root/'library/packages'/lock['package_id']/lock['content_sha256'];actual=io_path(code/'user_source.py').read_bytes();assert actual==io_path(run/provenance['frozen_package_root']/'user_source.py').read_bytes()==io_path(installed/'user_source.py').read_bytes()and b'np.float32(.25)'in actual
        assert sha(Path(provenance['environment']['executable']))==provenance['interpreter_sha256'];meta=provenance['inputs']['audio_in'];input_path=directory/Path(provenance['result_manifest']).parent/meta['file'];assert sha(input_path)==meta['sha256'];maximum=max(maximum,close(npy(input_path),a));meta=provenance['outputs']['audio_out'];out_path=directory/meta['path'];assert sha(out_path)==meta['sha256']and meta['shape']==[2,513]and meta['channel_ids']==['ch0','ch1']and meta['sample_rate_hz']==48000 and meta['valid_interval']==[0,513];maximum=max(maximum,close(npy(out_path),b))
        results.append(dict(name=name,success=True,run=str(run),output_sha256=record['output_sha256'],native_raw_bytes=2*LIMIT,code_sha256=digest))
    assert len(hashes)==len(code_hashes)==len(runners)==1 and maximum==0 and len(list((root/'runs').iterdir()))==2
    report=dict(status='passed',root=str(root),actual_successful_wavs=2,npy_boundaries=4,native_raw_streams=6,raw_bytes_per_region=LIMIT,raw_bytes_per_run=2*LIMIT,maximum_error=maximum,code_sha256=next(iter(code_hashes)),runner_sha256=next(iter(runners)),dll_sha256=dll['sha256'],runs=results,scope='actual two native regions each16MiB around frozen Python,second-region+1 fails C,old success preserved,exact-budget recovery;not parallel requests/native UI flood/per-stream tail/lifecycle/desktop')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',successful_wavs=2,npy_boundaries=4,native_raw_streams=6,maximum_error=maximum)))
if __name__=='__main__':main()
