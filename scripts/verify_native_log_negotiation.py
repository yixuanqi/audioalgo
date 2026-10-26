"""Black-box actual CLI negotiation of SDK-only DLLs plus independent WAV oracle."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests/integration'))
from verify_native_plugin import f32, read_wave, write_wave, sha

MODES=('good','size','version','function','rejected','absent')
NAMES={m:'PC-06.b optional native log API '+m for m in MODES}
OLD_CATALOG='PC-06.a genuine prebuilt v1 DLL catalog remains available'
NUMERIC={m:'PC-06.a/b actual '+m+' DLL numerical CLI output' for m in ('good','absent','legacy')}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--folder',type=Path,required=True);parser.add_argument('--phase',required=True);parser.add_argument('--cli',type=Path,required=True);args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1];folder=args.folder.resolve();assert folder.parent==repo/'build/reports/loops' and folder.name=='PC0602-log-negotiation'
    external=Path(json.loads((folder/'external-path.json').read_text())['external']).resolve();assert not external.is_relative_to(repo)
    phase=folder/args.phase;phase.mkdir(exist_ok=False);cli=args.cli.resolve();host_before=sha(cli)
    legacy_identity=json.loads((folder.parent/'PC0601-native-log-queue/baseline-identity.json').read_text())
    old_dll=Path(legacy_identity['artifacts'][1]['path']);old_package=old_dll.parent;assert sha(old_dll)==legacy_identity['artifacts'][1]['sha256']
    env=os.environ.copy();env.update(AUDIOALGO_AUTHORING_ROOT=str(external/'empty-authoring'),AUDIOALGO_PLUGIN_PATH=str(external/'empty-plugins'))
    rows=[];suite=ET.Element('testsuite',name='native optional logging DLL contracts')
    def command(name,argv):
        result=subprocess.run([str(a)for a in argv],cwd=external,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
        (phase/(name+'.stdout')).write_bytes(result.stdout);(phase/(name+'.stderr')).write_bytes(result.stderr)
        return result
    def case(name,row,errors):
        node=ET.SubElement(suite,'testcase',name=name,classname='SDK external DLL CLI',status='run')
        if errors:ET.SubElement(node,'failure',message='; '.join(errors)).text=json.dumps(row,ensure_ascii=False)
        ET.SubElement(node,'system-out').text=json.dumps(row,ensure_ascii=False)
        rows.append(dict(test_name=name,status='failed'if errors else'passed',errors=errors,**row))
    hashes={}
    for mode in MODES:
        package=external/'build'/mode;dll=package/'native_logging.dll';manifest=package/'manifest.json';hashes[mode]=dict(dll_sha256=sha(dll),manifest_sha256=sha(manifest))
        result=command('catalog-'+mode,[cli,'--list-nodes','--plugin-dir',package]);data=json.loads(result.stdout.decode('utf-8'));codes=[e['code']for e in data['diagnostics']];found=any(n['type_id']=='example.sdk_logging' for n in data['nodes']);builtin=any(n['type_id']=='builtin.gain' for n in data['nodes'])
        expected='plugin_log_api_rejected'if mode=='rejected'else'plugin_log_api_incompatible';invalid=mode not in ('good','absent');errors=[]
        if result.returncode!=(1 if invalid else 0):errors.append('wrong CLI exit code')
        if codes!=([expected]if invalid else[]):errors.append('wrong optional API diagnosis')
        if found==invalid:errors.append('partial registration or missing valid node')
        if not builtin:errors.append('builtin registry lost')
        case(NAMES[mode],dict(mode=mode,exit_code=result.returncode,codes=codes,node_registered=found,builtin_gain=builtin,package=str(package),**hashes[mode]),errors)
    result=command('catalog-legacy',[cli,'--list-nodes','--plugin-dir',old_package]);catalog=json.loads(result.stdout.decode('utf-8'));valid=result.returncode==0 and not catalog['diagnostics'] and any(n['type_id']=='example.external_stateful'for n in catalog['nodes']);case(OLD_CATALOG,dict(exit_code=result.returncode,old_dll=str(old_dll),old_dll_sha256=sha(old_dll)),[]if valid else['old genuine v1 DLL failed catalog'])
    audio=external/'audio';audio.mkdir(exist_ok=True);signal=[[f32(((3*n+7*c)%29-14)/64)for n in range(513)]for c in range(2)];input_file=audio/'input.wav'
    if not input_file.exists():write_wave(input_file,signal,48000)
    rate,source=read_wave(input_file);assert rate==48000 and source==[tuple(c)for c in signal]
    for mode in ('good','absent','legacy'):
        old=mode=='legacy';node_type='example.external_stateful'if old else'example.sdk_logging';package=old_package if old else external/'build'/mode
        project=dict(schema_version=1,project_id='pc0602-'+mode,name='SDK optional logging '+mode,revision=1,selected_output_node_id='native',selected_output_port_id='main'if old else'audio_out',nodes=[dict(id='file',type_id='builtin.file_source',parameters=dict(path='input.wav')),dict(id='native',type_id=node_type,parameters=dict(gain=1.5,bias=.00001)if old else{})],connections=[dict(from_node='file',from_port='audio',to_node='native',to_port='primary'if old else'audio_in')])
        if old:project['nodes'].append(dict(id='reference',type_id='builtin.file_source',parameters=dict(path='input.wav')));project['connections'].append(dict(from_node='reference',from_port='audio',to_node='native',to_port='reference'))
        project_file=audio/(mode+'.aaproj')
        if not project_file.exists():project_file.write_text(json.dumps(project,indent=2)+'\n')
        assert json.loads(project_file.read_text())==project
        destination=folder/'artifacts'/(args.phase+'-'+mode);destination.parent.mkdir(exist_ok=True);result=command('run-'+mode,[cli,'run',project_file,destination,'127','--plugin-dir',package]);errors=[];maximum=None;record={}
        if result.returncode!=0:errors.append('numerical CLI run failed')
        else:
            output_rate,output=read_wave(destination/'output.wav');expected=[[f32(1.5*x+x+.00001*n)if old else f32(2*x)for n,x in enumerate(c)]for c in signal]
            if output_rate!=48000 or len(output)!=2 or any(len(c)!=513 for c in output):errors.append('output format mismatch')
            else:
                maximum=max(abs(a-b)for actual,expect in zip(output,expected)for a,b in zip(actual,expect))
                if any(abs(a-b)>2e-6+1e-5*abs(b)for actual,expect in zip(output,expected)for a,b in zip(actual,expect)):errors.append('independent numerical oracle mismatch')
            record=json.loads((destination/'run.json').read_text());complete=json.loads((destination/'complete.json').read_text());dll=old_dll if old else package/'native_logging.dll'
            if record['output_sha256']!=sha(destination/'output.wav') or complete['output_sha256']!=record['output_sha256']:errors.append('output hash mismatch')
            if not(record['block_size']==127 and record['frames']==513 and record['valid_start_frame']==0 and record['valid_frame_count']==513):errors.append('run timeline mismatch')
            if len(record['plugins'])!=1 or record['plugins'][0]['library_sha256']!=sha(dll) or record['plugins'][0]['manifest_sha256']!=sha(package/'manifest.json'):errors.append('DLL/provenance hash mismatch')
        case(NUMERIC[mode],dict(mode=mode,exit_code=result.returncode,run=str(destination),maximum_error=maximum,output_sha256=record.get('output_sha256')),errors)
    assert sha(old_dll)==legacy_identity['artifacts'][1]['sha256']
    for mode in MODES:assert sha(external/'build'/mode/'native_logging.dll')==hashes[mode]['dll_sha256']
    assert sha(cli)==host_before
    failures=sum(r['status']=='failed'for r in rows);suite.set('tests',str(len(rows)));suite.set('failures',str(failures));suite.set('errors','0');suite.set('skipped','0');ET.ElementTree(suite).write(folder/(args.phase+'.xml'),encoding='utf-8',xml_declaration=True)
    report=dict(status='passed'if not failures else'failed',phase=args.phase,cases=rows,failures=failures,host=dict(path=str(cli),before_sha256=host_before,after_sha256=sha(cli)),external=str(external),input_sha256=sha(input_file),dll_hashes=hashes,old_dll_sha256=sha(old_dll),scope='actual optional table negotiation and native CLI calculation only; no bound sink/worker log archive/UI claim')
    (folder/(args.phase+'-data.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status=report['status'],cases=len(rows),failures=failures)))
    raise SystemExit(1 if failures else 0)
if __name__=='__main__':main()
