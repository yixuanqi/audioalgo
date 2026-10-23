"""Bounded, data-only Python plugin discovery. Never imports a plugin module."""
from __future__ import annotations
from dataclasses import dataclass,field
from pathlib import Path
import hashlib
import json
import math
import re
from typing import Any

MAX_MANIFEST_BYTES=1024*1024
IDENTIFIER=re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}\Z')
ENTRYPOINT=re.compile(r'[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*:[A-Za-z_][A-Za-z0-9_]*\Z')

@dataclass(frozen=True)
class PluginDiagnostic:
    code:str
    message:str
    manifest:Path
    plugin_id:str=''

@dataclass(frozen=True)
class PythonNode:
    plugin_id:str
    plugin_version:str
    type_id:str
    root:Path
    manifest_sha256:str
    descriptor:dict[str,Any]

@dataclass
class Discovery:
    nodes:list[PythonNode]=field(default_factory=list)
    diagnostics:list[PluginDiagnostic]=field(default_factory=list)

def require(ok:bool,message:str)->None:
    if not ok:raise ValueError(message)

def text(value:Any,name:str)->str:
    require(isinstance(value,str) and 0<len(value)<=1024,name+' must be a nonempty bounded string')
    return value

def identifier(value:Any,name:str)->str:
    require(isinstance(value,str) and IDENTIFIER.fullmatch(value) is not None,name+' is invalid')
    return value

def array(value:Any,name:str,limit:int=128,nonempty:bool=False)->list[Any]:
    require(isinstance(value,list) and len(value)<=limit and (bool(value) or not nonempty),name+' must be a bounded array')
    return value

def numeric(value:Any)->bool:
    if type(value) not in (int,float):return False
    try:return math.isfinite(value)
    except OverflowError:return False

def validate_parameters(parameters:list[dict[str,Any]],values:dict[str,Any])->dict[str,Any]:
    require(isinstance(values,dict),'Parameter values must be an object')
    names={p['id'] for p in parameters};require(set(values)<=names,'Unknown parameter')
    actual={}
    for p in parameters:
        value=values.get(p['id'],p['default']);kind=p['type']
        valid=(type(value) is bool if kind=='bool' else type(value) is int if kind=='int' else numeric(value) if kind=='float' else isinstance(value,str))
        require(valid,'Parameter '+p['id']+' has the wrong type or a non-finite value')
        if kind=='int':require(-(2**63)<=value<2**63,'Integer parameter exceeds int64')
        if kind in ('int','float'):
            require(('minimum' not in p or value>=p['minimum']) and ('maximum' not in p or value<=p['maximum']),'Parameter '+p['id']+' is out of range')
        if kind=='enum':require(value in {c['value'] for c in p['choices']},'Parameter '+p['id']+' is not a declared choice')
        actual[p['id']]=value
    return actual

