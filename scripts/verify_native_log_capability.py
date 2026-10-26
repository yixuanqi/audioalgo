"""Independently bind real native capability captions/catalogs and actual old/new DLL audio."""
import argparse,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32,read_wav,sha
from verify_native_log_artifacts import exports

NAME='PC-06.a real legacy and SDK DLL nodes explain log capability in library Inspector and fresh CLI'
def load(p):return json.loads(p.read_text(encoding='utf-8'))
def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);parser.add_argument('--report',type=Path,required=True);parser.add_argument('--phase-prefix',default='capabilities');args=parser.parse_args();folder=args.folder.resolve();repo=Path(__file__).resolve().parents[1]
    outputs=[];roots=[]
    baseline=load(folder.parent/'PC0601-native-log-queue/baseline-identity.json');old=Path(baseline['artifacts'][1]['path']);assert sha(old)==baseline['artifacts'][1]['sha256'] and sha(repo/'sdk/include/audioalgo/node_abi_v1.h')==baseline['artifacts'][0]['sha256']
    assert 'audioalgo_node_log_api_v1' not in exports(old)
    for record in ['direct-components','off-components']:
        command=load(folder/(record+'.json'));case=next(c for c in ET.parse(folder/(record+'.xml')).iter('testcase')if c.get('name')==NAME);assert not case.findall('failure') and not case.findall('error') and not case.findall('skipped')
        root=Path(next(s.split('=',1)[1]for s in case.findtext('system-out','').splitlines()if s.startswith('PC0605_CAP_ROOT='))).resolve();assert root.parent==repo/'build/reports/loops/PC0605-native-capability/artifacts';roots.append(str(root))
        rows=load(root/'capability-demo.json');assert [r['mode']for r in rows]==['legacy','absent','good']
        for row in rows:
            mode=row['mode'];dll=Path(row['dll']);assert sha(dll)==row['dll_sha256'];expected='sdk_v1'if mode=='good'else'unavailable';caption='User logs: available.'if mode=='good'else'User logs: unavailable in this version.'
            assert row['capability']==expected and caption in row['library_tooltip'] and caption in row['inspector_text'] and 'ID capability-node'in row['inspector_text']
            assert ('audioalgo_node_log_api_v1'in exports(dll))==(mode=='good')
            catalog=load(root/('cli-'+mode+'.json'));nodes=[n for n in catalog['nodes']if n.get('library_sha256')==sha(dll)];assert len(nodes)==1 and nodes[0]['backend_descriptor']['user_logs']==expected
            if mode=='legacy':assert dll==old
    maximum=0
    for phase,command_name in [(args.phase_prefix+'-green','sdk-cli-on'),(args.phase_prefix+'-off','sdk-cli-off')]:
        parent=folder.parent/'PC0602-log-negotiation';data=load(parent/(phase+'-data.json'));receipt=load(folder/(command_name+'.json'));assert data['status']=='passed' and data['failures']==0 and len(data['cases'])==10
        assert data['host']['before_sha256']==data['host']['after_sha256']==next(a['sha256']for a in receipt['artifacts']if Path(a['path']).name=='audio_cli.exe')
        rate,source=read_wav(Path(data['external'])/'audio/input.wav');assert rate==48000
        for row in data['cases']:
            if not row['test_name'].startswith('PC-06.a/b actual'):continue
            run=Path(row['run']);out_rate,output=read_wav(run/'output.wav');assert out_rate==48000
            expected=[[f32(1.5*x+x+.00001*n)if row['mode']=='legacy'else f32(2*x)for n,x in enumerate(c)]for c in source]
            assert len(output)==2 and all(len(c)==513 for c in output)
            for actual,wanted in zip(output,expected):
                for a,b in zip(actual,wanted):assert math.isfinite(a)and abs(a-b)<=2e-6+1e-5*abs(b);maximum=max(maximum,abs(a-b))
            record=load(run/'run.json');assert sha(run/'output.wav')==record['output_sha256']==load(run/'complete.json')['output_sha256']
            if row['mode']=='legacy':assert record['plugins'][0]['library_sha256']==sha(old)
            outputs.append(dict(phase=phase,mode=row['mode'],run=str(run),output_sha256=record['output_sha256']))
    env=load(folder/'legacy-component-environment.json');assert Path(env['package'])==old.parent and env['dll_sha256']==sha(old);legacy=Path(env['output']);ra,a=read_wav(legacy/'a.wav');rb,b=read_wav(legacy/'b.wav');assert ra==rb==48000
    first=[[f32(1.5*x+y+.00001*n)for n,(x,y)in enumerate(zip(c,d))]for c,d in zip(a,b)]
    for port in ['main','side']:
        run=legacy/('component-'+port);rate,output=read_wav(run/'output.wav');assert rate==48000
        expected=[[f32(.5*x+y-.00002*n)if port=='main'else f32(x-y)for n,(x,y)in enumerate(zip(c,d))]for c,d in zip(first,b)]
        for actual,wanted in zip(output,expected):
            for x,y in zip(actual,wanted):assert math.isfinite(x)and abs(x-y)<=2e-6+1e-5*abs(y);maximum=max(maximum,abs(x-y))
        assert all(p['library_sha256']==sha(old)for p in load(run/'run.json')['plugins']);outputs.append(dict(phase='legacy-component',mode=port,run=str(run),output_sha256=sha(run/'output.wav')))
    assert len(outputs)==8
    report=dict(status='passed',roots=roots,actual_wavs=8,outputs=outputs,maximum_error=maximum,legacy_dll_sha256=sha(old),old_c_header_sha256=sha(repo/'sdk/include/audioalgo/node_abi_v1.h'),scope='actual library/Inspector/CLI capability for legacy,absent and SDK DLL on ON/OFF plus8 actual native numerical outputs; no desktop/callback budget/lifecycle/workbench restart claim')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',actual_wavs=8,maximum_error=maximum)))
if __name__=='__main__':main()
