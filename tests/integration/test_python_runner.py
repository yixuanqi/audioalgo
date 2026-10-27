"""Real independent process + loopback protocol + materialized NumPy PCM."""
from pathlib import Path
import hashlib,json,os,secrets,socket,struct,subprocess,sys,threading,time,unittest,uuid
import importlib.util,py_compile
import numpy as np
from test_python_manifest import manifest

ROOT=Path(__file__).resolve().parents[2]

def make_plugin(root:Path,plugin_id='test.python',factor=1.0,clip=False):
    root.mkdir(parents=True);m=manifest(plugin_id,plugin_id+'.node');n=m['nodes'][0]
    n['execution']['stateful']=True
    if clip:n['execution']['kind']='clip'
    modes=('gain','stateful','dtype','shape','nan','exception','wait','flood')
    n['parameters'].append({'id':'mode','type':'enum','default':'gain','choices':[{'value':v,'label':v} for v in modes]})
    (root/'manifest.json').write_text(json.dumps(m),encoding='utf-8')
    (root/'helper.py').write_text(f'FACTOR={factor!r}\n',encoding='utf-8')
    code='''import json,time,sys
from pathlib import Path
import numpy as np
from audioalgo_runner.sdk import AudioPort
from .helper import FACTOR
DESCRIPTOR=__DESCRIPTOR__
class GainNode:
    @classmethod
    def describe(cls):return DESCRIPTOR
    def __init__(self):self.position=0;self.calls=0;self.prepares=0
    def prepare(self,context):self.context=context;self.prepares+=1;self.gain=np.float32(10**(context.parameters['gain_db']/20))
    def process(self,inputs,context):
        self.calls+=1;value=inputs['audio_in'];n=value.samples.shape[1];mode=context.parameters['mode']
        if mode=='exception':raise RuntimeError('controlled user failure')
        if mode=='wait':
            (context.run_directory/'entered.txt').write_text('entered')
            while True:context.check_cancelled();time.sleep(.005)
        if mode=='flood':
            for i in range(256):print('stdout-flood:'+'x'*4096);print('stderr-flood:'+'y'*4096,file=sys.stderr)
        output=value.samples*self.gain*np.float32(FACTOR)
        if mode=='stateful':output=output+np.arange(self.position,self.position+n,dtype=np.float32)*np.float32(.00001)
        self.position+=n
        if mode=='dtype':output=output.astype(np.float64)
        if mode=='shape':output=output[:,:-1]
        if mode=='nan' and n:output[0,0]=np.nan
        return {'audio_out':AudioPort(output,value.sample_rate_hz,value.channel_ids,value.valid_interval)}
    def reset(self,reason):
        (self.context.run_directory/(self.context.node_id+'-audit.json')).write_text(json.dumps({'calls':self.calls,'prepares':self.prepares,'position':self.position,'reason':reason}))
    def process_clip(self,inputs,context):return self.process(inputs,context)
'''.replace('__DESCRIPTOR__',repr(n))
    (root/'gain_node.py').write_text(code,encoding='utf-8')
    return n

class Client:
    def __init__(self,root:Path,plugins:Path,max_payload_bytes=None):
        self.root=root;self.logs=[bytearray(),bytearray()];self.log_bytes=[0,0]
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(4)
        token=secrets.token_hex(32);env=os.environ.copy();env['PYTHONPATH']=str(ROOT/'python');env['AUDIOALGO_RUNNER_TOKEN']=token
        command=[sys.executable,'-u','-m','audioalgo_runner','--port',str(listener.getsockname()[1]),'--run-root',str(root),'--plugin-root',str(plugins)]
        if max_payload_bytes is not None:command+=['--max-payload-bytes',str(max_payload_bytes)]
        self.process=subprocess.Popen(command,cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        self.readers=[]
        for index,pipe in enumerate((self.process.stdout,self.process.stderr)):
            def drain(index=index,pipe=pipe):
                while data:=pipe.read1(4096):
                    self.log_bytes[index]+=len(data);self.logs[index].extend(data)
                    if len(self.logs[index])>65536:del self.logs[index][:-65536]
            thread=threading.Thread(target=drain,daemon=True);thread.start();self.readers.append(thread)
        try:self.socket,_=listener.accept()
        except BaseException:
            self.process.kill();self.process.wait(timeout=3)
            for thread in self.readers:thread.join(timeout=1)
            raise AssertionError('Runner did not establish the independent control connection: '+bytes(self.logs[1]).decode(errors='replace'))
        finally:listener.close()
        self.socket.settimeout(5);hello=self.receive();assert hello['message_type']=='hello' and hello['version']==1 and hello['token']==token
        assert hello['pid']==self.process.pid

    def send(self,value):
        body=json.dumps(value,allow_nan=False).encode();self.socket.sendall(struct.pack('!I',len(body))+body)
    def receive(self):
        def exact(n):
            data=bytearray()
            while len(data)<n:
                part=self.socket.recv(n-len(data))
                if not part:raise EOFError('Runner control connection closed')
                data.extend(part)
            return data
        n=struct.unpack('!I',exact(4))[0];assert n<=1048576
        return json.loads(exact(n))
    def execute(self,message):
        self.send(message)
        while True:
            response=self.receive()
            if response['message_type'] in ('result','diagnostic'):return response
    def close(self):
        try:self.send({'version':1,'message_type':'shutdown'})
        except (OSError,EOFError):pass
        try:self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=3)
        self.socket.close()
        for thread in self.readers:thread.join(timeout=1)
        for pipe in (self.process.stdout,self.process.stderr):pipe.close()

