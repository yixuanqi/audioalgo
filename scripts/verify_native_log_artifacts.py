"""Independent PE/SDK/source/old-DLL and numerical evidence audit for PC0602."""
import argparse
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha

def read(p):return json.loads(p.read_text(encoding='utf-8'))
def exports(path):
    data=path.read_bytes();pe=struct.unpack_from('<I',data,0x3c)[0];assert data[:2]==b'MZ' and data[pe:pe+4]==b'PE\0\0'
    machine,sections=struct.unpack_from('<HH',data,pe+4);assert machine==0x8664
    size=struct.unpack_from('<H',data,pe+20)[0];optional=pe+24;assert struct.unpack_from('<H',data,optional)[0]==0x20b
    export_rva=struct.unpack_from('<I',data,optional+112)[0];assert export_rva
    def offset(rva):
        for n in range(sections):
            at=optional+size+40*n;virtual_size,virtual_address,raw_size,raw_address=struct.unpack_from('<IIII',data,at+8)
            if virtual_address<=rva<virtual_address+max(virtual_size,raw_size):return raw_address+rva-virtual_address
        raise AssertionError('Unmapped export RVA')
    table=offset(export_rva);count=struct.unpack_from('<I',data,table+24)[0];names=offset(struct.unpack_from('<I',data,table+32)[0]);assert count<10000
    result=set()
    for n in range(count):
        at=offset(struct.unpack_from('<I',data,names+4*n)[0]);end=data.index(b'\0',at);assert end-at<1024;result.add(data[at:end].decode('ascii'))
    return result
