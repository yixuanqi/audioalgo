"""Independently inspect parallel native request gates, complete budgets and audio isolation."""
import argparse,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_budget import inspect_archive

NAME='PC-06.c parallel native consumers own separate near-limit budgets and one failure cannot consume another quota'
LIMIT=16*1024*1024
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0609-parallel-budget'
    case=next(c for c in ET.parse(args.junit).iter('testcase')if c.get('name')==NAME);assert not case.findall('failure')and not case.findall('error')and not case.findall('skipped')
    root=Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0609_PARALLEL_ROOT='))).resolve();assert root.parent==folder/'artifacts';identity=load(root/'dll-identity.json');dll=Path(identity['dll']);assert sha(dll)==identity['sha256']==load(repo/'build/reports/loops/PC0606-native-budget/independent-data.json')['dll_sha256']
    sources={}
    for index,name in enumerate(['A','B']):
        rate,source=read_wav(root/('input-'+name+'.wav'));assert rate==48000 and len(source)==2 and all(len(c)==513 for c in source)
        expected=[[f32(.125*math.sin(.03*(n+1))+.02*c)if index==0 else f32(.075*math.cos(.017*(n+1))+.01*c)for n in range(513)]for c in range(2)]
        assert[list(c)for c in source]==expected;sources[name]=source
    assert sha(root/'input-A.wav')!=sha(root/'input-B.wav')
    rows=load(root/'parallel-demo.json');gates=load(root/'barriers.json');assert len(rows)==4 and[(r['pair'],r['input'])for r in rows]==[(0,'A'),(0,'B'),(1,'A'),(1,'B')]and len({r['request']for r in rows})==4
    results=[];requests={};raw_paths=set();successes=[]
    for row in rows:
        pair=row['pair'];name=row['input'];label='pair-'+str(pair)+'-'+name;success=pair==0 or name=='B';assert row['success']==success and row['code']==(''if success else'log_limit');project=load(root/(label+'.aaproj'));nodes={n['id']:n for n in project['nodes']};assert project['project_id']=='pc0609-parallel-native'and project['revision']==19 and nodes['file']['parameters']['path']=='input-'+name+'.wav'and nodes['native']['parameters']['log_bytes']==LIMIT+(0 if success else 1)
        path=Path(row['archive']).resolve();assert path.parent.parent==root/'run-logs';data=inspect_archive(path,sha(dll),project_id=project['project_id'],revision=19,native_nodes=('native',));assert data['identity'][1]==row['request']and data['identity'][3]==41 and data['raw_bytes']==LIMIT and data['streams']['native']['events']==16384
        raw=Path(data['streams']['native']['path']).resolve();assert raw not in raw_paths;raw_paths.add(raw);events=[json.loads(s)for s in path.read_text(encoding='utf-8').splitlines()];assert events[0]['total_bytes']==1024 and events[0]['sequence']==1 and events[0]['source']=='native';requests[row['request']]=dict(pair=pair,raw=str(raw),first_bytes=events[0]['total_bytes'])
        run=(root/row['run']).resolve();assert run.is_relative_to(root/'runs')
        if not success:
            assert not run.exists()and len(data['diagnostics'])==1 and data['diagnostics'][0]['node_id']=='native'and data['diagnostics'][0]['text'].startswith('log_limit:')
        else:
            assert not data['diagnostics'];record=load(run/'run.json');complete=load(run/'complete.json');rate,output=read_wav(run/'output.wav');expected=[[f32(2*x)for x in c]for c in sources[name]];assert rate==48000 and[list(c)for c in output]==expected
            assert record['frames']==513 and record['channels']==2 and record['block_size']==1 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['project_id']==project['project_id']and record['project_revision']==19 and record['session_epoch']==41 and record['request_id']==row['request']
            assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')and len(record['plugins'])==1 and record['plugins'][0]['library_sha256']==sha(dll)
            for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/('input-'+name+'.wav'))==sha(run/resource['snapshot_path'])
            successes.append(dict(pair=pair,input=name,run=str(run),output_sha256=record['output_sha256']))
        results.append(dict(pair=pair,input=name,request=row['request'],success=success,raw_bytes=LIMIT,raw_sha256=sha(raw),code=row['code']))
    assert len(gates)==2 and[g['pair']for g in gates]==[0,1]
    for gate in gates:
        assert len(gate['blocked'])==2 and len({b['request']for b in gate['blocked']})==2
        for blocked in gate['blocked']:
            match=requests[blocked['request']];assert match['pair']==gate['pair']and blocked['entered']is True and blocked['pending_before_release']is True and blocked['first_bytes']==match['first_bytes']==1024 and Path(blocked['raw_file']).resolve()==Path(match['raw'])
    assert len(successes)==3 and len(list((root/'runs').iterdir()))==3 and len({r['output_sha256']for r in successes})==2
    b=[r['output_sha256']for r in successes if r['input']=='B'];assert len(b)==2 and b[0]==b[1]
    report=dict(status='passed',root=str(root),actual_successful_wavs=3,failed_requests=1,native_raw_streams=4,bytes_per_request=LIMIT,barrier_pairs=2,maximum_error=0,dll_sha256=sha(dll),runs=results,successes=successes,scope='actual two independent RunService consumers with shared immutable catalog/DLL,overlapping real worker log barriers,full budgets and one-request overflow isolation;not editor parallel scheduling/native cancel-close-late/UI flood/desktop')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',successful_wavs=3,failed_requests=1,native_raw_streams=4,barrier_pairs=2,maximum_error=0)))
if __name__=='__main__':main()
