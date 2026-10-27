"""Offline block/clip execution in the dedicated Python process."""
from __future__ import annotations
from pathlib import Path
from types import MappingProxyType
from typing import Any,Callable
import hashlib
import json
import os
import platform
import shutil
import sys
import tempfile
import uuid
import numpy as np
from .manifest import discover,identifier,text,validate_parameters
from .sdk import AudioPort,Context,DiagnosticError,Cancelled
from .snapshot import freeze_code
from .source_loader import load_class

DEFAULT_PAYLOAD_BYTES=64*1024*1024

def filesystem_root(path:Path)->Path:
    """Use the Windows filesystem namespace for all descendants, including NPY files."""
    resolved=path.resolve()
    if os.name!='nt':return resolved
    name=str(resolved)
    if name.startswith('\\\\?\\'):return resolved
    return Path('\\\\?\\UNC\\'+name[2:] if name.startswith('\\\\') else '\\\\?\\'+name)

def sha(path:Path)->str:
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda:stream.read(65536),b''):digest.update(data)
    return digest.hexdigest()

def fail(code:str,message:str,port:str='')->None:raise DiagnosticError(code,message,port)

def checked(ok:bool,code:str,message:str,port:str='')->None:
    if not ok:fail(code,message,port)

def header(path:Path,limit:int,port:str)->tuple[int,int]:
    # Parse the bounded NPY header and physical byte count before allocating samples.
    with path.open('rb') as stream:
        version=np.lib.format.read_magic(stream)
        checked(version in ((1,0),(2,0)),'python_payload_format','Unsupported NPY header version',port)
        reader=np.lib.format.read_array_header_1_0 if version==(1,0) else np.lib.format.read_array_header_2_0
        shape,fortran,dtype=reader(stream,max_header_size=10000);offset=stream.tell()
    checked(len(shape)==2 and all(type(x) is int and x>=0 for x in shape),'python_input_shape_invalid','Expected [channels,samples]',port)
    checked(dtype==np.dtype(np.float32) and not fortran,'python_input_dtype_invalid','Expected C-order float32; implicit conversion is disabled',port)
    channels,frames=shape
    checked(channels in (1,2,4),'python_input_channels_invalid','Only1/2/4 PCM channels are supported',port)
    size=channels*frames*4
    checked(size<=limit,'python_payload_limit','Payload exceeds the configured byte budget before allocation',port)
    checked(path.stat().st_size==offset+size,'python_payload_size_mismatch','Physical payload size disagrees with NPY shape',port)
    return channels,frames