class PythonRunnerTests(unittest.TestCase):
    def setUp(self):
        self.root=ROOT/'build/runs'/('python-runner-'+uuid.uuid4().hex);self.root.mkdir(parents=True)
        self.plugins=self.root/'plugins';self.descriptor=make_plugin(self.plugins/'first')
        self.client=None
        index=np.arange(513,dtype=np.float64);self.samples=np.stack((.2*np.sin(index*.03),.1*np.cos(index*.05))).astype(np.float32)
        path=self.root/'input.npy';np.save(path,self.samples,allow_pickle=False)
        self.input={'path':'input.npy','sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'sample_rate_hz':48000,
                    'channel_ids':['reference','microphone'],'valid_interval':[5,501],'shape':[2,513],'dtype':'float32'}
    def tearDown(self):
        if self.client:self.client.close()
    def start(self):self.client=Client(self.root,self.plugins)
    def request(self,node='node-a',mode='gain',type_id='test.python.node'):
        return {'version':1,'message_type':'execute','project_id':'python-test','session_epoch':3,'request_id':uuid.uuid4().hex,
                'run_id':uuid.uuid4().hex,'revision':7,'node_id':node,'type_id':type_id,
                'parameters':{'gain_db':6.020599913279624,'mode':mode},'inputs':{'audio_in':dict(self.input)},'block_frames':256}
    def check_result(self,message,response,expected):
        self.assertEqual(response['message_type'],'result',response)
        for key in ('project_id','session_epoch','request_id','run_id','revision','node_id'):self.assertEqual(response[key],message[key])
        record=response['result']['outputs']['audio_out'];path=self.root/record['path'];actual=np.load(path,allow_pickle=False)
        self.assertEqual(actual.dtype,np.float32);self.assertEqual(actual.shape,(2,513));np.testing.assert_allclose(actual,expected,atol=1e-6,rtol=1e-5)
        self.assertEqual(record['valid_interval'],[5,501]);self.assertEqual(record['channel_ids'],['reference','microphone'])
        self.assertEqual(record['sha256'],hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertFalse(list(self.root.glob('python-work-*')))
        return path
    def test_real_process_gain_stateful_two_instances_and_reset(self):
        self.start()
        a=self.request();self.check_result(a,self.client.execute(a),2*self.samples.astype(np.float64))
        reference=2*self.samples.astype(np.float64)+np.arange(513,dtype=np.float64)*.00001
        for node in ('state-a','state-b'):
            message=self.request(node,'stateful');self.check_result(message,self.client.execute(message),reference)
            audit=json.loads((self.root/(node+'-audit.json')).read_text())
            self.assertEqual(audit,{'prepares':1,'calls':3,'position':513,'reason':'run_complete'})
    def test_deep_run_paths_preserve_payload_code_and_failure_recovery(self):
        self.root=self.root/'deep'
        while len(str(self.root))<210:self.root=self.root/'nested_1234567890'
        self.root.mkdir(parents=True)
        np.save(self.root/'input.npy',self.samples,allow_pickle=False)
        self.start();message=self.request();response=self.client.execute(message)
        self.assertEqual(response['message_type'],'result',response)
        def physical(path):
            name=str(path.resolve())
            return Path('\\\\?\\'+name) if os.name=='nt' and not name.startswith('\\\\?\\') else path
        result=response['result'];output=self.root/result['outputs']['audio_out']['path'];data=physical(output).read_bytes()
        self.assertGreater(len(str(output)),260)
        actual=np.load(physical(output),allow_pickle=False);np.testing.assert_allclose(actual,2*self.samples.astype(np.float64),atol=1e-6,rtol=1e-5)
        self.assertEqual(hashlib.sha256(data).hexdigest(),result['outputs']['audio_out']['sha256'])
        for name,digest in result['code_files'].items():
            self.assertEqual(hashlib.sha256(physical(self.root/result['code_snapshot']/name).read_bytes()).hexdigest(),digest)
        failed=self.client.execute(self.request(mode='exception'));self.assertEqual(failed['code'],'python_exception')
        self.assertFalse(list(self.root.glob('python-work-*')))
        recovered=self.client.execute(self.request());self.assertEqual(recovered['message_type'],'result',recovered)
        self.assertEqual(physical(output).read_bytes(),data)
    def test_wrong_dtype_shape_nan_exception_and_payload_failures_recover(self):
        self.start()
        for mode,code in [('dtype','python_output_dtype_invalid'),('shape','python_output_shape_invalid'),('nan','python_output_non_finite'),('exception','python_exception')]:
            with self.subTest(mode=mode):
                message=self.request(mode=mode);response=self.client.execute(message)
                self.assertEqual(response['message_type'],'diagnostic');self.assertEqual(response['code'],code,response)
                self.assertEqual(response['node_id'],message['node_id'])
                if mode!='exception':self.assertEqual(response['port_id'],'audio_out')
                else:self.assertIn('gain_node.py',response['traceback']);self.assertIn('controlled user failure',response['traceback'])
                self.assertFalse(list(self.root.glob('python-work-*')))
        bad=self.request();bad['inputs']['audio_in']['sha256']='0'*64
        self.assertEqual(self.client.execute(bad)['code'],'python_payload_hash_mismatch')
        good=self.request();self.check_result(good,self.client.execute(good),2*self.samples.astype(np.float64))
    def test_namespaced_modules_and_restart_update_preserve_history(self):
        make_plugin(self.plugins/'second','test.other',3.0);self.start()
        first=self.request();old=self.check_result(first,self.client.execute(first),2*self.samples.astype(np.float64));old_bytes=old.read_bytes()
        other=self.request(type_id='test.other.node');self.check_result(other,self.client.execute(other),6*self.samples.astype(np.float64))
        old_pid=self.client.process.pid;(self.plugins/'first/helper.py').write_text('FACTOR=2.0\n')
        self.assertEqual(self.client.execute(self.request())['code'],'python_restart_required')
        self.client.close();self.client=None;self.start();self.assertNotEqual(self.client.process.pid,old_pid)
        updated=self.request();self.check_result(updated,self.client.execute(updated),4*self.samples.astype(np.float64));self.assertEqual(old.read_bytes(),old_bytes)
    def test_unhashed_bytecode_cannot_override_the_frozen_source(self):
        self.start();first=self.request();response=self.client.execute(first)
        self.check_result(first,response,2*self.samples.astype(np.float64))
        snapshot=self.root/response['result']['code_snapshot'];helper=snapshot/'helper.py'
        self.client.close();self.client=None
        fake=self.root/'fake_helper.py';fake.write_text('FACTOR=9.0\n')
        self.assertEqual(fake.stat().st_size,helper.stat().st_size)
        os.utime(fake,(helper.stat().st_atime,helper.stat().st_mtime))
        cached=Path(importlib.util.cache_from_source(str(helper)));cached.parent.mkdir(exist_ok=True)
        py_compile.compile(str(fake),cfile=str(cached),dfile=str(helper),doraise=True,invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP)
        self.start();again=self.request();self.check_result(again,self.client.execute(again),2*self.samples.astype(np.float64))
    def test_cooperative_cancel_and_log_flood_use_separate_bounded_pipes(self):
        self.start();message=self.request(mode='wait');self.client.send(message);deadline=time.monotonic()+3
        while not (self.root/'entered.txt').exists() and time.monotonic()<deadline:time.sleep(.005)
        self.assertTrue((self.root/'entered.txt').exists());started=time.monotonic()
        self.client.send(message|{'message_type':'cancel'})
        while True:
            response=self.client.receive()
            if response['message_type']=='diagnostic':break
        self.assertEqual(response['code'],'run_cancelled');self.assertLess(time.monotonic()-started,2)
        flood=self.request(mode='flood');self.check_result(flood,self.client.execute(flood),2*self.samples.astype(np.float64))
        self.assertTrue(all(n>65536 for n in self.client.log_bytes));self.assertTrue(all(len(b)<=65536 for b in self.client.logs))
        gain=self.request();self.check_result(gain,self.client.execute(gain),2*self.samples.astype(np.float64))
    def test_clip_runs_once_and_payload_budget_is_checked_before_plugin_import(self):
        make_plugin(self.plugins/'clip','test.clip',clip=True);self.start()
        message=self.request('clip-node',type_id='test.clip.node');self.check_result(message,self.client.execute(message),2*self.samples.astype(np.float64))
        audit=json.loads((self.root/'clip-node-audit.json').read_text());self.assertEqual(audit['calls'],1);self.assertEqual(audit['prepares'],1)
        self.client.close();self.client=None
        marker=self.root/'plugin-imported.txt';source=self.plugins/'first/gain_node.py'
        source.write_text(f'from pathlib import Path\nPath({str(marker)!r}).write_text("imported")\n'+source.read_text())
        self.client=Client(self.root,self.plugins,8192)
        response=self.client.execute(self.request());self.assertEqual(response['code'],'python_payload_limit',response)
        self.assertFalse(marker.exists());self.assertFalse(list(self.root.glob('python-work-*')))
    def test_workspace_profile_compiles_all_sources_before_any_import(self):
        package=self.plugins/'first';m=json.loads((package/'manifest.json').read_text())
        m['authoring_profile']={'kind':'workspace-function-v1','syntax_only':True}
        m['nodes'][0]['entrypoint']='workspace_src.process:custom'
        (package/'manifest.json').write_text(json.dumps(m))
        source=package/'workspace_src';source.mkdir()
        marker=self.root/'imported.txt'
        (source/'process.py').write_text(f'from pathlib import Path\nPath({str(marker)!r}).write_text("imported")\n')
        (source/'unused.py').write_text('value = (\n',encoding='utf-8')
        self.start();response=self.client.execute(self.request())
        self.assertEqual(response['code'],'python_syntax_error',response)
        self.assertIn('src/unused.py:1:9:',response['message']);self.assertFalse(marker.exists())

    def test_unknown_workspace_profile_refuses_user_import(self):
        package=self.plugins/'first';m=json.loads((package/'manifest.json').read_text())
        m['authoring_profile']={'kind':'unknown','syntax_only':False}
        (package/'manifest.json').write_text(json.dumps(m))
        marker=self.root/'imported.txt';source=package/'gain_node.py'
        source.write_text(f'from pathlib import Path\nPath({str(marker)!r}).write_text("imported")\n'+source.read_text())
        self.start();response=self.client.execute(self.request())
        self.assertEqual(response['code'],'python_authoring_profile_invalid',response);self.assertFalse(marker.exists())

    def test_ordinary_plugin_keeps_128_file_budget_and_recovers(self):
        package=self.plugins/'first'
        for i in range(126):(package/f'unused_{i}.py').write_text('value = True\n')
        self.start();response=self.client.execute(self.request())
        self.assertEqual(response['code'],'python_code_limit',response)
        (package/'unused_125.py').unlink()
        request=self.request();self.check_result(request,self.client.execute(request),2*self.samples.astype(np.float64))

    def test_protocol_length_version_and_unknown_message_are_diagnosable(self):
        self.start();self.client.send({'version':1,'message_type':'unknown'})
        self.assertEqual(self.client.receive()['code'],'protocol_message_unknown')
        self.client.send({'version':2,'message_type':'execute'});self.assertEqual(self.client.receive()['code'],'protocol_version_invalid')
        self.client.process.wait(timeout=3);self.assertNotEqual(self.client.process.returncode,0)
        self.client.close();self.client=None;self.start()
        self.client.socket.sendall(struct.pack('!I',1048577));self.assertEqual(self.client.receive()['code'],'protocol_message_too_large')
        self.client.process.wait(timeout=3);self.assertNotEqual(self.client.process.returncode,0)
        self.client.close();self.client=None;self.start()
        message=self.request();self.check_result(message,self.client.execute(message),2*self.samples.astype(np.float64))

if __name__=='__main__':unittest.main()
