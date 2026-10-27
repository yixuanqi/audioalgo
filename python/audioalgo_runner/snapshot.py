"""Materialize all package-local Python modules before importing user code."""
from __future__ import annotations
from pathlib import Path
import hashlib
import json
import os
from .sdk import DiagnosticError

MAX_CODE_BYTES=8*1024*1024
MAX_CODE_FILES=128
MAX_RESOURCE_BYTES=64*1024*1024
MAX_RESOURCE_FILES=64

def inventory(root:Path):
    entries={};resources=set();workspace=False
    manifest=root/'manifest.json'
    if manifest.is_file():
        with manifest.open('rb') as stream:raw=stream.read(1024*1024+1)
        if len(raw)>1024*1024:raise DiagnosticError('python_code_limit','Manifest exceeds1 MiB')
        metadata=json.loads(raw)
        if 'workspace_files' in metadata:
            workspace=True;entries=metadata['workspace_files'];profile=metadata.get('authoring_profile',{})
            if (not isinstance(entries,dict) or len(entries)>191 or not isinstance(profile,dict) or
                    profile.get('kind')!='workspace-function-v1' or type(profile.get('syntax_only')) is not bool):
                raise DiagnosticError('python_authoring_profile_invalid','Invalid workspace runtime file list')
            for name,entry in entries.items():
                path=Path(name)
                if (not name or len(name.encode())>522 or '\\' in name or ':' in name or path.is_absolute() or
                        path.as_posix()!=name or '..' in path.parts or name in ('manifest.json','snapshot.json')):
                    raise DiagnosticError('python_code_path_invalid','Invalid workspace runtime path')
                if (not isinstance(entry,dict) or entry.get('role') not in ('source','resource') or
                        type(entry.get('bytes')) is not int or entry['bytes']<0 or
                        not isinstance(entry.get('sha256'),str) or len(entry['sha256'])!=64 or
                        any(c not in '0123456789abcdef' for c in entry['sha256'])):
                    raise DiagnosticError('python_authoring_profile_invalid','Invalid workspace runtime identity')
                if entry['role']=='resource':resources.add(name)
    files={}
    for path in root.rglob('*'):
        if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
            raise DiagnosticError('python_code_path_invalid','Runtime files must not be links')
        if not path.is_file():continue
        name=path.relative_to(root).as_posix()
        if workspace:
            if name not in entries and name not in ('manifest.json','snapshot.json'):
                raise DiagnosticError('python_snapshot_modified','Workspace has an undeclared runtime file')
            if name!='snapshot.json':files[name]=path
        elif path.suffix=='.py' or name=='manifest.json':files[name]=path
    if len(files)-len(resources)>MAX_CODE_FILES or len(resources)>MAX_RESOURCE_FILES:
        raise DiagnosticError('python_code_limit','Package has too many source/resource files')
    if workspace and set(files)!=set(entries)|{'manifest.json'}:
        raise DiagnosticError('python_snapshot_modified','Declared workspace runtime file is missing')
    return dict(sorted(files.items())),resources,entries

def freeze_code(root:Path,destination:Path)->tuple[Path,str,dict[str,str]]:
    files,resources,entries=inventory(root)
    payload={};hashes={};total=0;resource_total=0
    for name,path in files.items():
        if path.is_symlink() or not path.is_file() or root.resolve() not in path.resolve().parents:
            raise DiagnosticError('python_code_path_invalid','Code files must be regular files inside the plugin root')
        limit=MAX_RESOURCE_BYTES-resource_total if name in resources else MAX_CODE_BYTES-total
        with path.open('rb') as stream:data=stream.read(limit+1)
        if len(data)>limit:raise DiagnosticError('python_code_limit','Snapshot exceeds its source/resource byte budget')
        if name in resources:resource_total+=len(data)
        else:total+=len(data)
        payload[name]=data;hashes[name]=hashlib.sha256(data).hexdigest()
        if name in entries and (entries[name]['bytes']!=len(data) or entries[name]['sha256']!=hashes[name]):
            raise DiagnosticError('python_snapshot_modified','Runtime file differs from its declared identity')
    if 'manifest.json' not in hashes:raise DiagnosticError('python_manifest_missing','Code snapshot has no manifest')
    # Verify the source set is still the exact captured version before the freeze boundary.
    current=inventory(root)
    def unchanged(name,path):
        with path.open('rb') as stream:return stream.read(len(payload[name])+1)==payload[name]
    if current!=(files,resources,entries) or not all(unchanged(n,p) for n,p in files.items()):
        raise DiagnosticError('python_code_changed_during_snapshot','Code changed while it was being frozen')
    digest=hashlib.sha256(json.dumps(hashes,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    target=destination/digest;target.mkdir(parents=True,exist_ok=True)
    for name,data in payload.items():
        path=target/name;path.parent.mkdir(parents=True,exist_ok=True)
        if path.exists():
            with path.open('rb') as stream:existing=stream.read(len(data)+1)
            if existing!=data:raise DiagnosticError('python_snapshot_modified','Previously frozen code was modified')
        else:
            temp=path.with_name(path.name+'.tmp')
            with temp.open('xb') as stream:stream.write(data)
            os.replace(temp,path)
    return target,digest,hashes
