"""Length-prefixed control data; stdout/stderr never carry control messages."""
from __future__ import annotations
import json
import socket
import struct
import threading
from .sdk import DiagnosticError

MAX_CONTROL_BYTES=1024*1024
IDENTITY_FIELDS=('project_id','session_epoch','request_id','run_id','revision','node_id')

class FramedSocket:
    def __init__(self,socket_:socket.socket):self.socket=socket_;self.write_lock=threading.Lock()
    def receive(self)->dict:
        def exact(count):
            data=bytearray()
            while len(data)<count:
                part=self.socket.recv(count-len(data))
                if not part:raise EOFError()
                data.extend(part)
            return data
        count=struct.unpack('!I',exact(4))[0]
        if not 0<count<=MAX_CONTROL_BYTES:raise DiagnosticError('protocol_message_too_large','Control length must be1..1048576 bytes')
        def invalid(value):raise ValueError('Non-finite JSON constant')
        try:value=json.loads(exact(count).decode('utf-8'),parse_constant=invalid)
        except (ValueError,UnicodeError) as error:raise DiagnosticError('protocol_json_invalid',str(error)) from error
        if not isinstance(value,dict):raise DiagnosticError('protocol_message_invalid','Control message must be an object')
        if type(value.get('version')) is not int or value['version']!=1:raise DiagnosticError('protocol_version_invalid','Unsupported control protocol version')
        return value
    def send(self,value:dict)->None:
        data=json.dumps(value,ensure_ascii=False,allow_nan=False,separators=(',',':')).encode('utf-8')
        if len(data)>MAX_CONTROL_BYTES:raise DiagnosticError('protocol_message_too_large','Response exceeds the1 MiB control budget')
        with self.write_lock:self.socket.sendall(struct.pack('!I',len(data))+data)

def identity(message:dict)->dict:
    return {key:message.get(key,0 if key in ('revision','session_epoch') else '') for key in IDENTITY_FIELDS}

def validate_execution_identity(message:dict)->None:
    for key in ('project_id','request_id','run_id','node_id'):
        value=message.get(key)
        if not isinstance(value,str) or not 0<len(value)<=1024:raise DiagnosticError('protocol_identity_invalid',key+' must be a bounded nonempty string')
    for key in ('revision','session_epoch'):
        if type(message.get(key)) is not int or message[key]<0:raise DiagnosticError('protocol_identity_invalid',key+' must be a nonnegative integer')
