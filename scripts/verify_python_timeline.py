"""Inspect actual native/Python materializations with independent float32 formulas."""
import argparse
import datetime as dt
import json
from pathlib import Path
import math
import numpy as np
from verify_python_mixed import ROOT,read_wav,sha,f32,verify,io_path
from verify_python_snapshot import snapshot

def speex_checks(root):
    rate,original=read_wav(root/'original.wav');frames=len(original[0]);checks=[];boundaries=[]
    assert rate==16000 and len(original)==2 and frames==32037
    expected=np.array([[f32(f32(x*.75)+f32(f32(i)*f32(.00001))) for i,x in enumerate(c)] for c in original])
    for kind in ('component','cli'):
        for host in (1,127,256,480,512,1024):
            run=root/f'{kind}-{host}';r=json.loads((run/'run.json').read_text(encoding='utf-8'));a=r['tasks'][1]['alignment']
            assert a['offset_frames']==959 and a['raw_frames']==frames+959 and a['aligned_frames']==frames
            assert r['frames']==r['raw_frames']==r['aligned_frames']==frames and r['alignment_offset_frames']==0
            assert r['valid_start_frame']==0 and r['valid_frame_count']==frames
            for artifact in a['artifacts']:
                raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path'];assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
                raw_pcm=read_wav(raw)[1];aligned_pcm=read_wav(aligned)[1]
                for c in range(2):
                    assert len(raw_pcm[c])==frames+959 and len(aligned_pcm[c])==frames
                    assert raw_pcm[c][959:]==aligned_pcm[c]==original[c]
                boundaries.append({'run':str(run),'raw_sha256':sha(raw),'aligned_sha256':sha(aligned),'offset_frames':959})
            rate2,pcm=read_wav(run/'output.wav');assert rate2==rate
            error=np.abs(np.array(pcm)-expected);assert np.all(error<=2e-6+1e-5*np.abs(expected))
            assert sha(run/'output.wav')==r['output_sha256']==json.loads((run/'complete.json').read_text())['output_sha256']
            task=r['task_artifacts'][0];directory=run/task['directory'];p=task['provenance']
            snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
            for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
            for port,sign in [('main',1),('side',-1)]:
                meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['valid_interval']==[0,frames]
                actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(2,frames)
                delta=np.abs(actual-expected*sign);assert np.all(delta<=2e-6+1e-5*np.abs(expected))
            for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
            assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':math.ceil(frames/host),'frames':frames}
            assert p['request_id']==r['request_id']
            checks.append({'run':str(run),'output_sha256':sha(run/'output.wav'),'maximum_absolute_error':float(error.max())})
    return checks,boundaries

