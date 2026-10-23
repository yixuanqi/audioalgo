"""Independent RIFF/NumPy formulas for deployed extension evidence; never import the plugin."""
import argparse
import datetime as dt
import json
import math
import shutil
import subprocess
from pathlib import Path
import numpy as np
from verify_python_mixed import ROOT,read_wav,sha,io_path,f32
from verify_python_snapshot import snapshot

def load(p):return json.loads(io_path(p).read_text(encoding='utf-8'))
def verify(run,version=1):
    r=load(run/'run.json');complete=load(run/'complete.json');project=load(run/'project.aaproj')
    task=r['task_artifacts'][0];directory=run/task['directory'];p=task['provenance']
    snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
    defaults={'enabled':True,'repeat':2,'scale':.25,'polarity':'negative','tag':'user v1','coefficient':'coefficient.txt'}
    defaults.update(next(n['parameters'] for n in project['nodes'] if n['id']=='python'))
    resources={x['parameter_id']:x for x in r['resources']}
    for x in r['resources']:assert sha(run/x['snapshot_path'])==x['sha256']==x['snapshot_sha256']
    coefficient=float((run/resources['coefficient']['snapshot_path']).read_text())
    gain=f32((defaults['repeat']*defaults['scale']*(1 if defaults['polarity']=='positive' else -1)*coefficient if defaults['enabled'] else 1)*version)
    rate,samples=read_wav(run/resources['path']['snapshot_path']);rate_out,out=read_wav(run/'output.wav');assert rate==rate_out==48000
    assert len(out)==2 and all(len(c)==1025 for c in out)
    expected=np.array([[f32(x*gain) for x in c] for c in samples],dtype=np.float32)
    errors=[]
    for port,sign in [('main',1),('side',-1)]:
        meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256']
        actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(2,1025)
        assert meta['channel_ids']==['ch0','ch1'] and meta['valid_interval']==[0,1025]
        difference=np.abs(actual.astype(np.float64)-expected.astype(np.float64)*sign)
        assert np.all(difference<=2e-6+1e-5*np.abs(expected));errors.append(float(difference.max()))
    result_root=(directory/p['result_manifest']).parent
    for meta in p['inputs'].values():assert sha(result_root/meta['file'])==meta['sha256']
    for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
    code=(directory/p['code_snapshot']/'user_transform.py').read_text();assert f'VERSION_GAIN={version}.0' in code
    log=load(directory/'extension.json');assert log['calls']==math.ceil(1025/r['block_size']) and log['tag']==defaults['tag'] and log['gain']==gain
    assert Path(log['coefficient']).resolve()==(run/resources['coefficient']['snapshot_path']).resolve()
    mixed=np.array([[f32(float(x)+f32(-float(x)*.5)) for x in c] for c in expected],dtype=np.float64)
    difference=np.abs(np.array(out)-mixed);assert np.all(difference<=2e-6+1e-5*np.abs(mixed));errors.append(float(difference.max()))
    assert sha(run/'output.wav')==r['output_sha256']==complete['output_sha256']
    assert r['raw_frames']==r['aligned_frames']==r['frames']==1025 and r['alignment_offset_frames']==0
    assert p['request_id']==r['request_id'] and p['node_id']=='python'
    return {'run':str(run),'run_id':r['run_id'],'request_id':r['request_id'],'process_id':p['process_id'],'code_sha256':p['code_sha256'],'output_sha256':r['output_sha256'],'maximum_absolute_error':max(errors)}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--command-record',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--cli',type=Path,required=True);parser.add_argument('--host-build-record',type=Path);args=parser.parse_args()
    record=load(args.command_record);assert record['exit_code']==0
    start=dt.datetime.fromisoformat(record['started_utc']).timestamp();end=dt.datetime.fromisoformat(record['finished_utc']).timestamp()
    roots=[p for p in (ROOT/'build/runs').glob('extension-*') if start<=p.stat().st_mtime<=end];assert len(roots)==2
    args.cli=args.cli.resolve();editor=ROOT/'build/windows-debug/bin/AudioAlgo Editor.exe'
    checks=[];commands=[];host_before={str(p):sha(p) for p in (args.cli,editor)}
    for root in roots:
        for run in sorted(root.glob('component-*'))+sorted(root.glob('cli-*')):checks.append(verify(run))
        if (root/'frozen-v1').exists():
            old=verify(root/'frozen-v1');new=verify(root/'updated-v2',2);checks.extend([old,new,verify(root/'recovered-v2',2)])
            assert old['process_id']!=new['process_id'] and old['code_sha256']!=new['code_sha256'] and not (root/'bad-resource').exists()
    assert len(checks)==19
    # Exercise updates/errors in fresh CLI processes against the same fixed host.
    root=args.output.parent/(args.output.stem+'-cli-lifecycle');root.mkdir(exist_ok=False)
    shutil.copytree(ROOT/'examples/plugins/python_user_transform',root/'plugin');shutil.copy(ROOT/'examples/python_stateful_demo/input.wav',root/'input.wav')
    shutil.copy(ROOT/'examples/python_extension_demo/coefficient.txt',root/'coefficient.txt')
    project=load(ROOT/'examples/python_extension_demo/extension.aaproj');project['nodes'][0]['parameters']['path']='input.wav'
    (root/'extension.aaproj').write_text(json.dumps(project),encoding='utf-8')
    for name,version in [('v1',1),('bad',1),('v2',2)]:
        (root/'coefficient.txt').write_text('invalid coefficient' if name=='bad' else '0.5')
        if version==2:
            path=root/'plugin/user_transform.py';path.write_text(path.read_text().replace('VERSION_GAIN=1.0','VERSION_GAIN=2.0'))
        command=[str(args.cli.resolve()),'--plugin-dir',str((root/'plugin').resolve()),'run',str((root/'extension.aaproj').resolve()),str((root/name).resolve()),'127']
        result=subprocess.run(command,capture_output=True,timeout=20);log=root/(name+'.log');log.write_bytes(result.stdout+result.stderr);commands.append({'command':command,'exit_code':result.returncode,'log_sha256':sha(log)})
        if name=='bad':
            assert result.returncode!=0 and not (root/name).exists();assert b'coefficient_invalid' in log.read_bytes() and b'node=python' in log.read_bytes() and b'port=coefficient' in log.read_bytes()
        else:assert result.returncode==0;checks.append(verify(root/name,version))
    assert checks[-1]['process_id']!=checks[-2]['process_id'] and checks[-1]['code_sha256']!=checks[-2]['code_sha256']
    assert host_before=={str(p):sha(p) for p in (args.cli,editor)}
    builds=load(args.host_build_record or args.command_record.parent/'extension-build-ready.json');assert builds['exit_code']==0
    for artifact in builds['artifacts']:
        if artifact['path'] in host_before:assert artifact['sha256']==host_before[artifact['path']]
    with args.output.open('x',encoding='utf-8') as stream:json.dump({'scope':'P3-06 automated component/CLI subset','command_record_sha256':sha(args.command_record),'checks':checks,'cli_commands':commands,'unchanged_host_hashes':host_before,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},stream,indent=2)
    print(f'Independent extension oracle verified {len(checks)} WAVs and all main/side NPYs; max error {max(c["maximum_absolute_error"] for c in checks)}')
if __name__=='__main__':main()
