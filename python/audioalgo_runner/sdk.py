"""Borrowed PCM values for one external worker call, never a GUI/device callback."""
from __future__ import annotations
from dataclasses import dataclass,replace
from pathlib import Path
from typing import Any,Callable,Mapping
import numpy as np

class DiagnosticError(Exception):
    def __init__(self,code:str,message:str,port_id:str=''):
        super().__init__(message);self.code=code;self.port_id=port_id

class Cancelled(DiagnosticError):
    def __init__(self):super().__init__('run_cancelled','Python execution was cancelled; no result was published')

@dataclass(frozen=True)
class AudioPort:
    samples:np.ndarray
    sample_rate_hz:int
    channel_ids:tuple[str,...]
    valid_interval:tuple[int,int]

@dataclass(frozen=True)
class Context:
    node_id:str
    run_id:str
    request_id:str
    revision:int
    project_id:str
    session_epoch:int
    parameters:Mapping[str,Any]
    run_directory:Path
    maximum_block_frames:int
    frame_count:int
    block_offset:int
    is_cancelled:Callable[[],bool]

    def check_cancelled(self)->None:
        if self.is_cancelled():raise Cancelled()

    def block(self,offset:int,count:int)->Context:
        return replace(self,block_offset=offset,frame_count=count)
