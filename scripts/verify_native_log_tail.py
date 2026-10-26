"""Read actual SDK native capture summaries, raw files and independent audio oracle."""
import argparse,base64,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_artifacts import exports

NAME='PC-04.b native stream keeps exact bounded UTF8 capture tail and independent persisted counters'
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def inspect_capture(raw,events):
    raw=Path(raw).resolve();s=load(raw.with_suffix('.summary.json'));data=raw.read_bytes();text=data.decode('utf-8');tail=data[-65536:]
    # The first retained byte must begin a codepoint. NUL is a valid byte.
    while tail and tail[0]&0xc0==0x80:tail=tail[1:]
    tail.decode('utf-8');assert base64.b64decode(s['tail_utf8_base64'],validate=True)==tail
    assert s['schema_version']==1 and Path(s['raw_log_file']).resolve()==raw and s['source']=='native'
    assert s['persisted_bytes']==len(data) and s['tail_limit_bytes']==65536 and s['tail_bytes']==len(tail)<=65536 and s['tail_discarded_bytes']==len(data)-len(tail)
    assert s['attempted_valid_bytes']==s['accepted_bytes']+s['queue_full_bytes'] and s['accepted_bytes']>=s['persisted_bytes'] and s['accepted_messages']>=s['persisted_messages']
    assert s['raw_complete']==(s['accepted_bytes']==len(data)) and s['all_messages_accepted']==(s['queue_full_messages']==s['rejected_messages']==s['closed_messages']==0)
    native=[e for e in events if e['source']=='native' and e['raw_log_file'] and Path(e['raw_log_file']).resolve()==raw];assert len(native)==s['persisted_messages']
    reconstructed=[]
    for e in native:
        for k in ['project_id','request_id','revision','session_epoch','node_id','type_id','package_id','version','content_sha256','source']:assert e[k]==s[k]
        reconstructed.append(base64.b64decode(e['text_utf8_base64'],validate=True)if e.get('text_encoding')=='utf8-base64'else e['text'].encode('utf-8'))
    assert b''.join(reconstructed)==data
    return dict(raw=str(raw),raw_sha256=sha(raw),summary_sha256=sha(raw.with_suffix('.summary.json')),persisted_bytes=len(data),tail_bytes=len(tail),tail_discarded_bytes=len(data)-len(tail),accepted_bytes=s['accepted_bytes'],queue_full_messages=s['queue_full_messages'],rejected_messages=s['rejected_messages'],raw_complete=s['raw_complete'],all_messages_accepted=s['all_messages_accepted'],failure_code=s['failure_code'])
