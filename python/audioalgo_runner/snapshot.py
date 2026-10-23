"""Materialize all package-local Python modules before importing user code."""
from __future__ import annotations
from pathlib import Path
import hashlib
import json
import os
from .sdk import DiagnosticError

MAX_CODE_BYTES=8*1024*1024
MAX_CODE_FILES=128

def freeze_code(root:Path,destination:Path)->tuple[Path,str,dict[str,str]]:
    files=sorted(p for p in root.rglob('*') if p.suffix=='.py' or p==root/'manifest.json')
    if len(files)>MAX_CODE_FILES:raise DiagnosticError('python_code_limit','Package has too many local Python files')
    payload={};hashes={};total=0
    for path in files:
        if path.is_symlink() or not path.is_file() or root.resolve() not in path.resolve().parents:
            raise DiagnosticError('python_code_path_invalid','Code files must be regular files inside the plugin root')
        with path.open('rb') as stream:data=stream.read(MAX_CODE_BYTES-total+1)
        total+=len(data)
        if total>MAX_CODE_BYTES:raise DiagnosticError('python_code_limit','Code snapshot exceeds its byte budget')
        name=path.relative_to(root).as_posix();payload[name]=data;hashes[name]=hashlib.sha256(data).hexdigest()
    if 'manifest.json' not in hashes:raise DiagnosticError('python_manifest_missing','Code snapshot has no manifest')
    # Verify the source set is still the exact captured version before the freeze boundary.
    current=sorted(p for p in root.rglob('*') if p.suffix=='.py' or p==root/'manifest.json')
    def unchanged(path):
        name=path.relative_to(root).as_posix()
        with path.open('rb') as stream:return stream.read(len(payload[name])+1)==payload[name]
    if current!=files or not all(unchanged(p) for p in files):
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