def validate_node(n:Any)->None:
    require(isinstance(n,dict),'Node must be an object')
    identifier(n.get('type_id'),'type_id');text(n.get('version'),'version');text(n.get('display_name'),'display_name')
    require(isinstance(n.get('entrypoint'),str) and ENTRYPOINT.fullmatch(n['entrypoint']) is not None,'Invalid package-relative entrypoint')
    require(len(n['entrypoint'])<=1024,'Entrypoint exceeds 1024 characters')
    execution=n.get('execution');require(isinstance(execution,dict),'execution must be an object')
    require(execution.get('kind') in ('block','clip'),'execution.kind must be block or clip')
    require(type(execution.get('stateful',False)) is bool,'stateful must be boolean')
    require(execution.get('realtime_verified',False) is False,'Python realtime execution is unsupported')
    ports=set()
    for direction in ('inputs','outputs'):
        for p in array(n.get(direction),direction,64,True):
            require(isinstance(p,dict),'Port must be an object');name=identifier(p.get('id'),'port.id')
            require(name not in ports,'Port IDs must be unique within a node');ports.add(name)
            text(p.get('type'),'port.type');require(type(p.get('required',False)) is bool,'required must be boolean')
    fmt=n.get('format');require(isinstance(fmt,dict),'format must be an object')
    for name in ('sample_rates_hz','channels'):
        values=array(fmt.get(name),'format.'+name,32,True)
        require(all(type(x) is int and 0<x<2**31 for x in values) and len(values)==len(set(values)),'Format values must be unique positive integers')
    parameters=array(n.get('parameters'),'parameters');names=set()
    for p in parameters:
        require(isinstance(p,dict),'Parameter must be an object');name=identifier(p.get('id'),'parameter.id')
        require(name not in names,'Duplicate parameter');names.add(name)
        require(p.get('type') in ('bool','int','float','enum','string','path'),'Unsupported parameter type')
        require(p.get('update_policy','OfflineOnly')=='OfflineOnly','Only OfflineOnly parameter updates are supported')
        require(type(p.get('input_resource',False)) is bool,'input_resource must be boolean')
        require(not p.get('input_resource',False) or p['type']=='path','Input resources require path parameters')
        for bound in ('minimum','maximum','step'):
            if bound in p:
                require(p['type'] in ('int','float') and numeric(p[bound]),'Parameter bounds require finite numeric parameters')
                if p['type']=='int':require(p[bound]==math.trunc(p[bound]),'Integer bounds and steps must be whole numbers')
        if 'step' in p:require(p['step']>0,'Parameter step must be positive')
        if 'minimum' in p and 'maximum' in p:require(p['minimum']<=p['maximum'],'Parameter bounds are reversed')
        if p['type']=='enum':
            choices=array(p.get('choices'),'choices',128,True);seen=set()
            for c in choices:
                require(isinstance(c,dict),'Choice must be an object');value=text(c.get('value'),'choice.value')
                text(c.get('label'),'choice.label');require(value not in seen,'Duplicate choice');seen.add(value)
        else:require(not p.get('choices',[]),'Only enum parameters may declare choices')
        require('default' in p,'Parameter default is required')
    validate_parameters(parameters,{})

def load_manifest(path:Path)->tuple[str,str,list[dict[str,Any]],str]:
    with path.open('rb') as stream:raw=stream.read(MAX_MANIFEST_BYTES+1)
    require(len(raw)<=MAX_MANIFEST_BYTES,'Manifest exceeds 1 MiB')
    def constant(value:str):raise ValueError('Non-finite JSON constant '+value)
    value=json.loads(raw.decode('utf-8'),parse_constant=constant)
    require(isinstance(value,dict),'Manifest must be an object')
    require(type(value.get('schema_version')) is int and value['schema_version']==1,'Unsupported manifest schema')
    require(value.get('runtime')=='python','Manifest runtime must be python')
    plugin_id=identifier(value.get('plugin_id'),'plugin_id');version=text(value.get('plugin_version'),'plugin_version')
    nodes=array(value.get('nodes'),'nodes',64,True);seen=set()
    for node in nodes:
        validate_node(node);require(node['type_id'] not in seen,'Duplicate node in package');seen.add(node['type_id'])
    return plugin_id,version,nodes,hashlib.sha256(raw).hexdigest()

def discover(roots:list[Path])->Discovery:
    result=Discovery();paths=set();types=set();plugins=set()
    for raw in roots:
        root=Path(raw)
        if not root.is_dir():continue
        candidates=[root/'manifest.json']+[p/'manifest.json' for p in sorted(root.iterdir()) if p.is_dir() and not p.is_symlink()]
        for path in candidates:
            if path.is_file():paths.add(path.resolve())
    for path in sorted(paths):
        plugin_id=''
        try:
            # Shared plugin roots can contain native manifests; they are not Python packages.
            with path.open('rb') as stream:prefix=stream.read(MAX_MANIFEST_BYTES+1)
            if len(prefix)<=MAX_MANIFEST_BYTES:
                marker=json.loads(prefix.decode('utf-8'))
                if isinstance(marker,dict) and marker.get('runtime')!='python':continue
            plugin_id,version,nodes,digest=load_manifest(path)
            if plugin_id in plugins:
                result.diagnostics.append(PluginDiagnostic('python_plugin_duplicate','Duplicate plugin ID',path,plugin_id));continue
            if any(n['type_id'] in types for n in nodes):
                result.diagnostics.append(PluginDiagnostic('python_node_duplicate','Duplicate node type ID',path,plugin_id));continue
            result.nodes.extend(PythonNode(plugin_id,version,n['type_id'],path.parent,digest,n) for n in nodes)
            plugins.add(plugin_id);types.update(n['type_id'] for n in nodes)
        except (ValueError,OSError,TypeError,KeyError,UnicodeError) as error:
            result.diagnostics.append(PluginDiagnostic('python_manifest_invalid',str(error)[:2048],path,plugin_id))
    return result
