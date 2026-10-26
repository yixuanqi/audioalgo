"""Inspect PC0506 Red/Green source diagnostics and real WAV/control artifacts."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET
from verify_python_mixed import f32, read_wav, sha
from verify_python_snapshot import close

NAME='PC-05.b mismatched mixed sources identify their output port before any factory'


def inspect(junit, red=False):
    case=next(c for c in ET.parse(junit).iter('testcase') if c.get('name')==NAME)
    failures=case.findall('failure')
    if red:
        assert len(failures)==5 and all(f.get('message')=='error->port_id=="audio"' for f in failures)
    else:
        assert not failures and case.find('error') is None
    root=Path(next(s.split('=',1)[1] for s in case.findtext('system-out','').splitlines() if s.startswith('PC05_SOURCE_ROOT='))).resolve()
    managed=Path(__file__).resolve().parents[1]/'build/reports/loops/PC0506-source-port/artifacts';assert root.parent==managed
    project=json.loads((root/'project.aaproj').read_text(encoding='utf-8'));assert project['selected_output_node_id']=='join' and project['selected_output_port_id']=='main'
    assert {(c['from_node'],c['from_port'],c['to_node'],c['to_port']) for c in project['connections']}=={('a','audio','join','primary'),('b','audio','join','reference')}
    rate,a=read_wav(root/'a.wav');assert rate==48000 and len(a)==2 and all(len(c)==513 for c in a)
    maximum=close(a,[[f32(((3*n+7*c)%29-14)/64) for n in range(513)] for c in range(2)])
    rows=json.loads((root/'source-probe.json').read_text(encoding='utf-8'));assert len(rows)==5 and {r['kind'] for r in rows}=={'rate','channels','channel_ids','frames','interval'}
    profiles={'rate':(16000,2,513,['left','right'],0,513),'channels':(48000,1,513,['mono'],0,513),'channel_ids':(48000,2,513,['right','left'],0,513),'frames':(48000,2,512,['left','right'],0,512),'interval':(48000,2,513,['left','right'],1,512)}
    for row in rows:
        assert row['code']=='source_format_mismatch' and row['node']=='b' and row['port']==('' if red else 'audio') and row['factories']==0
        r,ch,frames,ids,start,count=profiles[row['kind']];assert (row['rate'],row['channels'],row['frames'],row['channel_ids'],row['valid_start'],row['valid_count'])==(r,ch,frames,ids,start,count)
        path=root/(row['kind']+'.wav');actual_rate,b=read_wav(path);assert actual_rate==r and len(b)==ch and all(len(c)==frames for c in b) and sha(path)==row['input_sha256']
        maximum=max(maximum,close(b,[[f32(((11*n+5*c)%31-15)/128) for n in range(frames)] for c in range(ch)]))
    output_rate,output=read_wav(root/'control.wav');assert output_rate==48000;maximum=max(maximum,close(output,[[f32(x*2) for x in c] for c in a]))
    return dict(root=str(root),rows=rows,maximum_error=maximum,control_sha256=sha(root/'control.wav'),a_sha256=sha(root/'a.wav'))


def main():
    parser=argparse.ArgumentParser();parser.add_argument('junit',type=Path);parser.add_argument('--red',type=Path,required=True);parser.add_argument('--report',type=Path,required=True);args=parser.parse_args()
    red=inspect(args.red,True);green=inspect(args.junit)
    assert red['a_sha256']==green['a_sha256'] and red['control_sha256']==green['control_sha256']
    assert {r['kind']:r['input_sha256'] for r in red['rows']}=={r['kind']:r['input_sha256'] for r in green['rows']}
    report=dict(status='passed',red=red,green=green,maximum_error=max(red['maximum_error'],green['maximum_error']),scope='worker mixed source preflight and control only; labels/intervals are actual binding receipts, WAV cannot encode those fields; not GUI/CLI/real Python execution')
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(status='passed',red_missing_ports=5,green_exact_ports=5,maximum_error=report['maximum_error'])))


if __name__=='__main__':main()
