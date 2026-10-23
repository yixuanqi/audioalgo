"""Read real per-port EOS WAVs and observations using independent FIR formulas."""
import argparse,datetime as dt,json,math
from pathlib import Path
from verify_python_mixed import ROOT,read_wav,sha,f32
from verify_python_snapshot import snapshot
from verify_python_mixed import io_path
import numpy as np

def inspect_mixed(base,run,frame,port,host):
    rate,samples=read_wav(base/'input.wav');frames=len(samples[0]);tail=2 if port=='main' else 5;length=frames+tail
    assert rate==48000 and frames==frame-4
    r=json.loads((run/'run.json').read_text());a=r['tasks'][1]['alignment'];fir=[];main=[]
    for channel in samples:
        x=lambda n:channel[n] if 0<=n<frames else 0.0
        gain,echo=(1,.25) if port=='main' else (2,.125)
        fir.append([f32(f32(x(i)*gain)+f32(x(i-tail)*echo)) for i in range(length)])
        main.append([f32(f32(v*.75)+f32(f32(i)*f32(.00001))) for i,v in enumerate(fir[-1])])
    assert r['frames']==r['raw_frames']==r['aligned_frames']==r['valid_frame_count']==length and r['alignment_offset_frames']==0
    assert r['valid_start_frame']==0 and r['drain_padding_frames']==(3 if port=='main' else 0)
    assert (a['offset_frames'],a['raw_frames'],a['aligned_frames'],a['source_frames'],a['retained_tail_frames'])==(frame-1,length+frame-1,length,frames,tail)
    assert a['drain_padding_frames']==r['drain_padding_frames']
    artifact=a['artifacts'][0];assert artifact['port_id']==port and artifact['valid_frame_count']==length
    raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path'];assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
    assert [list(c) for c in read_wav(aligned)[1]]==fir
    for c,pcm in enumerate(read_wav(raw)[1]):assert len(pcm)==length+frame-1 and list(pcm[frame-1:])==fir[c]
    task=r['task_artifacts'][0];directory=run/task['directory'];p=task['provenance']
    snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
    assert p['process_id']>0 and p['request_id']==r['request_id']
    assert sha(Path(p['environment']['executable']))==p['interpreter_sha256']
    for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
    for name,sign in (('main',1),('side',-1)):
        meta=p['outputs'][name];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['valid_interval']==[0,length]
        actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(len(samples),length)
        assert np.array_equal(actual,np.array(main,dtype=np.float32)*sign)
    for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
    assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':math.ceil(length/host),'frames':length}
    expected=np.array(main,dtype=np.float32)*np.float32(.5);actual=np.array(read_wav(run/'output.wav')[1]);error=np.abs(actual-expected)
    assert actual.shape==expected.shape and np.all(error<=2e-6+1e-5*np.abs(expected))
    assert sha(run/'output.wav')==r['output_sha256']==json.loads((run/'complete.json').read_text())['output_sha256']
    assert sha(base/'input.wav')==r['input_sha256']
    return {'run':str(run),'process_id':p['process_id'],'output_sha256':sha(run/'output.wav'),'maximum_absolute_error':float(error.max())}

def mixed_checks(record,start,end):
    roots=[p for p in (ROOT/'build/runs').glob('mixed-stream-eos-*') if start<=p.stat().st_mtime<=end];assert len(roots)==1
    checks=[]
    for frame in (480,512):
        for port in ('main','side'):
            for host in (1,127,256,480,512,1024):
                base=roots[0]/f'{frame}-{port}-{host}';probe=json.loads((base/'probe.json').read_text());frames=frame-4
                assert probe['output_ends']==probe['produced']==[frames+2,frames+5] and probe['finished']==[1,1]
                assert probe['prepares']==1 and probe['algorithm_input_frames']==2*frame
                checks.append(inspect_mixed(base,base/'run',frame,port,host))
    recoveries=[p for p in (ROOT/'build/runs').glob('mixed-stream-recovery-*') if start<=p.stat().st_mtime<=end];assert len(recoveries)==1
    root=recoveries[0];observation=json.loads((root/'recovery-observation.json').read_text())
    assert observation['code']=='stream_no_progress' and observation['node_id']=='before'
    assert observation['port_id']=='side'
    assert observation['failed_finished']==[1,0] and 0<observation['failed_calls']<200
    assert observation['failed_directory_exists'] is False and not (root/'failed').exists()
    first=inspect_mixed(root,root/'first',480,'main',127);retry=inspect_mixed(root,root/'retry',480,'main',127)
    assert first['output_sha256']==retry['output_sha256'] and first['process_id']!=retry['process_id']
    checks.extend((first,retry));return checks