class Engine:
    def __init__(self,plugin_roots:list[Path],run_root:Path,max_payload_bytes:int=DEFAULT_PAYLOAD_BYTES):
        checked(type(max_payload_bytes) is int and max_payload_bytes>0,'python_payload_limit','Payload budget must be positive')
        self.catalog=discover([filesystem_root(p) for p in plugin_roots]);self.nodes={n.type_id:n for n in self.catalog.nodes}
        self.run_root=filesystem_root(run_root);self.run_root.mkdir(parents=True,exist_ok=True)
        self.max_payload_bytes=max_payload_bytes;self.loaded_hashes={}

    def confined(self,name:Any)->Path:
        checked(isinstance(name,str) and bool(name),'python_payload_path_invalid','Payload path must be relative to the run root')
        path=Path(name);checked(not path.is_absolute(),'python_payload_path_invalid','Absolute payload paths are forbidden')
        target=(self.run_root/path).resolve()
        checked(self.run_root in target.parents,'python_payload_path_invalid','Payload escapes the run root')
        checked(target.is_file(),'python_payload_missing','Input payload is missing')
        return target

    def load_class(self,info)->tuple[type,Path,str,dict[str,str]]:
        snapshot,digest,files=freeze_code(info.root,self.run_root/'python-code')
        checked(files['manifest.json']==info.manifest_sha256,'python_restart_required','Manifest changed; rediscover and restart the runner')
        previous=self.loaded_hashes.get(info.plugin_id)
        checked(previous is None or previous==digest,'python_restart_required','Code changed; restart the runner instead of reloading modules')
        self.loaded_hashes[info.plugin_id]=digest
        manifest=json.loads((snapshot/'manifest.json').read_bytes())
        if 'authoring_profile' in manifest:
            from .workspace_function import node_class
            cls=node_class(info,snapshot,digest,files,manifest['authoring_profile'])
        else:
            cls=load_class(info.plugin_id,digest,snapshot,files,info.descriptor['entrypoint'])
        checked(callable(getattr(cls,'describe',None)) and cls.describe()==info.descriptor,
                'python_descriptor_mismatch','Loaded class descriptor disagrees with the frozen manifest')
        return cls,snapshot,digest,files

    def execute(self,message:dict[str,Any],cancelled:Callable[[],bool]=lambda:False)->dict[str,Any]:
        if cancelled():raise Cancelled()
        try:
            node_id=text(message.get('node_id'),'node_id');type_id=identifier(message.get('type_id'),'type_id')
            checked(type(message.get('revision')) is int and message['revision']>=0 and
                    type(message.get('session_epoch',0)) is int and message.get('session_epoch',0)>=0,
                    'python_request_invalid','Revision and session epoch must be nonnegative integers')
            checked(type_id in self.nodes,'node_type_missing','Python node type is not registered')
            info=self.nodes[type_id];desc=info.descriptor
            checked(all(p['type']=='audio.pcm@1' for p in desc['inputs']+desc['outputs']),
                    'python_port_type_unsupported','This runner profile executes PCM ports only')
            parameters=validate_parameters(desc['parameters'],message.get('parameters',{}))
            block=message.get('block_frames',512)
            checked(type(block) is int and 0<block<=65536,'python_block_size_invalid','block_frames must be1..65536')
            inputs=message.get('inputs');checked(isinstance(inputs,dict) and set(inputs)=={p['id'] for p in desc['inputs']},'python_input_ports_invalid','Input ports disagree with the descriptor')
            plans=[];total=0;reference=None
            for port,value in inputs.items():
                checked(isinstance(value,dict),'python_input_metadata_invalid','Input metadata must be an object',port)
                path=self.confined(value.get('path'));channels,frames=header(path,self.max_payload_bytes,port)
                checked(value.get('shape')==[channels,frames] and value.get('dtype')=='float32',
                        'python_input_shape_mismatch','Sidecar shape/dtype disagrees with the actual payload',port)
                rate=value.get('sample_rate_hz');ids=value.get('channel_ids');valid=value.get('valid_interval')
                checked(type(rate) is int and rate in desc['format']['sample_rates_hz'] and channels in desc['format']['channels'],'python_input_format_invalid','Unsupported input format',port)
                checked(isinstance(ids,list) and len(ids)==channels and all(isinstance(x,str) and x for x in ids) and len(set(ids))==channels,'python_input_channels_invalid','Channel IDs must be unique and match the payload',port)
                checked(isinstance(valid,list) and len(valid)==2 and all(type(x) is int and x>=0 for x in valid) and valid[0]<=frames and valid[1]<=frames-valid[0],'python_input_interval_invalid','Valid interval is outside the input',port)
                digest=value.get('sha256');checked(isinstance(digest,str) and len(digest)==64,'python_payload_hash_invalid','SHA256 is required',port)
                shape=(channels,frames,rate,tuple(ids),tuple(valid))
                checked(reference is None or reference==shape,'python_input_format_mismatch','PCM inputs must share an explicit clock, channel IDs and interval',port)
                reference=shape;total+=channels*frames*4;plans.append((port,path,digest))
            channels,frames,rate,ids,valid=reference
            total+=len(desc['outputs'])*channels*frames*4
            checked(total<=self.max_payload_bytes,'python_payload_limit','Aggregate input/output budget exceeded before sample allocation')
        except ValueError as error:raise DiagnosticError('python_request_invalid',str(error)) from error
        work=Path(tempfile.mkdtemp(prefix='python-work-',dir=self.run_root));instance=None;reason='run_failed'
        context=Context(node_id,str(message.get('run_id','')),str(message.get('request_id','')),message['revision'],
                        str(message.get('project_id','')),message.get('session_epoch',0),
                        MappingProxyType(parameters),self.run_root,block,frames,0,cancelled)
        try:
            owned={};input_records={}
            for port,path,digest in plans:
                name='port-'+hashlib.sha256(port.encode()).hexdigest()+'.npy'
                captured=work/'inputs'/name;captured.parent.mkdir(exist_ok=True)
                with path.open('rb') as source,captured.open('xb') as target:
                    remaining=self.max_payload_bytes+10001
                    while True:
                        context.check_cancelled();data=source.read(min(65536,remaining+1))
                        if not data:break
                        remaining-=len(data);checked(remaining>=0,'python_payload_limit','Input grew beyond its byte budget',port);target.write(data)
                checked(sha(captured)==digest,'python_payload_hash_mismatch','Captured payload does not match its declared SHA256',port)
                checked(header(captured,self.max_payload_bytes,port)==(channels,frames),'python_input_shape_invalid','Captured shape changed',port)
                samples=np.load(captured,allow_pickle=False)
                checked(bool(np.isfinite(samples).all()),'python_input_non_finite','Input contains NaN or infinity',port)
                samples.flags.writeable=False;owned[port]=AudioPort(samples,rate,ids,valid)
                input_records[port]={'sha256':digest,'file':'inputs/'+name}
            context.check_cancelled();cls,snapshot,digest,code_files=self.load_class(info);instance=cls();instance.prepare(context)
            outputs={p['id']:np.empty((channels,frames),dtype=np.float32) for p in desc['outputs']}
            kind=desc['execution']['kind'];offsets=[0] if kind=='clip' else range(0,frames,block)
            # Empty block streams still prepare/reset once, with no invented sample call.
            for offset in offsets:
                context.check_cancelled();count=frames if kind=='clip' else min(block,frames-offset);ctx=context.block(offset,count)
                views={}
                for port,value in owned.items():
                    first=min(count,max(0,valid[0]-offset));end=min(count,max(0,valid[0]+valid[1]-offset))
                    sample=value.samples[:,offset:offset+count].copy();sample.flags.writeable=False
                    views[port]=AudioPort(sample,rate,ids,(first,max(0,end-first)))
                method=instance.process_clip if kind=='clip' else instance.process
                produced=method(MappingProxyType(views),ctx);context.check_cancelled()
                checked(isinstance(produced,dict) and set(produced)==set(outputs),'python_output_ports_invalid','Output ports disagree with the manifest')
                for port,value in produced.items():
                    checked(isinstance(value,AudioPort) and isinstance(value.samples,np.ndarray),'python_output_type_invalid','Output must be AudioPort containing ndarray',port)
                    checked(value.samples.dtype==np.float32,'python_output_dtype_invalid','Output must be float32; implicit conversion is disabled',port)
                    checked(value.samples.shape==(channels,count),'python_output_shape_invalid','Output has the wrong channel/sample shape',port)
                    checked(value.sample_rate_hz==rate and tuple(value.channel_ids)==ids,'python_output_format_invalid','Output clock/channel IDs changed',port)
                    checked(tuple(value.valid_interval)==next(iter(views.values())).valid_interval,'python_output_interval_invalid','Output valid interval changed in this PCM profile',port)
                    checked(bool(np.isfinite(value.samples).all()),'python_output_non_finite','Output contains NaN or infinity',port)
                    outputs[port][:,offset:offset+count]=value.samples
            context.check_cancelled();output_records={}
            for port,samples in outputs.items():
                name='port-'+hashlib.sha256(port.encode()).hexdigest()+'.npy'
                path=work/'outputs'/name;path.parent.mkdir(exist_ok=True);np.save(path,samples,allow_pickle=False)
                checked(header(path,self.max_payload_bytes,port)==(channels,frames),'python_output_shape_invalid','Materialized output is invalid',port)
                output_records[port]={'path':'outputs/'+name,'sha256':sha(path),'shape':[channels,frames],'byte_size':path.stat().st_size,
                    'sample_rate_hz':rate,'channel_ids':list(ids),'valid_interval':list(valid),'dtype':'float32'}
            environment={'python':platform.python_version(),'numpy':np.__version__,'executable':sys.executable,'platform':platform.platform()}
            result={'outputs':output_records,'inputs':input_records,'code_sha256':digest,'code_files':code_files,
                'path_base':'result_directory','code_snapshot_base':'run_root',
                'code_snapshot':str(snapshot.relative_to(self.run_root)),'environment':environment,
                'environment_sha256':hashlib.sha256(json.dumps(environment,sort_keys=True).encode()).hexdigest()}
            (work/'result.json').write_text(json.dumps(result,sort_keys=True,allow_nan=False),encoding='utf-8')
            instance.reset('run_complete');instance=None;context.check_cancelled()
            final=self.run_root/('python-result-'+uuid.uuid4().hex);os.replace(work,final);reason='run_complete'
            for record in output_records.values():record['path']=(final.relative_to(self.run_root)/record['path']).as_posix()
            result['path_base']='run_root'
            result['result_manifest']=(final.relative_to(self.run_root)/'result.json').as_posix()
            return result
        except Cancelled:reason='run_cancelled';raise
        finally:
            if instance is not None:
                try:instance.reset(reason)
                except BaseException:pass
            if work.exists():shutil.rmtree(work)
