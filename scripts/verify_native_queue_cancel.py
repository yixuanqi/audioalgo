"""Read actual production queue native baseline/cancel/recovery artifacts."""
import argparse,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_tail import inspect_capture

NAME='PC-03.c active native production queue cancellation drains final logs removes payload and recovers'
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def main():
    p=argparse.ArgumentParser();p.add_argument('junit',type=Path);p.add_argument('--report',type=Path,required=True);a=p.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0612-native-cancel';case=next(c for c in ET.parse(a.junit).iter('testcase')if c.get('name')==NAME);assert not any(case.findall(k)for k in ['failure','error','skipped']);root=Path(next(l.split('=',1)[1]for l in case.findtext('system-out','').splitlines()if l.startswith('PC0612_CANCEL_ROOT='))).resolve();assert root.parent==folder/'artifacts';identity=load(root/'dll-identity.json');dll=Path(identity['dll']);assert sha(dll)==identity['sha256']==load(folder.parent/'PC0611-detach-drain/independent-data.json')['dll_sha256'];rows=load(root/'cancel-demo.json');assert[r['name']for r in rows]==['baseline','cancelled','recovery']and len({r['request']for r in rows})==3
    rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)];captures=[];outputs=[]
    project=load(root/'native-cancel.aaproj');assert project['project_id']=='pc0612-native-cancel'and project['revision']==31 and{n['id']for n in project['nodes']}=={'file','native-A','native-B','output'}
    for row in rows:
        cancelled=row['name']=='cancelled';assert row['code']==('run_cancelled'if cancelled else'')and row['active']==row['pending']==row['jobs']==0 and row['worker_claimed']is False and row['worker_events']==row['ui_events']==4 and row['first_log_barrier']==cancelled
        if cancelled:assert 0<row['cancel_ms']<=5000
        else:assert row['cancel_ms']==0
        path=Path(row['archive']).resolve();assert path.parent.parent==root/'run-logs';events=[json.loads(l)for l in path.read_text(encoding='utf-8').splitlines()];assert len(events)==4 and all(e['source']=='native'and e['level']=='info'and e['project_id']=='pc0612-native-cancel'and e['revision']==31 and e['session_epoch']==53 and e['request_id']==row['request']and e['package_id']=='example.sdk_logging.detach'and e['content_sha256']==sha(dll)for e in events)
        paths=set()
        for node in ['native-A','native-B']:
            stream=[e for e in events if e['node_id']==node];assert[e['text']for e in stream]==['F'*16,'sdk-detach accepted\n']and[e['sequence']for e in stream]==[1,2]and[e['total_bytes']for e in stream]==[16,36];raw=Path(stream[0]['raw_log_file']).resolve();assert raw.parent==path.parent and raw not in paths;paths.add(raw);assert raw.read_bytes()==b'F'*16+b'sdk-detach accepted\n';capture=inspect_capture(raw,events);s=load(raw.with_suffix('.summary.json'));assert s['accepted_bytes']==s['persisted_bytes']==36 and s['accepted_messages']==s['persisted_messages']==2 and s['raw_complete']and s['all_messages_accepted']and not s['failure_code'];captures.append(dict(name=row['name'],node=node,**capture))
        run=root/row['run']
        if cancelled:assert not run.exists();continue
        record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');assert out_rate==48000 and[list(c)for c in output]==[[f32(4*x)for x in c]for c in source];assert record['frames']==513 and record['channels']==2 and record['block_size']==127 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['request_id']==row['request']and record['project_revision']==31 and record['session_epoch']==53 and len(record['plugins'])==2 and all(p['library_sha256']==sha(dll)for p in record['plugins']);assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
        for r in record['resources']:assert r['sha256']==r['snapshot_sha256']==sha(root/'input.wav')==sha(run/r['snapshot_path'])
        outputs.append(dict(name=row['name'],sha256=sha(run/'output.wav')))
    assert len(outputs)==2 and outputs[0]['sha256']==outputs[1]['sha256']and len(list((root/'runs').iterdir()))==2
    report=dict(status='passed',root=str(root),actual_successful_wavs=2,cancelled_runs=1,native_raw_streams=6,maximum_error=0,cancellation_ms=rows[1]['cancel_ms'],captures=captures,outputs=outputs,dll_sha256=sha(dll),scope='Actual EditorRunQueue worker cancellation at first native raw/event barrier,UI delivery/completion,final sinks,cleanup,idle counts and samequeue recovery;not production outer GUI button/OS process cleanup/View-app close/late crash-module lifetime/desktop')
    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',wavs=2,cancelled_runs=1,captures=6,cancellation_ms=rows[1]['cancel_ms'],maximum_error=0)))
if __name__=='__main__':main()
