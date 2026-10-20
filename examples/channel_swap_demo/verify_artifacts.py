"""Independent PCM16/float32 RIFF oracle for the component and CLI experiment."""
import hashlib, json, math, struct, sys
from pathlib import Path

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read_wav(path):
    data=path.read_bytes()
    assert data[:4]==b'RIFF' and data[8:12]==b'WAVE'
    offset,fmt,payload=12,None,None
    while offset+8<=len(data):
        kind,size=struct.unpack_from('<4sI',data,offset)
        block=data[offset+8:offset+8+size]
        if kind==b'fmt ': fmt=struct.unpack_from('<HHIIHH',block)
        elif kind==b'data': payload=block
        offset+=8+size+size%2
    assert fmt and payload and fmt[1]==2 and fmt[2]==48000
    if fmt[0]==1 and fmt[5]==16:
        samples=[v/32768 for v in struct.unpack('<'+'h'*(len(payload)//2),payload)]
    else:
        assert fmt[0]==3 and fmt[5]==32
        samples=struct.unpack('<'+'f'*(len(payload)//4),payload)
    assert len(samples)==1026
    return samples

def main():
    root=Path(sys.argv[1]).resolve()
    source=read_wav(root/'input.wav')
    recipe=json.loads((Path(__file__).resolve().parent/'demo_manifest.json').read_text(encoding='utf-8'))
    assert sha(root/'input.wav')==recipe['input_sha256']
    rows=[]
    for name,enabled in [('enabled',True),('disabled',False),('undo-enabled',True)]:
        snapshot=json.loads((root/(name+'.aaproj')).read_text(encoding='utf-8'))
        assert next(n for n in snapshot['nodes'] if n['id']=='swap')['parameters']['enabled'] is enabled
        for runner in ('component','cli'):
            folder=root/(runner+'-'+name)
            output=read_wav(folder/'output.wav')
            errors=[]
            for frame in range(513):
                for channel in range(2):
                    expected=source[frame*2+(1-channel if enabled else channel)]
                    actual=output[frame*2+channel]
                    error=abs(actual-expected)
                    assert math.isfinite(actual) and error<=2e-6+1e-5*abs(expected)
                    errors.append(error)
            manifest=json.loads((folder/'run.json').read_text(encoding='utf-8'))
            complete=json.loads((folder/'complete.json').read_text(encoding='utf-8'))
            executed=json.loads((folder/'project.aaproj').read_text(encoding='utf-8'))
            assert manifest['project_id']==snapshot['project_id'] and manifest['project_revision']==snapshot['revision']
            assert manifest['frames']==513 and manifest['channels']==2 and manifest['sample_rate_hz']==48000
            assert manifest['valid_start_frame']==0 and manifest['valid_frame_count']==513 and manifest['block_size']==127
            assert manifest['run_id']==manifest['request_id']==complete['run_id']
            assert sha(root/'input.wav')==manifest['input_sha256']
            assert sha(folder/'output.wav')==manifest['output_sha256']==complete['output_sha256']
            assert sha(folder/'project.aaproj')==manifest['project_snapshot_sha256']
            assert next(n for n in executed['nodes'] if n['id']=='swap')['parameters']['enabled'] is enabled
            for resource in manifest['resources']:
                assert sha(root/Path(resource['path']).name)==resource['sha256']
            rows.append({'name':runner+'-'+name,'run_id':manifest['run_id'],'revision':manifest['project_revision'],
                         'max_absolute_error':max(errors),'output_sha256':manifest['output_sha256']})
    assert len({r['run_id'] for r in rows})==6
    report={'status':'passed','scope':'independent artifact verification; no desktop claim',
            'oracle':'PCM16/32768 input, direct stereo index permutation, original float64 tolerance',
            'input_sha256':sha(root/'input.wav'),'outputs':rows}
    target=root.parent/(root.name+'-oracle.json')
    with target.open('x',encoding='utf-8') as stream: json.dump(report,stream,indent=2)
    print(json.dumps(report,indent=2))

if __name__=='__main__': main()
