from __future__ import annotations
import argparse
import os
from pathlib import Path
import socket
import threading
import traceback
from .engine import Engine
from .protocol import FramedSocket,identity,validate_execution_identity
from .sdk import DiagnosticError

def main()->int:
    parser=argparse.ArgumentParser(description='AudioAlgo dedicated offline Python runner')
    parser.add_argument('--port',type=int,required=True)
    parser.add_argument('--run-root',type=Path,required=True)
    parser.add_argument('--plugin-root',type=Path,action='append',required=True)
    parser.add_argument('--max-payload-bytes',type=int,default=64*1024*1024)
    args=parser.parse_args()
    # Pop before any user module can inspect inherited environment. Never log the token.
    token=os.environ.pop('AUDIOALGO_RUNNER_TOKEN','')
    if len(token)!=64 or not 0<args.port<65536:return 2
    engine=Engine(args.plugin_root,args.run_root,args.max_payload_bytes)
    sock=socket.create_connection(('127.0.0.1',args.port),timeout=5);sock.settimeout(None);channel=FramedSocket(sock)
    channel.send({'version':1,'message_type':'hello','token':token,'pid':os.getpid(),'request_id':'','run_id':'','revision':0})
    token=''
    state_lock=threading.Lock();active={'thread':None,'message':None,'cancel':None}

    def diagnostic(error:BaseException,message:dict,trace:str='')->dict:
        return {'version':1,'message_type':'diagnostic',**identity(message),
                'code':getattr(error,'code','python_exception'),'message':str(error)[:4096],
                'port_id':getattr(error,'port_id',''),'traceback':trace[-8192:]}

    def execute(message:dict,cancel:threading.Event):
        try:response={'version':1,'message_type':'result',**identity(message),'result':engine.execute(message,cancel.is_set)}
        except BaseException as error:response=diagnostic(error,message,traceback.format_exc())
        with state_lock:active.update(thread=None,message=None,cancel=None)
        try:channel.send(response)
        except (OSError,DiagnosticError):pass

    code=0
    try:
        while True:
            try:message=channel.receive()
            except EOFError:break
            except DiagnosticError as error:channel.send(diagnostic(error,{}));code=2;break
            kind=message.get('message_type')
            if kind=='shutdown':break
            if kind not in ('execute','cancel'):
                channel.send(diagnostic(DiagnosticError('protocol_message_unknown','Unknown message_type'),message));continue
            try:validate_execution_identity(message)
            except DiagnosticError as error:channel.send(diagnostic(error,message));continue
            if kind=='cancel':
                with state_lock:
                    current=active['message'];cancel=active['cancel']
                    matches=current is not None and identity(current)==identity(message)
                    if matches:cancel.set()
                if matches:channel.send({'version':1,'message_type':'progress',**identity(message),'state':'cancelling'})
                else:channel.send(diagnostic(DiagnosticError('run_not_active','No matching active request'),message))
                continue
            with state_lock:
                busy=active['thread'] is not None
                if not busy:
                    event=threading.Event();worker=threading.Thread(target=execute,args=(message,event),daemon=True)
                    active.update(thread=worker,message=message,cancel=event)
            if busy:channel.send(diagnostic(DiagnosticError('runner_busy','Runner executes one request at a time'),message))
            else:worker.start()
    finally:
        with state_lock:
            worker=active['thread'];cancel=active['cancel']
            if cancel:cancel.set()
        if worker:worker.join(timeout=2)
        sock.close()
    return code

if __name__=='__main__':raise SystemExit(main())
