"""Read real host WAV/NPY/provenance using a separate stdlib RIFF and float32 oracle."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import os
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]
def io_path(p):
    p=Path(p)
    if os.name!='nt':return p
    name=str(p.absolute())
    if name.startswith('\\\\?\\'):return p
    return Path('\\\\?\\UNC\\'+name[2:] if name.startswith('\\\\') else '\\\\?\\'+name)
def sha(p):return hashlib.sha256(io_path(p).read_bytes()).hexdigest()
def read_wav(p):
    raw=io_path(p).read_bytes();assert raw[:4]==b'RIFF' and raw[8:12]==b'WAVE'
    offset=12;fmt=None;data=None
    while offset+8<=len(raw):
        kind=raw[offset:offset+4];size=struct.unpack_from('<I',raw,offset+4)[0];chunk=raw[offset+8:offset+8+size]
        if kind==b'fmt ':fmt=struct.unpack_from('<HHIIHH',chunk)
        if kind==b'data':data=chunk
        offset+=8+size+(size%2)
    assert fmt and data is not None and fmt[0] in (3,65534) and fmt[5]==32
    values=struct.unpack('<'+'f'*(len(data)//4),data)
    return fmt[2],[values[i::fmt[1]] for i in range(fmt[1])]
def fixture(p):
    samples=[[f32(.125*math.sin((i+1)*(c+1)*.03)) for i in range(1025)] for c in range(2)]
    data=b''.join(struct.pack('<f',samples[c][i]) for i in range(1025) for c in range(2))
    fmt=struct.pack('<HHIIHH',3,2,48000,48000*8,8,32)
    payload=b'fmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(data))+data
    content=b'RIFF'+struct.pack('<I',len(payload)+4)+b'WAVE'+payload
    if p.exists():assert p.read_bytes()==content,'Existing demo input differs; refusing to overwrite'
    else:p.write_bytes(content)
    return samples
def verify(run,samples,scale=.75):
    rate,actual=read_wav(run/'output.wav');assert rate==48000 and len(actual)==2 and all(len(c)==1025 for c in actual)
    maximum=0
    for c in range(2):
        for i,x in enumerate(samples[c]):
            state=f32(f32(f32(x*2)*f32(scale))+f32(f32(i)*f32(.00001)))
            expected=f32(state*.5);error=abs(actual[c][i]-expected);maximum=max(maximum,error)
            assert error<=2e-6+1e-5*abs(expected),(c,i,actual[c][i],expected)
    report=json.loads((run/'run.json').read_text(encoding='utf-8'));complete=json.loads((run/'complete.json').read_text(encoding='utf-8'))
    assert report['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
    assert [t['kind'] for t in report['tasks']]==['source','native_region','offline','native_region','sink']
    for task in report['tasks']:
        assert all(d<task['index'] for d in task['dependencies'])
        for p in task['outputs']:
            assert p['frames']==1025 and p['dtype']=='float32' and p['sample_rate_hz']==48000 and p['channel_ids']==['ch0','ch1']
    assert len(report['task_artifacts'])==1
    task=report['task_artifacts'][0];directory=Path(task['directory']);provenance=task['provenance']
    if task.get('path_base')=='run_root':directory=run/directory
    for name,digest in provenance['code_files'].items():assert sha(directory/provenance['code_snapshot']/name)==digest
    for output in provenance['outputs'].values():assert sha(directory/output['path'])==output['sha256']
    lifecycle=json.loads((directory/'lifecycle.json').read_text());assert lifecycle=={'prepares':1,'calls':math.ceil(1025/report['block_size']),'frames':1025}
    return {'run':str(run),'run_id':report['run_id'],'maximum_absolute_error':maximum,'output_sha256':report['output_sha256'],'lifecycle':lifecycle}
def main():
    p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=False);example=ROOT/'examples/python_stateful_demo'
    samples=fixture(example/'input.wav');records=[]
    for host in (1,127,512):
        run=args.output/str(host);cmd=[str(args.cli.resolve()),'--plugin-dir',str(example/'plugin'),'run',str(example/'mixed.aaproj'),str(run.resolve()),str(host)]
        result=subprocess.run(cmd,cwd=ROOT,capture_output=True,timeout=30);(args.output/f'{host}.log').write_bytes(result.stdout+result.stderr)
        assert result.returncode==0,(result.returncode,(result.stdout+result.stderr).decode(errors='replace'))
        records.append(verify(run,samples))
    (args.output/'oracle.json').write_text(json.dumps({'scope':'mixed_chain_cli_subset','checks':records,'cli_sha256':sha(args.cli)},indent=2),encoding='utf-8')
    print(json.dumps(records,indent=2));return 0
if __name__=='__main__':raise SystemExit(main())
