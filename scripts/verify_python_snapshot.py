"""Independent inspection of one recorded whole-run freeze/replay test execution."""
import argparse
import ast
import datetime as dt
import hashlib
import json
from pathlib import Path
import struct
from verify_python_mixed import f32,read_wav,sha,io_path

ROOT=Path(__file__).resolve().parents[1]
def snapshot(root):
    root=io_path(root)
    m=json.loads((root/'snapshot.json').read_text(encoding='utf-8'))
    actual={p.relative_to(root).as_posix() for p in root.rglob('*') if p.suffix=='.py' or p==root/'manifest.json'}
    assert actual==set(m['files'])
    for name,digest in m['files'].items():assert sha(root/name)==digest
    signature=b''.join(name.encode()+b'\0'+digest.encode()+b'\n' for name,digest in sorted(m['files'].items()))
    assert hashlib.sha256(signature).hexdigest()==m['content_sha256']==root.name
    return m['content_sha256']
def npy(path):
    raw=io_path(path).read_bytes();assert raw[:6]==b'\x93NUMPY'
    width=2 if raw[6]==1 else 4;size=int.from_bytes(raw[8:8+width],'little');offset=8+width
    h=ast.literal_eval(raw[offset:offset+size].decode('latin1').strip());assert h['descr']=='<f4' and not h['fortran_order']
    channels,frames=h['shape'];data=raw[offset+size:];assert len(data)==channels*frames*4
    samples=struct.unpack('<'+'f'*(channels*frames),data);return [samples[c*frames:(c+1)*frames] for c in range(channels)]
def close(actual,expected):
    assert len(actual)==len(expected) and all(len(a)==len(e) for a,e in zip(actual,expected));maximum=0
    for a,e in zip(actual,expected):
        for x,y in zip(a,e):
            error=abs(x-y);maximum=max(maximum,error);assert error<=2e-6+1e-5*abs(y),(x,y)
    return maximum
def inspect(run,source,factor=1,two=True):
    report=json.loads((run/'run.json').read_text(encoding='utf-8'));complete=json.loads((run/'complete.json').read_text(encoding='utf-8'))
    assert report['output_sha256']==complete['output_sha256']==sha(run/'output.wav')
    pre=[[f32(x*2) for x in c] for c in source]
    first=[[f32(f32(f32(x*f32(.75))*factor)+f32(f32(i)*f32(.00001))) for i,x in enumerate(c)] for c in pre]
    second=[[f32(f32(f32(x*f32(.25))*factor)+f32(f32(i)*f32(.00002))) for i,x in enumerate(c)] for c in first] if two else first
    expected=[[f32(x*.5) for x in c] for c in second];maximum=close(read_wav(run/'output.wav')[1],expected)
    processes=[];hashes=[]
    for artifact in report['task_artifacts']:
        assert artifact['path_base']=='run_root' and not Path(artifact['directory']).is_absolute()
        directory=run/artifact['directory'];p=artifact['provenance'];assert p['process_id']>0 and p['request_id']==report['request_id']
        assert sha(Path(p['environment']['executable']))==p['interpreter_sha256']
        snapshot(run/p['frozen_package_root']);snapshot(run/p['runner_snapshot'])
        for name,digest in p['code_files'].items():assert sha(directory/p['code_snapshot']/name)==digest
        target=first if p['node_id']=='python' else second
        for name,o in p['outputs'].items():
            path=directory/o['path'];assert sha(path)==o['sha256'];samples=npy(path)
            maximum=max(maximum,close(samples,target if name=='main' else [[-x for x in c] for c in target]))
        processes.append(p['process_id']);hashes.append(p['code_sha256'])
    assert len(set(hashes))==1
    return {'run':str(run),'request_id':report['request_id'],'process_ids':processes,'code_sha256':hashes[0],'output_sha256':report['output_sha256'],'maximum_absolute_error':maximum}
def main():
    p=argparse.ArgumentParser();p.add_argument('--command-record',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    command=json.loads(args.command_record.read_text(encoding='utf-8'));assert command['exit_code']==0
    start=dt.datetime.fromisoformat(command['started_utc']).timestamp();end=dt.datetime.fromisoformat(command['finished_utc']).timestamp()
    candidates=[r for pattern in ('mixed-freeze-*','mixed-replay-*') for r in (ROOT/'build/runs').glob(pattern) if start<=r.stat().st_mtime<=end]
    assert len(candidates)==4,[(str(r),r.stat().st_mtime) for r in candidates]
    checks=[]
    for root in candidates:
        if root.name.startswith('mixed-freeze-'):
            source=read_wav(root/'input.wav')[1];a=inspect(root/'v1',source);b=inspect(root/'v2',source,2)
            assert a['code_sha256']!=b['code_sha256'] and a['request_id']!=b['request_id'] and not set(a['process_ids'])&set(b['process_ids']);checks.extend((a,b))
        else:
            assert not (root/'plugin').exists() and not (root/'input.wav').exists()
            project=json.loads((root/'relocated/replay.aaproj').read_text(encoding='utf-8'));reference=next(n['parameters']['path'] for n in project['nodes'] if n['id']=='file')
            source=read_wav(root/'relocated'/reference)[1];a=inspect(root/'relocated',source,two=False);b=inspect(root/'replayed',source,two=False)
            assert a['output_sha256']==b['output_sha256'];checks.extend((a,b))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8') as stream:json.dump({'source_command':str(args.command_record),'checks':checks},stream,indent=2)
    print(json.dumps(checks,indent=2));return 0
if __name__=='__main__':raise SystemExit(main())