def main():
    p=argparse.ArgumentParser();p.add_argument('junit',type=Path);p.add_argument('--direct-junit',type=Path,action='append',default=[]);p.add_argument('--report',type=Path,required=True);a=p.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0610-native-tail';case=next(c for c in ET.parse(a.junit).iter('testcase')if c.get('name')==NAME);assert not any(case.findall(k)for k in ['failure','error','skipped'])
    root=Path(next(l.split('=',1)[1]for l in case.findtext('system-out','').splitlines()if l.startswith('PC0610_TAIL_ROOT='))).resolve();assert root.parent==folder/'artifacts';external=Path(load(folder/'external-path.json')['external']);sdk=Path(load(folder/'external-path.json')['sdk_source']);identity=load(root/'dll-identity.json');dll=Path(identity['dll']);assert sha(dll)==identity['sha256']and exports(dll)=={'audioalgo_node_api_v1','audioalgo_node_log_api_v1'}and dll==external/'build/native_logging.dll'
    for src in(repo/'tests/sdk/native_log_tail').iterdir():assert src.read_bytes()==(external/'consumer'/src.name).read_bytes()
    for name in ['node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp']:assert(repo/'sdk/include/audioalgo'/name).read_bytes()==(sdk/'include/audioalgo'/name).read_bytes()
    tlogs=list((external/'build').glob('native_log_tail.dir/Release/*/CL.read.1.tlog'));assert len(tlogs)==1;inputs=tlogs[0].read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower()not in inputs and'juce'not in inputs and str(sdk/'include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower()in inputs
    rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)]
    rows=load(root/'tail-demo.json');assert[r['name']for r in rows]==['ascii-exact','ascii-plus-one','euro-plus-two','nul-plus-one']and len({r['request']for r in rows})==4;captures=[];wavs=[]
    for row in rows:
        path=Path(row['archive']).resolve();assert path.parent.parent==root/'run-logs';events=[json.loads(l)for l in path.read_text(encoding='utf-8').splitlines()];assert all(e['source']=='native'for e in events);raw=Path(row['raw']);assert raw.resolve().parent==path.parent;expected=(b'\xe2\x82\xac'*(row['bytes']//3)if row['pattern']=='euro'else(b'\0'if row['pattern']=='nul'else b'F')*row['bytes']);assert raw.read_bytes()==expected;capture=inspect_capture(raw,events);s=load(Path(row['summary']));assert s['project_id']=='pc0610-native-tail'and s['request_id']==row['request']and s['revision']==23 and s['session_epoch']==43 and s['content_sha256']==sha(dll)and s['package_id']=='example.sdk_logging.tail'and s['failure_code']==''and s['raw_complete']and s['all_messages_accepted'];assert s['tail_bytes']==(65535 if row['pattern']=='euro'else 65536)and s['tail_discarded_bytes']==(3 if row['pattern']=='euro'else row['bytes']-65536);captures.append(dict(name=row['name'],**capture))
        run=root/row['run'];record=load(run/'run.json');complete=load(run/'complete.json');out_rate,audio=read_wav(run/'output.wav');assert out_rate==48000 and[list(c)for c in audio]==[[f32(2*x)for x in c]for c in source];assert record['frames']==513 and record['channels']==2 and record['block_size']==1 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['request_id']==row['request']and record['project_revision']==23 and record['session_epoch']==43
        assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')and len(record['plugins'])==1 and record['plugins'][0]['library_sha256']==sha(dll)
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        wavs.append(dict(name=row['name'],sha256=sha(run/'output.wav')))
    assert len({w['sha256']for w in wavs})==1
    # Actual pre-fix Run/raw/audio success,with every new summary missing.
    red=ET.parse(folder/'behavior-red.xml');assert red.find('testsuite').get('failures')=='95';red_root=Path(next(l.split('=',1)[1]for l in red.find('.//testcase').findtext('system-out','').splitlines()if l.startswith('PC0610_TAIL_ROOT=')));red_rows=load(red_root/'tail-demo.json');assert len(red_rows)==4 and all(Path(r['raw']).stat().st_size==r['bytes']and not Path(r['summary']).exists()and(red_root/r['run']/'output.wav').is_file()for r in red_rows)
    direct=[];seen=set()
    for junit in a.direct_junit:
        tree=ET.parse(junit);assert all(not any(c.findall(k)for k in ['failure','error','skipped'])for c in tree.iter('testcase'))
        for c in tree.iter('testcase'):
            for line in c.findtext('system-out','').splitlines():
                if '_ROOT='not in line:continue
                candidate=Path(line.split('=',1)[1]).resolve();assert candidate.is_relative_to(repo/'build/reports/loops')
                for archive in(candidate/'run-logs').glob('*/events.jsonl'):
                    events=[json.loads(l)for l in archive.read_text(encoding='utf-8').splitlines()]
                    for raw in {e['raw_log_file']for e in events if e['source']=='native'and e.get('raw_log_file')}:
                        if raw not in seen:direct.append(inspect_capture(raw,events));seen.add(raw)
    report=dict(status='passed',root=str(root),actual_successful_wavs=4,native_raw_streams=4,maximum_error=0,captures=captures,direct_captures=direct,dll_sha256=sha(dll),wavs=wavs,red_findings='Four actual raw/audio successes without captured-tail summary;95 real behavior failures retained.',scope='Worker-owned per-native-stream64KiB suffix/counters and raw bytes;not GUI continuous flood/full detach/cancel/close/late lifecycle/desktop')
    a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',wavs=4,captures=4,direct_captures=len(direct),maximum_error=0)))
if __name__=='__main__':main()

