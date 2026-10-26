"""Read actual PC0604 SDK DLL, GUI/CLI WAVs and encoded/raw/UI logs independently."""
import argparse
import base64
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32, read_wav, sha
from verify_native_log_artifacts import exports

NAME = 'PC-06.b real SDK Unicode levels and embedded NUL survive production Run archive history and copy'
TEXT = ['debug 中文 🎧\r\n', 'info before\0after "\' $() `literal` \x01\x1bEND\r\n',
        'warning <b>plain</b>\n', 'error terminal\n']
LEVELS = ['debug', 'info', 'warning', 'error']

def load(path): return json.loads(path.read_text(encoding='utf-8'))
def main():
    p=argparse.ArgumentParser();p.add_argument('junit',type=Path);p.add_argument('--report',type=Path,required=True);args=p.parse_args()
    repo=Path(__file__).resolve().parents[1];folder=repo/'build/reports/loops/PC0604-native-text'
    case=next(c for c in ET.parse(args.junit).iter('testcase')if c.get('name')==NAME)
    assert not case.findall('failure') and not case.findall('error') and not case.findall('skipped')
    root=Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0604_TEXT_ROOT='))).resolve()
    assert root.parent==folder/'artifacts'
    identity=load(root/'dll-identity.json');dll=Path(identity['dll']);assert sha(dll)==identity['sha256']
    receipt=load(args.junit.with_suffix('.json'))
    for filename,key in [('audioalgo_tests.exe','test_host_sha256'),('audio_cli.exe','cli_sha256')]:
        assert identity[key]==next(a['sha256']for a in receipt['artifacts']if Path(a['path']).name==filename)
    external=Path(load(folder/'external-path.json')['external']);sdk=Path(load(folder/'external-path.json')['sdk_source'])
    assert not external.is_relative_to(repo) and dll==external/'build/native_logging.dll'
    assert exports(dll)=={'audioalgo_node_api_v1','audioalgo_node_log_api_v1'}
    for source in (repo/'tests/sdk/native_log_text').iterdir():assert source.read_bytes()==(external/'consumer'/source.name).read_bytes()
    sdk_hashes={}
    for name in ['node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp']:
        source=repo/'sdk/include/audioalgo'/name;assert source.read_bytes()==(sdk/'include/audioalgo'/name).read_bytes();sdk_hashes[name]=sha(source)
    cache=(external/'build/CMakeCache.txt').read_text(encoding='utf-8').replace('\\','/').lower();assert str(repo).replace('\\','/').lower() not in cache
    logs=list((external/'build').glob('native_log_text.dir/Release/*/CL.read.1.tlog'));assert len(logs)==1
    inputs=logs[0].read_text(encoding='utf-16').replace('\\','/').lower();assert 'juce' not in inputs and str(repo).replace('\\','/').lower() not in inputs and str(sdk/'include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower() in inputs
    manifest=load(dll.parent/'manifest.json');data=dll.read_bytes();at=data.index(b'"schema_version":1');start=data.rfind(b'{',0,at);end=data.index(b'\0',at);assert json.loads(data[start:end])==manifest
    rate,source=read_wav(root/'input.wav');source=[list(c)for c in source];assert rate==48000 and len(source)==2 and all(len(c)==513 for c in source)
    assert source==[[f32(.125*math.sin(.03*(n+1))+.02*c)for n in range(513)]for c in range(2)]
    expected=[[f32(2*x)for x in channel]for channel in source];raw_expected=''.join(TEXT).encode();assert len(raw_expected)==97
    project=load(root/'project.aaproj');assert project['project_id']=='pc0604-native-text'
    rows=load(root/'text-demo.json');assert len(rows)==2 and {r['origin']for r in rows}=={'gui','cli'} and len({r['request']for r in rows})==2
    runs=[]
    for row in rows:
        run=(root/row['run']).resolve();archive=(root/row['archive']).resolve();assert run.is_relative_to(root) and archive.is_relative_to(root/'run-logs')
        record=load(run/'run.json');complete=load(run/'complete.json');out_rate,output=read_wav(run/'output.wav');assert out_rate==48000 and [list(c)for c in output]==expected
        assert record['request_id']==row['request'] and record['project_id']==project['project_id'] and record['project_revision']==9
        assert record['frames']==513 and record['channels']==2 and record['valid_start_frame']==0 and record['valid_frame_count']==513 and record['block_size']==127
        assert record['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
        for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(root/'input.wav')==sha(run/resource['snapshot_path'])
        assert len(record['plugins'])==1 and record['plugins'][0]['library_sha256']==sha(dll) and record['plugins'][0]['manifest_sha256']==sha(dll.parent/'manifest.json')
        events=[json.loads(line)for line in archive.read_text(encoding='utf-8').splitlines()];assert len(events)==4
        running=0;paths=set()
        for n,event in enumerate(events):
            original=base64.b64decode(event['text_utf8_base64'],validate=True).decode('utf-8') if event.get('text_encoding')=='utf8-base64' else event['text']
            assert original==TEXT[n] and event['text']==TEXT[n].replace('\0','\\0')
            assert ('text_encoding'in event)==(n==1) and ('text_utf8_base64'in event)==(n==1)
            assert event['level']==LEVELS[n] and event['sequence']==n+1 and event['source']=='native' and event['node_id']=='native'
            assert event['package_id']=='example.sdk_logging.text' and event['type_id']=='example.sdk_logging' and event['version']=='1.0.0' and event['content_sha256']==sha(dll)
            assert event['request_id']==row['request'] and event['project_id']==project['project_id'] and event['revision']==9 and event['session_epoch']==record['session_epoch']
            running+=len(TEXT[n].encode());assert event['total_bytes']==running and event['discarded_bytes']==event['dropped_text_bytes']==0
            paths.add(Path(event['raw_log_file']).resolve())
        assert len(paths)==1;raw=next(iter(paths));assert raw.is_relative_to(archive.parent) and raw.read_bytes()==raw_expected
        runs.append(dict(origin=row['origin'],run=str(run),request=row['request'],output_sha256=sha(run/'output.wav'),archive_sha256=sha(archive),raw_sha256=sha(raw),raw_bytes=97,events=4))
    visible=[text.replace('\0','\\0').replace('\x01','\\x01').replace('\x1b','\\x1b').replace('\r\n','\n')for text in TEXT]
    for name in ['ui-live.txt','ui-history.txt']:
        text=(root/name).read_text(encoding='utf-8');assert all(value in text for value in visible) and '\0' not in text and '\x01' not in text and '\x1b' not in text
    for level,body in zip(LEVELS,visible):
        text=(root/f'filter-{level}.txt').read_text(encoding='utf-8');assert body in text
        assert all(other not in text for other in visible if other!=body)
    report=dict(status='passed',root=str(root),actual_wavs=2,runs=runs,maximum_error=0,raw_bytes_per_run=97,
                encoded_nul_events=2,sdk_hashes=sdk_hashes,dll_sha256=sha(dll),test_host_sha256=identity['test_host_sha256'],cli_sha256=identity['cli_sha256'],
                scope='actual SDK Unicode/4levels/NUL/C0 plain text,canonical encoded archive/history and copied presentation; not full native budgets/capability/lifecycle/desktop')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',actual_wavs=2,raw_bytes_per_run=97,maximum_error=0)))
if __name__=='__main__':main()
