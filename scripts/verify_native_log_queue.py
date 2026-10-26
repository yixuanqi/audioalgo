"""Read SDK-only consumer results, compiler inputs and preserved v1 identities."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):return json.loads(path.read_text(encoding='utf-8'))
def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args()
    folder=args.folder.resolve();repo=Path(__file__).resolve().parents[1];assert folder.parent==repo/'build/reports/loops' and folder.name=='PC0601-native-log-queue'
    external=Path(read(folder/'external-path.json')['external']).resolve();assert not external.is_relative_to(repo)
    suite=ET.parse(folder/'current-target.xml').getroot();cases=suite.findall('testcase');assert len(cases)==1 and suite.get('failures')==suite.get('skipped')=='0'
    case=cases[0];assert case.get('name')=='native_log_queue_contract' and case.find('failure') is None
    metrics=json.loads(case.findtext('system-out').strip());assert metrics==dict(status='passed',failures=0,callback_pop_allocations=0,capacity=64,max_message_bytes=1024,basic_accepted=68,basic_full=1,basic_rejected=9,concurrent_delivered=20000)
    identity=read(folder/'consumer-run-identity.json');binary=Path(identity['binary']);assert binary==external/'build-after/Release/native_log_queue_consumer.exe' and identity['exit_code']==0
    assert identity['before_sha256']==identity['after_sha256']==sha(binary) and Path(identity['cwd'])==external
    pe=binary.read_bytes();offset=struct.unpack_from('<I',pe,0x3c)[0];assert pe[:2]==b'MZ' and pe[offset:offset+4]==b'PE\0\0' and struct.unpack_from('<H',pe,offset+4)[0]==0x8664
    hashes={}
    for name in ('CMakeLists.txt','consumer.cpp','abi_check.c'):
        installed=external/'consumer'/name;source=repo/'tests/sdk/native_log_queue'/name;assert installed.read_bytes()==source.read_bytes();hashes[str(installed)]=sha(installed)
    for name in ('node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp'):
        installed=external/'sdk-after/include/audioalgo'/name;source=repo/'sdk/include/audioalgo'/name;assert installed.read_bytes()==source.read_bytes();hashes[str(installed)]=sha(installed)
    legacy=read(folder/'baseline-identity.json');assert sha(repo/legacy['old_report'])==legacy['old_report_sha256']
    for artifact in legacy['artifacts']:assert sha(Path(artifact['path']))==artifact['sha256']
    assert sha(external/'sdk-before/include/audioalgo/node_abi_v1.h')==sha(external/'sdk-after/include/audioalgo/node_abi_v1.h')==legacy['artifacts'][0]['sha256']
    cache=(external/'build-after/CMakeCache.txt').read_text(encoding='utf-8');assert str(repo).replace('\\','/').lower() not in cache.replace('\\','/').lower()
    assert ('AudioAlgoNodeSDK_DIR:PATH='+str(external/'sdk-after/lib/cmake/AudioAlgoNodeSDK').replace('\\','/')) in cache.replace('\\','/')
    logs=list((external/'build-after').glob('native_log_queue_consumer.dir/Release/*/CL.read.1.tlog'));assert len(logs)==1
    inputs=logs[0].read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower() not in inputs and 'juce' not in inputs
    assert str(external/'sdk-after/include/audioalgo/log_queue_v1.hpp').replace('\\','/').lower() in inputs
    assert str(external/'sdk-after/include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower() in inputs
    current=read(folder/'current-target.json')['sources'];old=read(repo/'build/reports/loops/PC0506-source-port/mixed-regression.json')['sources'];products=[p for p in old if p.startswith(('libs/','apps/','plugins/')) or p in ('CMakeLists.txt','CMakePresets.json')];assert all(current[p]==old[p] for p in products)
    report=dict(status='passed',external=str(external),metrics=metrics,consumer_binary=identity,source_and_sdk_hashes=hashes,compiler_inputs_sha256=sha(logs[0]),compiler_input_count=len(inputs.splitlines()),preserved_v1=legacy,unchanged_product_paths=products,scope='SDK C/C++ queue contract and immutable v1/host identities only; host negotiation/real native DLL logging and old DLL on extension-enabled host not implemented/verified')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',cases=1,allocations=0,concurrent_delivered=20000,v1_and_hosts_unchanged=True)))
if __name__=='__main__':main()
