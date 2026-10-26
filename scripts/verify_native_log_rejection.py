"""Independently inspect actual SDK status consumer and production native rejection artifacts."""
import argparse,json,math,struct
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_artifacts import exports

NAME='PC-06.c real native queue and message rejection preserve prefix fail safely and recover through production controls'
STATUS={'ok':0,'empty':0,'full':1,'utf8':4,'length':3,'size':2,'version':2,'level':2,'null':2}
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def archive(path,dll_hash,mode):
    rows=[json.loads(line)for line in path.read_text(encoding='utf-8').splitlines()];native=[e for e in rows if e['source']=='native'];diagnostics=[e for e in rows if e['source']=='diagnostic'];assert rows and len(rows)==len(native)+len(diagnostics)
    identity=tuple(rows[0][k]for k in ['project_id','request_id','revision','session_epoch']);assert identity[0]=='pc0607-native-rejection'
    for n,event in enumerate(rows):
        assert tuple(event[k]for k in ['project_id','request_id','revision','session_epoch'])==identity and event['node_id']=='native'and event['type_id']=='example.sdk_logging'and event['package_id']=='example.sdk_logging.faults'and event['version']=='1.0.0'and event['content_sha256']==dll_hash and event['sequence']==n+1
    succeeds=mode in ['ok','empty'];code=''if succeeds else'native_log_queue_full'if mode=='full'else'native_log_message_invalid'
    assert len(diagnostics)==(0 if succeeds else 1)and len(native)==(64 if mode=='full'else 3 if mode=='empty'else 2)
    if diagnostics:assert diagnostics[0]['level']=='error'and diagnostics[0]['text'].startswith(code+':')
    if mode=='full':assert '1 messages / 1024 bytes rejected'in diagnostics[0]['text']
    elif diagnostics:assert 'rejected 1 invalid messages'in diagnostics[0]['text']
    assert native[0]['text']=='fault-prefix\n'
    if mode=='empty':assert native[1]['text']==''
    paths={Path(e['raw_log_file']).resolve()for e in native};assert len(paths)==1;raw=next(iter(paths));assert raw.is_relative_to(path.parent)
    expected=b'fault-prefix\n'+(b'F'*(63*1024)if mode=='full'else('fault-status='+str(STATUS[mode])+'\n').encode())
    assert raw.read_bytes()==expected==b''.join(e['text'].encode()for e in native)
    written=0
    for event in native:
        written+=len(event['text'].encode());assert event['level']=='info'and event['total_bytes']==written+(1024 if mode=='full'else 0)and event['discarded_bytes']==(1024 if mode=='full'else 0)and event['dropped_text_bytes']==0
    return dict(request=identity[1],revision=identity[2],epoch=identity[3],raw_sha256=sha(raw),raw_bytes=len(expected),accepted=len(native),full_bytes=1024 if mode=='full'else 0,code=code)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0607-native-rejection'
    case=next(c for c in ET.parse(args.junit).iter('testcase')if c.get('name')==NAME);assert not case.findall('failure')and not case.findall('error')and not case.findall('skipped')
    root=Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0607_REJECT_ROOT='))).resolve();assert root.parent==folder/'artifacts';identity=load(root/'dll-identity.json');dll=Path(identity['dll']);assert sha(dll)==identity['sha256']
    command=load(args.junit.with_suffix('.json'));assert identity['cli_sha256']==next(a['sha256']for a in command['artifacts']if Path(a['path']).name=='audio_cli.exe')
    external=Path(load(folder/'external-path.json')['external']);sdk=Path(load(folder/'external-path.json')['sdk_source']);assert dll==external/'build/native_logging.dll'and not external.is_relative_to(repo)
    assert exports(dll)=={'audioalgo_node_api_v1','audioalgo_node_log_api_v1'}
    for source in(repo/'tests/sdk/native_log_faults').iterdir():assert source.read_bytes()==(external/'consumer'/source.name).read_bytes()
    for name in ['node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp']:assert(repo/'sdk/include/audioalgo'/name).read_bytes()==(sdk/'include/audioalgo'/name).read_bytes()
    logs=list((external/'build').glob('*.dir/Release/*/CL.read.1.tlog'));assert len(logs)==2
    for path in logs:
        text=path.read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower()not in text and'juce'not in text and str(sdk/'include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower()in text
    manifest=load(dll.parent/'manifest.json');data=dll.read_bytes();at=data.index(b'"plugin_id": "example.sdk_logging.faults"');start=data.rfind(b'{',0,at);end=data.index(b'\0',at);assert json.loads(data[start:end])==manifest
    sdk_receipt=load(folder/'sdk-consumer.json');assert sdk_receipt['exit_code']==0
    line=next(s[s.index('{'):].strip()for s in(folder/'sdk-consumer.log').read_text(encoding='utf-8').splitlines()if'{"rows":'in s);statuses=json.loads(line);assert statuses['status']=='passed'and statuses['failures']==statuses['host_callback_pop_allocations']==0 and len(statuses['rows'])==9
    for row in statuses['rows']:
        mode=row['mode'];assert row['producer_expected_status']==STATUS[mode]and row['process_status']==0
        assert row['accepted']==(64 if mode=='full'else 3 if mode=='empty'else 2)and row['full']==(1 if mode=='full'else 0)and row['full_bytes']==(1024 if mode=='full'else 0)and row['rejected']==(0 if mode in ['ok','empty','full']else 1)
        assert row['attempted_bytes']==(65549 if mode=='full'else 28)and row['raw_bytes']==(64525 if mode=='full'else 28)
    consumer=external/'build/Release/native_log_fault_consumer.exe';pe=consumer.read_bytes();at=struct.unpack_from('<I',pe,0x3c)[0];assert pe[:2]==b'MZ'and pe[at:at+4]==b'PE\0\0'and struct.unpack_from('<H',pe,at+4)[0]==0x8664
    rate,source=read_wav(root/'input.wav');assert rate==48000 and[list(c)for c in source]==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)];expected=[[f32(2*x)for x in c]for c in source]
    demo=load(root/'rejection-demo.json');assert len(demo)==16 and[demo[n]['mode']for n in range(2)]==['ok','empty'];paths=set();requests=set();successes=[];failures=[];details=[]
    for row in demo:
        mode=row['mode'];label=row['label'];project=load(root/(label+'.aaproj'));assert next(n for n in project['nodes']if n['id']=='native')['parameters']['mode']==mode
        path=(root/row['archive']).resolve();assert path.is_relative_to(root/'run-logs');paths.add(path);result=archive(path,sha(dll),mode);assert result['request']==row['request']and result['request']not in requests;requests.add(result['request']);assert row['success']==(mode in ['ok','empty'])and row['code']==result['code']
        ui=(root/(label+'-ui.txt')).read_text(encoding='utf-8');status=(root/(label+'-status.txt')).read_text(encoding='utf-8');assert 'fault-prefix'in ui and row['request']in ui and sha(dll)in ui and'node=native'in ui
        if row['success']:
            run=(root/row['run']).resolve();assert run.is_relative_to(root/'runs');record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');assert out_rate==48000 and[list(c)for c in output]==expected
            assert record['frames']==513 and record['channels']==2 and record['block_size']==127 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['request_id']==row['request']and record['project_revision']==result['revision']
            assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')and record['plugins'][0]['library_sha256']==sha(dll)
            for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
            assert 'Completed run'in status;successes.append(dict(label=label,run=str(run),sha256=sha(run/'output.wav')))
        else:
            assert row['run']==''and'Completed run'not in status and result['code']in status and result['code']in ui;failures.append(dict(label=label,mode=mode,code=result['code']))
        details.append(dict(label=label,mode=mode,**result))
    assert len(successes)==9 and len(failures)==7 and len({r['sha256']for r in successes})==1 and len(list((root/'runs').iterdir()))==9
    cli_paths=[p.resolve()for p in(root/'run-logs').glob('*/events.jsonl')if p.resolve()not in paths];assert len(cli_paths)==2;cli=[]
    for path in cli_paths:
        rows=[json.loads(s)for s in path.read_text(encoding='utf-8').splitlines()];mode='full'if len([e for e in rows if e['source']=='native'])==64 else'utf8';result=archive(path,sha(dll),mode);assert not(root/'cli-runs'/mode).exists()and(root/('cli-'+mode+'.txt')).read_text(encoding='utf-8').startswith(result['code']+' [node=native]');cli.append(dict(mode=mode,**result))
    assert {r['mode']for r in cli}=={'full','utf8'}
    report=dict(status='passed',root=str(root),actual_successful_wavs=9,gui_failed_requests=7,cli_failed_requests=2,gui_archives=details,cli_archives=cli,successful_runs=successes,maximum_error=0,sdk_status_rows=statuses['rows'],host_callback_pop_allocations=0,sdk_consumer_sha256=sha(consumer),dll_sha256=sha(dll),scope='real production controls/SDK rejection/raw counts/numerical recovery+freshCLI and independent SDK API statuses;not native per-stream capture tail/parallel rejection/lifecycle/desktop')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',successful_wavs=9,gui_failures=7,cli_failures=2,sdk_status_modes=9,maximum_error=0)))
if __name__=='__main__':main()