def main():
    p=argparse.ArgumentParser();p.add_argument('--command-record',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--kind',choices=('native','mixed'),default='native');args=p.parse_args()
    r=json.loads(args.command_record.read_text(encoding='utf-8'));assert r['exit_code']==0 and sha(Path(r['log_path']))==r['log_sha256']
    for artifact in r['artifacts']:assert artifact.get('sha256') and sha(Path(artifact['path']))==artifact['sha256']
    start=dt.datetime.fromisoformat(r['started_utc']).timestamp();end=dt.datetime.fromisoformat(r['finished_utc']).timestamp()
    if args.kind=='mixed':
        checks=mixed_checks(r,start,end)
        with args.output.open('x',encoding='utf-8') as f:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},f,indent=2)
        print(f'Verified {len(checks)} mixed EOS/recovery WAVs, native raw/aligned, main/side NPY, code and runner hashes');return
    roots=[x for x in (ROOT/'build/runs').glob('stream-port-eos-*') if start<=x.stat().st_mtime<=end];assert len(roots)==1
    checks=[]
    for frame in (480,512):
        for frames in (frame-4,1025):
            for port,tail,gain,echo in (('main',2,1,.25),('side',5,2,.125)):
                for host in (1,127,256,480,512,1024,0):
                    run=roots[0]/f'{frame}-{frames}-{port}-{host}';probe=json.loads((run/'probe.json').read_text())
                    assert (probe['frame'],probe['input_frames'],probe['selected'],probe['host'])==(frame,frames,port,host)
                    assert probe['output_ends']==probe['produced']==[frames+2,frames+5] and probe['finished']==[1,1]
                    assert probe['algorithm_input_frames']==math.ceil((frames+5)/frame)*frame
                    assert 0<probe['callback_calls']<=math.ceil((frames+5)/frame)*math.ceil(frame/7)
                    assert probe['drain_padding_frames']==(3 if port=='main' else 0)
                    pattern=[127,1]+[1+int(x%1024) for x in np.random.RandomState(20261023).randint(0,2**32,size=173,dtype=np.uint32)] if host==0 else []
                    assert probe['host_partitions']==pattern
                    length=frames+tail;offset=frame-1;assert probe['raw_frames']==length+offset
                    rate,input_pcm=read_wav(run/'input.wav');rate2,output=read_wav(run/'output.wav');rate3,raw=read_wav(run/'raw.wav')
                    assert rate==rate2==rate3==48000 and len(input_pcm)==len(output)==len(raw)==2
                    maximum=0
                    for source,actual,raw_channel in zip(input_pcm,output,raw):
                        assert len(source)==frames and len(actual)==length and len(raw_channel)==length+offset
                        x=lambda n:source[n] if 0<=n<frames else 0.0
                        expected=[f32(f32(x(i)*gain)+f32(x(i-tail)*echo)) for i in range(length)]
                        assert list(actual)==expected and list(raw_channel[offset:])==expected
                        assert all(v==0 for v in raw_channel[:offset])
                        maximum=max(maximum,max(abs(a-b) for a,b in zip(actual,expected)))
                    checks.append({'run':str(run),'output_sha256':sha(run/'output.wav'),'raw_sha256':sha(run/'raw.wav'),'maximum_absolute_error':maximum,'drain_padding_frames':probe['drain_padding_frames']})
    assert len(checks)==56
    with args.output.open('x',encoding='utf-8') as f:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},f,indent=2)
    print('Verified 56 selected-port outputs across six hosts and seeded partitions; maximum error 0')
if __name__=='__main__':main()
