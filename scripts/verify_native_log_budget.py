"""Independently read PC0606 exact/overflow native task raw/event budgets and WAVs."""
import argparse,hashlib,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_artifacts import exports

NAME='PC-04.b real native task shares exact sixteen MiB log budget across instances and recovers'
LIMIT=16*1024*1024
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def case_root(junit,passing):
    case=next(c for c in ET.parse(junit).iter('testcase')if c.get('name')==NAME)
    if passing:assert not case.findall('failure')and not case.findall('error')and not case.findall('skipped')
    return Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0606_BUDGET_ROOT='))).resolve()
def inspect_archive(path,dll_hash,project_id='pc0606-native-budget',revision=11,native_nodes=('native-A','native-B')):
    streams={};diagnostics=[];identity=None
    for line in path.read_text(encoding='utf-8').splitlines():
        event=json.loads(line);current=(event['project_id'],event['request_id'],event['revision'],event['session_epoch'])
        if identity is None:identity=current
        assert current==identity and identity[0]==project_id and identity[2]==revision
        assert event['package_id']=='example.sdk_logging.budget'and event['type_id']=='example.sdk_logging'and event['content_sha256']==dll_hash and event['version']=='1.0.0'
        if event['source']=='diagnostic':diagnostics.append(event);continue
        assert event['source']=='native'and event['level']=='info'and event['node_id']in native_nodes
        stream=streams.setdefault(event['node_id'],dict(path=event['raw_log_file'],bytes=0,events=0))
        assert event['raw_log_file']==stream['path']and event['sequence']==stream['events']+1 and 0<len(event['text'])<=1024 and event['text']=='F'*len(event['text'])
        stream['bytes']+=len(event['text']);stream['events']+=1
        assert event['total_bytes']==stream['bytes']and event['discarded_bytes']==event['dropped_text_bytes']==0
    for stream in streams.values():
        raw=Path(stream['path']).resolve();assert raw.is_relative_to(path.parent)and raw.stat().st_size==stream['bytes']
        assert raw.read_bytes()==b'F'*stream['bytes'];stream['sha256']=sha(raw)
    return dict(identity=identity,streams=streams,raw_bytes=sum(s['bytes']for s in streams.values()),diagnostics=diagnostics)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0606-native-budget';root=case_root(args.junit,True);assert root.parent==folder/'artifacts'
    dll=load(root/'dll-identity.json');library=Path(dll['dll']);assert sha(library)==dll['sha256']and exports(library)=={'audioalgo_node_api_v1','audioalgo_node_log_api_v1'}
    receipt=load(args.junit.with_suffix('.json'));assert next(a['sha256']for a in receipt['artifacts']if Path(a['path']).name=='audio_cli.exe')==dll['cli_sha256']
    external=Path(load(folder/'external-path.json')['external']);sdk=Path(load(folder/'external-path.json')['sdk_source']);assert library==external/'build/native_logging.dll'and not external.is_relative_to(repo)
    for source in(repo/'tests/sdk/native_log_budget').iterdir():assert source.read_bytes()==(external/'consumer'/source.name).read_bytes()
    for name in ['node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp']:assert(repo/'sdk/include/audioalgo'/name).read_bytes()==(sdk/'include/audioalgo'/name).read_bytes()
    logs=list((external/'build').glob('native_log_budget.dir/Release/*/CL.read.1.tlog'));assert len(logs)==1;inputs=logs[0].read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower()not in inputs and'juce'not in inputs
    assert str(sdk/'include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower()in inputs
    rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)]
    cases=load(root/'budget-demo.json');assert [r['name']for r in cases]==['single-exact','single-plus-one','shared-exact','shared-plus-one','shared-flood'];archives=[];names=set();successes=[]
    def audio(name,count):
        run=root/'runs'/name;record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');expected=[[f32(x*(4 if count==2 else 2))for x in c]for c in source]
        assert out_rate==48000 and[list(c)for c in output]==expected
        assert record['frames']==513 and record['channels']==2 and record['block_size']==1 and record['valid_start_frame']==0 and record['valid_frame_count']==513
        assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')and len(record['plugins'])==count and all(p['library_sha256']==sha(library)for p in record['plugins'])
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        successes.append(dict(name=name,output_sha256=record['output_sha256'],instances=count))
    audio('baseline',2)
    for row in cases:
        name=row['name'];succeeds=name in ['single-exact','shared-exact'];assert row['success']==succeeds and row['code']==(''if succeeds else'log_limit')
        archive=Path(row['archive']).resolve();assert archive.parent.parent==root/'run-logs';names.add(archive);data=inspect_archive(archive,sha(library));assert data['raw_bytes']==row['captured_bytes']==LIMIT
        if succeeds:assert not data['diagnostics'];audio(name,len(row['log_bytes']))
        else:
            assert not(root/'runs'/name).exists()and data['diagnostics']and all(e['level']=='error'and e['text'].startswith('log_limit:')for e in data['diagnostics'])
        if len(row['log_bytes'])==2:assert {n:s['bytes']for n,s in data['streams'].items()}=={'native-A':LIMIT//2,'native-B':LIMIT//2}
        else:assert data['streams']['native-A']['bytes']==LIMIT
        audio('recovery-'+name,len(row['log_bytes']));archives.append(dict(name=name,archive=str(archive),**data))
    cli=[p.resolve()for p in(root/'run-logs').glob('*/events.jsonl')if p.resolve()not in names];assert len(cli)==1;data=inspect_archive(cli[0],sha(library));assert data['raw_bytes']==LIMIT and data['diagnostics']and all(e['text'].startswith('log_limit:')for e in data['diagnostics'])
    assert not(root/'cli-runs/shared-plus-one').exists()and(root/'cli-stdout.txt').read_text(encoding='utf-8').startswith('log_limit [node=')
    archives.append(dict(name='fresh-cli-shared-plus-one',archive=str(cli[0]),**data));assert len(successes)==8
    # Preserve and independently discriminate the actual pre-fix failure scope.
    red=case_root(folder/'red-budget.xml',False);red_cases={r['name']:r for r in load(red/'budget-demo.json')};assert red_cases['single-plus-one']['code']=='native_log_limit'
    red_findings=[]
    for name,size in [('shared-plus-one',LIMIT+1),('shared-flood',2*LIMIT)]:
        row=red_cases[name];assert row['success']and row['captured_bytes']==size and(red/'runs'/name/'complete.json').is_file();raw=inspect_archive(Path(row['archive']),sha(library));assert raw['raw_bytes']==size;red_findings.append(dict(name=name,raw_bytes=size,incorrect_success=True))
    report=dict(status='passed',root=str(root),actual_successful_wavs=8,successes=successes,archives=archives,raw_budget_each_task=LIMIT,failed_requests=4,maximum_error=0,dll_sha256=sha(library),red_findings=red_findings,scope='one/two-instance same native task exact/+1/finite sustained flood,worker+fresh CLI,no successful failed payload,numerical recovery;not native UI flood/heartbeat,per-stream capture tail,queue-invalid/full/lifecycle or desktop')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',successful_wavs=8,task_archives=6,failed_requests=4,maximum_error=0)))
if __name__=='__main__':main()