def compare(actual,expected):
    assert len(actual)==len(expected) and all(len(a)==len(b)for a,b in zip(actual,expected))
    values=[(a,b)for left,right in zip(actual,expected)for a,b in zip(left,right)];assert all(math.isfinite(a) and abs(a-b)<=2e-6+1e-5*abs(b)for a,b in values)
    return max(abs(a-b)for a,b in values)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args();folder=args.folder.resolve();repo=Path(__file__).resolve().parents[1]
    external=Path(read(folder/'external-path.json')['external']).resolve();assert not external.is_relative_to(repo)
    phases={p:read(folder/(p+'-data.json'))for p in ('red-workspace','green','python-off')}
    bad={'PC-06.b optional native log API '+m for m in ('size','version','function','rejected')};red=phases['red-workspace'];assert red['status']=='failed' and red['failures']==4 and {r['test_name']for r in red['cases']if r['status']=='failed'}==bad
    assert all(d['status']=='passed' and d['failures']==0 and len(d['cases'])==10 for p,d in phases.items()if p!='red-workspace')
    assert red['dll_hashes']==phases['green']['dll_hashes']==phases['python-off']['dll_hashes']
    assert red['input_sha256']==phases['green']['input_sha256']==phases['python-off']['input_sha256']==sha(external/'audio/input.wav')
    legacy=read(folder.parent/'PC0601-native-log-queue/baseline-identity.json');old_dll=Path(legacy['artifacts'][1]['path']);assert sha(old_dll)==legacy['artifacts'][1]['sha256'] and 'audioalgo_node_log_api_v1' not in exports(old_dll)
    assert sha(repo/'sdk/include/audioalgo/node_abi_v1.h')==legacy['artifacts'][0]['sha256']
    assert red['host']['before_sha256']==red['host']['after_sha256']==legacy['artifacts'][3]['sha256']
    old_exports=exports(old_dll);assert 'audioalgo_node_api_v1' in old_exports
    packages={}
    for mode,hashes in red['dll_hashes'].items():
        package=external/'build'/mode;dll=package/'native_logging.dll';manifest=package/'manifest.json';assert sha(dll)==hashes['dll_sha256'] and sha(manifest)==hashes['manifest_sha256']
        names=exports(dll);assert 'audioalgo_node_api_v1' in names and ('audioalgo_node_log_api_v1' in names)==(mode!='absent')
        data=dll.read_bytes();marker=data.index(b'"schema_version":1');start=data.rfind(b'{',0,marker);end=data.index(b'\0',marker)
        assert json.loads(data[start:end].decode('utf-8'))==read(manifest)
        packages[mode]=dict(**hashes,exports=sorted(names))
    for source in (repo/'tests/sdk/native_log_plugin').iterdir():assert source.read_bytes()==(external/'consumer'/source.name).read_bytes()
    for name in ('node_abi_v1.h','node_log_abi_v1.h','log_queue_v1.hpp'):assert (repo/'sdk/include/audioalgo'/name).read_bytes()==(external/'sdk/include/audioalgo'/name).read_bytes()
    cache=(external/'build/CMakeCache.txt').read_text(encoding='utf-8').replace('\\','/').lower();assert str(repo).replace('\\','/').lower() not in cache
    compile_logs=list((external/'build').glob('log_*.dir/Release/*/CL.read.1.tlog'));assert len(compile_logs)==6
    for path in compile_logs:
        text=path.read_text(encoding='utf-16').replace('\\','/').lower();assert str(repo).replace('\\','/').lower() not in text and 'juce' not in text and str(external/'sdk/include/audioalgo/node_log_abi_v1.h').replace('\\','/').lower() in text
    maximum=0;outputs=[]
    for phase,d in phases.items():
        for row in d['cases']:
            if row['test_name'].startswith('PC-06.a/b actual'):
                path=Path(row['run']);record=read(path/'run.json');complete=read(path/'complete.json');rate,source=read_wav(external/'audio/input.wav');output_rate,output=read_wav(path/'output.wav');assert rate==output_rate==48000
                expected=[[f32(1.5*x+x+.00001*n)if row['mode']=='legacy'else f32(2*x)for n,x in enumerate(c)]for c in source];error=compare(output,expected);maximum=max(maximum,error)
                assert row['maximum_error']==error and row['output_sha256']==record['output_sha256']==complete['output_sha256']==sha(path/'output.wav')
                assert record['project_id']=='pc0602-'+row['mode'] and record['block_size']==127 and record['frames']==513 and record['valid_start_frame']==0 and record['valid_frame_count']==513
                for resource in record['resources']:assert resource['sha256']==resource['snapshot_sha256']==sha(external/'audio/input.wav')==sha(path/resource['snapshot_path'])
                outputs.append(dict(phase=phase,mode=row['mode'],run=str(path),request=record['request_id'],output_sha256=row['output_sha256'],maximum_error=error))
    for mode in ('good','absent','legacy'):assert len({o['output_sha256']for o in outputs if o['mode']==mode})==1
    env=read(folder/'legacy-component-environment.json');assert Path(env['package'])==old_dll.parent and env['dll_sha256']==sha(old_dll)
    junit=ET.parse(folder/'legacy-component.xml');assert len(junit.findall('.//testcase'))==2 and not junit.findall('.//failure') and not junit.findall('.//skipped')
    component=Path(env['output']);ra,a=read_wav(component/'a.wav');rb,b=read_wav(component/'b.wav');assert ra==rb==48000
    first=[[f32(1.5*x+y+.00001*n)for n,(x,y)in enumerate(zip(ac,bc))]for ac,bc in zip(a,b)]
    for port in ('main','side'):
        run=component/('component-'+port);rate,output=read_wav(run/'output.wav');assert rate==48000
        expected=[[f32(.5*x+y-.00002*n)if port=='main'else f32(x-y)for n,(x,y)in enumerate(zip(ac,bc))]for ac,bc in zip(first,b)];error=compare(output,expected);maximum=max(maximum,error);record=read(run/'run.json')
        assert len(record['plugins'])==2 and {p['node_id']for p in record['plugins']}=={'first','second'} and all(p['library_sha256']==sha(old_dll) and p['manifest_sha256']==sha(old_dll.parent/'manifest.json')for p in record['plugins'])
        outputs.append(dict(phase='legacy-component',mode=port,run=str(run),output_sha256=sha(run/'output.wav'),maximum_error=error))
    for phase,key in (('green','build-host'),('python-off','build-python-off')):
        d=phases[phase];built=read(folder/(key+'.json'));assert built['exit_code']==0
        assert d['host']['before_sha256']==d['host']['after_sha256']==sha(Path(d['host']['path']))==next(a['sha256']for a in built['artifacts']if Path(a['path']).name=='audio_cli.exe')
    report=dict(status='passed',packages=packages,old_dll_sha256=sha(old_dll),old_exports=sorted(old_exports),outputs=outputs,maximum_error=maximum,compiler_inputs=[dict(path=str(p),sha256=sha(p))for p in compile_logs],scope='optional entry table validation,immutable external DLLs,real old/new native CLI/component numerics only; no sink/drain/log UI')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',independent_wavs=len(outputs),maximum_error=maximum,real_old_v1=True)))
if __name__=='__main__':main()
