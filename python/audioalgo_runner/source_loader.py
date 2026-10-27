"""Import only source bytes named and hashed in the frozen package manifest."""
from __future__ import annotations
import hashlib
import importlib
import importlib.abc
import importlib.util
from pathlib import Path
import sys
from .sdk import DiagnosticError
from .snapshot import MAX_CODE_BYTES

class SourceLoader(importlib.abc.Loader):
    def __init__(self,path:Path|None,digest:str|None):self.path=path;self.digest=digest
    def create_module(self,spec):return None
    def exec_module(self,module):
        if self.path is None:return # Declared namespace package, with no initializer.
        if self.path.is_symlink():raise DiagnosticError('python_snapshot_modified','Frozen source became a symlink')
        with self.path.open('rb') as stream:data=stream.read(MAX_CODE_BYTES+1)
        if len(data)>MAX_CODE_BYTES or hashlib.sha256(data).hexdigest()!=self.digest:
            raise DiagnosticError('python_snapshot_modified','Imported source bytes differ from the code snapshot')
        module.__file__=str(self.path)
        exec(compile(data,str(self.path),'exec',dont_inherit=True),module.__dict__)

class FrozenFinder(importlib.abc.MetaPathFinder):
    def __init__(self):self.roots={}
    def find_spec(self,fullname,path=None,target=None):
        for namespace,(root,files) in self.roots.items():
            if fullname!=namespace and not fullname.startswith(namespace+'.'):continue
            parts=fullname[len(namespace):].lstrip('.').split('.') if fullname!=namespace else []
            if any(not p.isidentifier() for p in parts):raise ModuleNotFoundError(fullname)
            relative='/'.join(parts);package=(relative+'/' if relative else '')+'__init__.py'
            source=relative+'.py'
            if package in files:
                file=root/package;loader=SourceLoader(file,files[package]);is_package=True
            elif source in files:
                file=root/source;loader=SourceLoader(file,files[source]);is_package=False
            elif not parts or any(name.startswith(relative+'/') for name in files):
                file=None;loader=SourceLoader(None,None);is_package=True
            else:
                # Do not fall through to a normal loader that may accept unhashed .pyc files.
                raise ModuleNotFoundError('Module is absent from the frozen code set: '+fullname)
            spec=importlib.util.spec_from_loader(fullname,loader,is_package=is_package)
            if is_package:spec.submodule_search_locations=[str(root/relative)]
            return spec
        return None

_finder=FrozenFinder()

def load_module(plugin_id:str,digest:str,root:Path,files:dict[str,str],module_name:str):
    namespace='_audioalgo_'+hashlib.sha256((plugin_id+digest).encode()).hexdigest()
    if _finder not in sys.meta_path:sys.meta_path.insert(0,_finder)
    _finder.roots[namespace]=(root,files)
    return importlib.import_module(namespace+'.'+module_name)

def load_class(plugin_id:str,digest:str,root:Path,files:dict[str,str],entrypoint:str):
    module_name,class_name=entrypoint.split(':')
    return getattr(load_module(plugin_id,digest,root,files,module_name),class_name)