def tail_checks(root):
    rate,samples=read_wav(root/'input.wav');assert rate==48000 and len(samples)==2 and len(samples[0])==1025
    fir=[];main=[]
    for c in samples:
        x=lambda n:c[n] if 0<=n<1025 else 0.0
        fir.append([f32(f32(x(i)+f32(x(i-1)*.5))+f32(x(i-2)*.25)) for i in range(1027)])
        main.append([f32(f32(v*.75)+f32(f32(i)*f32(.00001))) for i,v in enumerate(fir[-1])])
    checks=[];boundaries=[]
    for host in (1,127,256,480,512,1024):
        run=root/str(host);r=json.loads((run/'run.json').read_text(encoding='utf-8'));a=r['tasks'][1]['alignment']
        assert a['source_frames']==1025 and a['retained_tail_frames']==2 and a['raw_frames']==1506 and a['aligned_frames']==1027 and a['offset_frames']==479
        assert r['source_frames']==1025 and r['frames']==r['valid_frame_count']==1027 and r['retained_tail_frames']==2
        artifact=a['artifacts'][0];raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path']
        assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
        raw_pcm=read_wav(raw)[1];aligned_pcm=read_wav(aligned)[1]
        for c in range(2):assert list(raw_pcm[c][479:])==list(aligned_pcm[c])==fir[c]
        assert artifact['raw_valid_start_frame']==479 and artifact['valid_start_frame']==0 and artifact['valid_frame_count']==1027
        task=r['task_artifacts'][0];directory=run/task['directory'];p=task['provenance']
        snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
        for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
        for port,sign in [('main',1),('side',-1)]:
            meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['valid_interval']==[0,1027]
            actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(2,1027)
            assert np.array_equal(actual,np.array(main,dtype=np.float32)*sign)
        for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
        expected=np.array([[f32(v*.5) for v in c] for c in main]);actual=np.array(read_wav(run/'output.wav')[1]);error=np.abs(actual-expected)
        assert actual.shape==(2,1027) and np.all(error<=2e-6+1e-5*np.abs(expected))
        assert sha(run/'output.wav')==r['output_sha256']==json.loads((run/'complete.json').read_text())['output_sha256']
        assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':math.ceil(1027/host),'frames':1027}
        checks.append({'run':str(run),'maximum_absolute_error':float(error.max()),'output_sha256':sha(run/'output.wav')});boundaries.append({'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
    return checks,boundaries

def interval_checks(root):
    inputs=((7,1018),(7,1000),(7,0),(1025,0))
    expected_intervals=((7,1020),(7,1000),(7,0),(1025,0))
    checks=[];boundaries=[]
    for profile,interval in enumerate(expected_intervals):
        for selected in ('main','side'):
            run=root/f'{profile}-{selected}';r=json.loads((run/'boundary-report.json').read_text())
            assert r['profile']==profile and r['selected_port']==selected
            rate,samples=read_wav(run/'input.wav');assert rate==48000 and len(samples)==2 and len(samples[0])==1025
            fir=[];main=[]
            for channel in samples:
                x=lambda n:channel[n] if 0<=n<1025 else 0.0
                fir.append([f32(f32(x(i)+f32(x(i-1)*.5))+f32(x(i-2)*.25)) for i in range(1027)])
                main.append([f32(f32(v*.75)+f32(f32(i)*f32(.00001))) for i,v in enumerate(fir[-1])])
            assert len(r['tasks'])==5
            for task in r['tasks']:
                for port in task['outputs']:
                    expected=inputs[profile] if port['node_id']=='file' else interval
                    assert (port['valid_start_frame'],port['valid_frame_count'])==expected
                    assert port['frames']==(1025 if port['node_id']=='file' else 1027)
            a=r['tasks'][1]['alignment'];assert (a['offset_frames'],a['raw_frames'],a['aligned_frames'])==(479,1506,1027)
            assert a['source_frames']==1025 and a['retained_tail_frames']==2 and len(a['artifacts'])==1
            artifact=a['artifacts'][0];raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path']
            assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
            assert (artifact['raw_valid_start_frame'],artifact['valid_start_frame'],artifact['valid_frame_count'])==(479+interval[0],*interval)
            assert [list(c) for c in read_wav(aligned)[1]]==fir
            for c,pcm in enumerate(read_wav(raw)[1]):assert len(pcm)==1506 and list(pcm[479:])==fir[c]
            task=r['task_artifacts'][0];directory=Path(task['directory']);p=task['provenance']
            snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
            assert p['process_id']>0 and sha(Path(p['environment']['executable']))==p['interpreter_sha256']
            for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
            for port,sign in (('main',1),('side',-1)):
                meta=p['outputs'][port];path=directory/meta['path']
                assert sha(path)==meta['sha256'] and tuple(meta['valid_interval'])==interval
                actual=np.load(io_path(path),allow_pickle=False)
                assert actual.dtype==np.float32 and actual.shape==(2,1027)
                assert np.array_equal(actual,np.array(main,dtype=np.float32)*sign)
            for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
            assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':9,'frames':1027}
            expected=np.array(main,dtype=np.float32)*np.float32(.5 if selected=='main' else -.5)
            actual=np.array(read_wav(run/'output.wav')[1]);error=np.abs(actual-expected)
            assert actual.shape==(2,1027) and np.all(error<=2e-6+1e-5*np.abs(expected))
            checks.append({'run':str(run),'valid_interval':interval,'output_sha256':sha(run/'output.wav'),'maximum_absolute_error':float(error.max())})
            boundaries.append({'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
    return checks,boundaries

def port_tail_checks(root):
    rate,samples=read_wav(root/'input.wav');assert rate==48000 and len(samples)==2 and len(samples[0])==1025
    checks=[];boundaries=[]
    for selected,tail,gain,echo in (('audio_out',2,1,.25),('side',5,2,.125)):
        frames=1025+tail;fir=[];main=[]
        for channel in samples:
            x=lambda n:channel[n] if 0<=n<1025 else 0.0
            fir.append([f32(f32(x(i)*gain)+f32(x(i-tail)*echo)) for i in range(frames)])
            main.append([f32(f32(v*.75)+f32(f32(i)*f32(.00001))) for i,v in enumerate(fir[-1])])
        for host in (1,127,256,480,512,1024):
            run=root/f'{selected}-{host}';r=json.loads((run/'run.json').read_text());a=r['tasks'][1]['alignment']
            assert (a['raw_frames'],a['aligned_frames'],a['offset_frames'],a['retained_tail_frames'])==(frames+479,frames,479,tail)
            assert r['source_frames']==1025 and r['frames']==r['valid_frame_count']==frames and r['retained_tail_frames']==tail
            artifact=a['artifacts'][0];assert artifact['node_id']=='before' and artifact['port_id']==selected
            assert artifact['raw_frames']==frames+479 and artifact['aligned_frames']==frames and artifact['valid_frame_count']==frames
            raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path']
            assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
            assert [list(c) for c in read_wav(aligned)[1]]==fir
            for c,pcm in enumerate(read_wav(raw)[1]):assert len(pcm)==frames+479 and list(pcm[479:])==fir[c]
            task=r['task_artifacts'][0];directory=run/task['directory'];p=task['provenance']
            snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
            assert p['process_id']>0 and p['request_id']==r['request_id']
            assert sha(Path(p['environment']['executable']))==p['interpreter_sha256']
            for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
            for port,sign in (('main',1),('side',-1)):
                meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['valid_interval']==[0,frames]
                actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(2,frames)
                assert np.array_equal(actual,np.array(main,dtype=np.float32)*sign)
            for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
            assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':math.ceil(frames/host),'frames':frames}
            expected=np.array(main,dtype=np.float32)*np.float32(.5);actual=np.array(read_wav(run/'output.wav')[1]);error=np.abs(actual-expected)
            assert np.all(error<=2e-6+1e-5*np.abs(expected)) and actual.shape==(2,frames)
            assert sha(run/'output.wav')==r['output_sha256']==json.loads((run/'complete.json').read_text())['output_sha256']
            checks.append({'run':str(run),'frames':frames,'output_sha256':sha(run/'output.wav'),'maximum_absolute_error':float(error.max())})
            boundaries.append({'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
    return checks,boundaries

def port_join_checks(root):
    assert json.loads((root/'probe.json').read_text())=={'code':'mixed_input_timeline_mismatch','node_id':'join','join_calls':0,'pair_prepares':1,'pair_calls':3}
    rate,samples=read_wav(root/'input.wav');assert rate==48000 and len(samples)==2 and len(samples[0])==1025
    checks=[];boundaries=[]
    for port,tail,gain,echo in ((0,2,1,.25),(1,5,2,.125)):
        frames=1025+tail;expected=[]
        for channel in samples:
            x=lambda n:channel[n] if 0<=n<1025 else 0.0
            expected.append([f32(f32(x(i)*gain)+f32(x(i-tail)*echo)) for i in range(frames)])
        raw=root/f'native-regions/1/port-{port}.raw.wav';aligned=root/f'native-regions/1/port-{port}.aligned.wav'
        assert [list(c) for c in read_wav(aligned)[1]]==expected
        for c,pcm in enumerate(read_wav(raw)[1]):assert len(pcm)==frames+479 and list(pcm[479:])==expected[c]
        checks.append({'port':port,'frames':frames,'aligned_sha256':sha(aligned),'maximum_absolute_error':0})
        boundaries.append({'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
    return checks,boundaries

def random_checks(root):
    rate,samples=read_wav(root/'input.wav');assert rate==48000 and len(samples)==2 and len(samples[0])==1025
    pattern=[127,1]+[1+int(x%1024) for x in np.random.RandomState(20261023).randint(0,2**32,size=173,dtype=np.uint32)]
    fir=[];main=[]
    for channel in samples:
        x=lambda n:channel[n] if 0<=n<1025 else 0.0
        fir.append([f32(f32(x(i)+f32(x(i-1)*.5))+f32(x(i-2)*.25)) for i in range(1027)])
        main.append([f32(f32(v*.75)+f32(f32(i)*f32(.00001))) for i,v in enumerate(fir[-1])])
    expected=np.array(main,dtype=np.float32)*np.float32(.5)
    trace=[];cursor=0
    while cursor<1025:
        count=min(pattern[len(trace)%len(pattern)],1025-cursor);trace.append(count);cursor+=count
    checks=[];boundaries=[];pids=[]
    for replay in (0,1):
        run=root/str(replay);r=json.loads((run/'boundary-report.json').read_text());assert r['host_partitions']==pattern and r['actual_probe_frames']==trace
        for index,offset,raw_frames,pcm in ((1,479,1506,fir),(3,511,1538,expected.tolist())):
            task=r['tasks'][index];a=task['alignment'];assert task['host_partitions']==pattern
            assert a['offset_frames']==offset and a['raw_frames']==raw_frames and a['aligned_frames']==1027
            artifact=a['artifacts'][0];raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path']
            assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
            assert [list(c) for c in read_wav(aligned)[1]]==pcm
            for c,actual in enumerate(read_wav(raw)[1]):assert len(actual)==raw_frames and list(actual[offset:])==pcm[c]
            assert (artifact['raw_valid_start_frame'],artifact['valid_start_frame'],artifact['valid_frame_count'])==(offset,0,1027)
            boundaries.append({'replay':replay,'task':index,'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
        for task in r['tasks']:
            for output in task['outputs']:assert (output['valid_start_frame'],output['valid_frame_count'])==(0,1025 if output['node_id']=='file' else 1027)
        task=r['task_artifacts'][0];directory=Path(task['directory']);p=task['provenance'];pids.append(p['process_id'])
        snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
        assert sha(Path(p['environment']['executable']))==p['interpreter_sha256']
        for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
        for port,sign in (('main',1),('side',-1)):
            meta=p['outputs'][port];path=directory/meta['path'];assert sha(path)==meta['sha256'] and meta['valid_interval']==[0,1027]
            actual=np.load(io_path(path),allow_pickle=False);assert actual.dtype==np.float32 and actual.shape==(2,1027)
            assert np.array_equal(actual,np.array(main,dtype=np.float32)*sign)
        for meta in p['inputs'].values():assert sha((directory/p['result_manifest']).parent/meta['file'])==meta['sha256']
        assert json.loads((directory/'lifecycle.json').read_text())=={'prepares':1,'calls':2,'frames':1027}
        error=np.abs(np.array(read_wav(run/'output.wav')[1])-expected);assert np.all(error<=2e-6+1e-5*np.abs(expected))
        checks.append({'run':str(run),'actual_probe_frames':trace,'output_sha256':sha(run/'output.wav'),'maximum_absolute_error':float(error.max())})
    assert len(set(pids))==2 and pids[0]>0 and pids[1]>0
    assert checks[0]['output_sha256']==checks[1]['output_sha256']
    return checks,boundaries

def main():
    p=argparse.ArgumentParser();p.add_argument('--command-record',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--kind',choices=('fixed','speex','tail','interval','port-tail','port-join','random'),default='fixed');args=p.parse_args()
    record=json.loads(args.command_record.read_text(encoding='utf-8'));assert record['exit_code']==0
    assert sha(Path(record['log_path']))==record['log_sha256']
    for artifact in record['artifacts']:
        assert artifact.get('sha256') and sha(Path(artifact['path']))==artifact['sha256']
    start=dt.datetime.fromisoformat(record['started_utc']).timestamp();end=dt.datetime.fromisoformat(record['finished_utc']).timestamp()
    roots=[r for r in (ROOT/'build/runs').glob('mixed-'+args.kind+'-*') if start<=r.stat().st_mtime<=end and (args.kind!='random' or (r/'input.wav').is_file())];assert len(roots)==1
    if args.kind=='random':
        checks,boundaries=random_checks(roots[0])
        with args.output.open('x',encoding='utf-8') as out:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},out,indent=2)
        print('Verified seeded mixed partitions, native callback trace and byte-identical replay');return
    if args.kind in ('port-tail','port-join'):
        checks,boundaries=(port_tail_checks if args.kind=='port-tail' else port_join_checks)(roots[0])
        with args.output.open('x',encoding='utf-8') as out:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},out,indent=2)
        print(f'Verified {len(checks)} independent output timelines ({args.kind})');return
    if args.kind=='interval':
        checks,boundaries=interval_checks(roots[0])
        with args.output.open('x',encoding='utf-8') as out:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},out,indent=2)
        print(f'Verified {len(checks)} partial/empty-interval WAVs and both Python outputs');return
    if args.kind=='tail':
        checks,boundaries=tail_checks(roots[0])
        with args.output.open('x',encoding='utf-8') as out:json.dump({'command_sha256':sha(args.command_record),'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},out,indent=2)
        print(f'Verified {len(checks)} finite-tail WAVs and native/Python extended valid intervals');return
    if args.kind=='speex':
        checks,boundaries=speex_checks(roots[0])
        with args.output.open('x',encoding='utf-8') as out:json.dump({'command_sha256':sha(args.command_record),'oracle_wav_sha256':sha(roots[0]/'original.wav'),'oracle_source_sha256':record['sources']['tests/integration/test_speex_denoise.cpp'],'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks)},out,indent=2)
        print(f'Verified {len(checks)} real Speex/Python component/CLI WAVs and native/Python boundaries');return
    root=roots[0];samples=read_wav(root/'input.wav')[1];checks=[];boundaries=[]
    for host in (1,127,256,480,512,1024):
        run=root/str(host);checks.append(verify(run,samples));r=json.loads((run/'run.json').read_text(encoding='utf-8'))
        expected=[[[f32(x*2) for x in c] for c in samples],read_wav(run/'output.wav')[1]]
        for ordinal,(index,offset) in enumerate(((1,479),(3,511))):
            a=r['tasks'][index]['alignment'];assert a['offset_frames']==offset and a['raw_frames']==1025+offset and a['aligned_frames']==1025
            assert len(a['artifacts'])==1
            for artifact in a['artifacts']:
                raw=run/artifact['raw_path'];aligned=run/artifact['aligned_path'];assert sha(raw)==artifact['raw_sha256'] and sha(aligned)==artifact['aligned_sha256']
                rate,raw_samples=read_wav(raw);rate2,aligned_samples=read_wav(aligned);assert rate==rate2==48000
                for c in range(2):
                    assert len(raw_samples[c])==1025+offset and len(aligned_samples[c])==1025
                    assert all(x==0 for x in raw_samples[c][:offset])
                    assert list(raw_samples[c][offset:])==list(aligned_samples[c])==list(expected[ordinal][c])
                boundaries.append({'run':str(run),'task':index,'offset_frames':offset,'raw_sha256':sha(raw),'aligned_sha256':sha(aligned)})
    with args.output.open('x',encoding='utf-8') as out:json.dump({'source_command':str(args.command_record),'command_sha256':sha(args.command_record),'checks':checks,'native_boundaries':boundaries,'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks),'limitations':['Fixed zero intrinsic latency only; delayed/tailed profiles and random mixed partitions are separate acceptance behaviors.']},out,indent=2)
    print(f'Verified {len(checks)} mixed WAVs, {len(boundaries)} raw/aligned pairs and Python code/output NPY hashes')
if __name__=='__main__':main()
