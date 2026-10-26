"""Independently prove accepted synchronous detach records reach raw/event/capture."""
import argparse,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_artifacts import exports
from verify_native_log_tail import inspect_capture

NAME='PC-06.d accepted synchronous detach logs drain before queue closure for both native instances'
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def get_root(junit,passing=True):
    case=next(c for c in ET.parse(junit).iter('testcase')if c.get('name')==NAME)
    if passing:assert not any(case.findall(k)for k in ['failure','error','skipped'])
    return Path(next(l.split('=',1)[1]for l in case.findtext('system-out','').splitlines()if l.startswith('PC0611_DETACH_ROOT='))).resolve()
def main():
    p=argparse.ArgumentParser();p.add_argument('junit',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0611-detach-drain';root=get_root(a.junit);assert root.parent==folder/'artifacts';identity=load(root/'dll-identity.json');dll=Path(identity['dll']);external=Path(load(folder/'external-path.json')['external']);sdk=Path(load(folder/'external-path.json')['sdk_source']);assert dll==external/'build/native_logging.dll'and sha(dll)==identity['sha256']and exports(dll)=={'audioalgo_node_api_v1','audioalgo_node_log_api_v1'}
    for source in(repo/'tests/sdk/native_log_detach').iterdir():assert source.read_bytes()==(external/'consumer'/source.name).read_bytes()
    for name in ['node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp']:assert(repo/'sdk/include/audioalgo'/name).read_bytes()==(sdk/'include/audioalgo'/name).read_bytes()
    tlogs=list((external/'build').glob('native_log_detach.dir/Release/*/CL.read.1.tlog'));assert len(tlogs)==1;inputs=tlogs[0].read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower()not in inputs and'juce'not in inputs and str(sdk/'include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower()in inputs
    rows=load(root/'detach-demo.json');assert len(rows)==2 and[r['attempt']for r in rows]==[0,1]and len({r['request']for r in rows})==2;rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)];captures=[];outputs=[]
    for row in rows:
        archive=Path(row['archive']).resolve();assert archive.parent.parent==root/'run-logs';events=[json.loads(l)for l in archive.read_text(encoding='utf-8').splitlines()];assert len(events)==4 and all(e['source']=='native'and e['level']=='info'and e['project_id']=='pc0611-detach-drain'and e['revision']==29 and e['session_epoch']==47 and e['request_id']==row['request']and e['content_sha256']==sha(dll)and e['package_id']=='example.sdk_logging.detach'for e in events)
        raws=set()
        for node in ['native-A','native-B']:
            stream=[e for e in events if e['node_id']==node];assert len(stream)==2 and[e['text']for e in stream]==['F'*16,'sdk-detach accepted\n']and[e['sequence']for e in stream]==[1,2]and[e['total_bytes']for e in stream]==[16,36]and all(e['discarded_bytes']==e['dropped_text_bytes']==0 for e in stream);raw=Path(stream[0]['raw_log_file']).resolve();assert raw.parent==archive.parent and all(Path(e['raw_log_file']).resolve()==raw for e in stream)and raw not in raws;raws.add(raw);assert raw.read_bytes()==b'F'*16+b'sdk-detach accepted\n';capture=inspect_capture(raw,events);summary=load(raw.with_suffix('.summary.json'));assert summary['persisted_messages']==summary['accepted_messages']==2 and summary['persisted_bytes']==summary['accepted_bytes']==summary['attempted_valid_bytes']==36 and summary['tail_bytes']==36 and summary['tail_discarded_bytes']==0 and summary['raw_complete']and summary['all_messages_accepted']and not summary['failure_code'];captures.append(dict(attempt=row['attempt'],node=node,**capture))
        run=root/row['run'];record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');assert out_rate==48000 and[list(c)for c in output]==[[f32(4*x)for x in c]for c in source]and record['frames']==513 and record['channels']==2 and record['block_size']==127 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['request_id']==row['request']and record['project_revision']==29 and record['session_epoch']==47;assert len(record['plugins'])==2 and all(p['library_sha256']==sha(dll)for p in record['plugins'])and record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        outputs.append(dict(attempt=row['attempt'],sha256=sha(run/'output.wav')))
    assert outputs[0]['sha256']==outputs[1]['sha256']
    red_root=get_root(folder/'red.xml',False);red_rows=load(red_root/'detach-demo.json');red_findings=[]
    for row in red_rows:
        events=[json.loads(l)for l in Path(row['archive']).read_text(encoding='utf-8').splitlines()];assert len(events)==2
        for node in ['native-A','native-B']:
            stream=[e for e in events if e['node_id']==node];assert len(stream)==1;raw=Path(stream[0]['raw_log_file']);s=load(raw.with_suffix('.summary.json'));assert raw.read_bytes()==b'F'*16 and s['accepted_messages']==2 and s['persisted_messages']==1 and s['accepted_bytes']==36 and s['persisted_bytes']==16 and not s['raw_complete']and s['all_messages_accepted']and not s['failure_code'];red_findings.append(dict(attempt=row['attempt'],node=node,accepted=36,persisted=16,lost_accepted_bytes=20))
    assert len(red_findings)==4
    report=dict(status='passed',root=str(root),actual_successful_wavs=2,native_raw_streams=4,maximum_error=0,captures=captures,outputs=outputs,dll_sha256=sha(dll),red_findings=red_findings,scope='Real serialized set_sink(NULL) borrowed-context farewell is accepted then worker drains before queueclose;two instances/two actual Run requests,no full active cancel/View/app close/late/crash lifecycle/desktop claim')
    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',wavs=2,captures=4,red_lost_accepted_bytes=80,maximum_error=0)))
if __name__=='__main__':main()
