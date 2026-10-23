#include "audio_python_backend/PythonTask.h"
#include "audio_algorithms/Gain.h"
#include "plugins/host/registration.h"
#include "audio_runtime/NativePlan.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <fstream>
#include <future>
#include <cmath>
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
audioalgo::PythonTaskRequest request(const std::filesystem::path& root);
void write(const std::filesystem::path& p,const std::string& s){std::ofstream(p)<<s;}
std::filesystem::path package(const std::filesystem::path& root) {
    const auto plugin=root/"plugin";std::filesystem::create_directories(plugin);
    const std::string descriptor=R"({"type_id":"test.cpp.python","version":"1.0.0","display_name":"C++ Python fixture","entrypoint":"node:Node","execution":{"kind":"block","stateful":true,"realtime_verified":false},"inputs":[{"id":"audio_in","type":"audio.pcm@1","required":true}],"outputs":[{"id":"audio_out","type":"audio.pcm@1"}],"format":{"sample_rates_hz":[16000,48000],"channels":[1,2,4]},"parameters":[{"id":"gain_db","type":"float","default":0.0,"minimum":-60.0,"maximum":12.0},{"id":"mode","type":"string","default":"stateful"}]})";
    write(plugin/"manifest.json",R"({"schema_version":1,"runtime":"python","plugin_id":"test.cpp","plugin_version":"1.0.0","nodes":[)"+descriptor+"]}");
    write(plugin/"node.py","import json,time,sys,subprocess\nimport numpy as np\nfrom audioalgo_runner.sdk import AudioPort\nDESCRIPTOR=json.loads(r'''"+descriptor+R"(''')
class Node:
    @classmethod
    def describe(cls):return DESCRIPTOR
    def prepare(self,c):self.position=0;self.gain=np.float32(10**(c.parameters['gain_db']/20));self.context=c
    def process(self,inputs,c):
        mode=c.parameters['mode'];value=inputs['audio_in'];n=value.samples.shape[1]
        if mode in ('wait','spawn'):
            if mode=='spawn':
                child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(1000)'],creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                (c.run_directory/'child-pid.txt').write_text(str(child.pid))
            (c.run_directory/'entered.txt').write_text('entered')
            while True:
                if mode=='wait':c.check_cancelled()
                time.sleep(.005)
        if mode=='crash':__import__('os')._exit(73)
        if mode=='flood':
            for i in range(256):print('o'*4096);print('e'*4096,file=sys.stderr)
        result=value.samples*self.gain
        if mode=='stateful':result=result+np.arange(self.position,self.position+n,dtype=np.float32)*np.float32(.00001)
        self.position+=n
        if mode=='dtype':result=result.astype(np.float64)
        return {'audio_out':AudioPort(result,value.sample_rate_hz,value.channel_ids,value.valid_interval)}
    def reset(self,reason):self.position=0
)");return plugin;
}

TEST_CASE("PY-06 C++ supervisor cancels cooperative work and reclaims unresponsive descendants within five seconds","[PY-06]") {
    for(const std::string mode:{"wait","spawn"}) {
        INFO(mode);const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-cancel-"+juce::Uuid().toString().toStdString());
        REQUIRE(std::filesystem::create_directories(root));auto r=request(root);r.parameters={{"mode",mode}};std::atomic<bool> cancel=false;r.is_cancelled=[&]{return cancel.load();};
        audioalgo::PythonTaskStatistics stats;auto future=std::async(std::launch::async,[&]{return audioalgo::run_python_task(r,&stats);});
        std::filesystem::path entered;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(entered.empty() && std::chrono::steady_clock::now()<deadline) {
            for(const auto& entry:std::filesystem::directory_iterator(root))if(std::filesystem::exists(entry.path()/"entered.txt")){entered=entry.path();break;}
            if(entered.empty())std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#ifdef _WIN32
        DWORD child_pid=0;HANDLE child=nullptr;
        if(mode=="spawn" && !entered.empty()){std::ifstream(entered/"child-pid.txt")>>child_pid;child=OpenProcess(SYNCHRONIZE,FALSE,child_pid);}
#endif
        const auto started=std::chrono::steady_clock::now();cancel=true;
        const auto ready=future.wait_for(std::chrono::seconds(5));REQUIRE(ready==std::future_status::ready);const auto result=future.get();
        CHECK_FALSE(entered.empty());REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));CHECK(std::get<audioalgo::Diagnostic>(result).code=="run_cancelled");
        CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds(5));CHECK(stats.forced_termination==(mode=="spawn"));
        CHECK_FALSE(std::filesystem::exists(entered));CHECK(stats.exit_code!=259);
#ifdef _WIN32
        if(mode=="spawn"){CHECK(child_pid!=0);CHECK(child!=nullptr);if(child){CHECK(WaitForSingleObject(child,1000)==WAIT_OBJECT_0);CloseHandle(child);}}
#endif
        r.is_cancelled={};r.parameters={{"mode",std::string("gain")}};const auto recovery=audioalgo::run_python_task(r);
        REQUIRE(std::holds_alternative<audioalgo::PythonTaskSuccess>(recovery));
        const auto& output=std::get<audioalgo::PythonTaskSuccess>(recovery).outputs.at("audio_out");
        for(std::size_t c=0;c<2;++c)CHECK(std::equal(output.channel(c).begin(),output.channel(c).end(),r.inputs.at("audio_in").channel(c).begin()));
    }
}

TEST_CASE("PY-08 C++ collector bounds actual pipe floods and recovers after crash and dtype rejection","[PY-08]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-faults-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));auto r=request(root);
    for(const std::string mode:{"flood","crash","dtype","gain"}) {
        INFO(mode);r.parameters={{"mode",mode}};audioalgo::PythonTaskStatistics stats;
        const auto result=audioalgo::run_python_task(r,&stats);
        if(mode=="flood" || mode=="gain") {
            if(auto* e=std::get_if<audioalgo::Diagnostic>(&result)){CAPTURE(e->code,e->message);REQUIRE(false);}
            REQUIRE(std::holds_alternative<audioalgo::PythonTaskSuccess>(result));
            const auto& output=std::get<audioalgo::PythonTaskSuccess>(result).outputs.at("audio_out");
            for(std::size_t c=0;c<2;++c)CHECK(std::equal(output.channel(c).begin(),output.channel(c).end(),r.inputs.at("audio_in").channel(c).begin()));
            if(mode=="flood"){CHECK(stats.stdout_bytes>1024*1024);CHECK(stats.stderr_bytes>1024*1024);CHECK(stats.stdout_tail.size()<=65536);CHECK(stats.stderr_tail.size()<=65536);}
        } else {
            REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));const auto& error=std::get<audioalgo::Diagnostic>(result);CHECK(error.node_id==r.node_id);
            if(mode=="dtype"){CHECK(error.code=="python_output_dtype_invalid");CHECK(error.port_id=="audio_out");}
            else CHECK((error.code=="python_runner_exited" || error.code=="python_runner_disconnected"));
            CHECK(stats.exit_code!=259);
        }
    }
}

TEST_CASE("PY-08 C++ control EOF from a live runner is diagnosed without waiting for execution timeout","[PY-08]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-eof-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));auto r=request(root);const auto module=root/"fault-runner/audioalgo_runner";
    REQUIRE(std::filesystem::create_directories(module));write(module/"__init__.py","");
    write(module/"__main__.py",R"(import socket,os,json,struct,sys,time
port=int(sys.argv[sys.argv.index('--port')+1]);s=socket.create_connection(('127.0.0.1',port))
token=os.environ.pop('AUDIOALGO_RUNNER_TOKEN');hello=json.dumps({'version':1,'message_type':'hello','token':token,'pid':os.getpid()}).encode()
s.sendall(struct.pack('!I',len(hello))+hello)
def read(n):
    b=b''
    while len(b)<n:b+=s.recv(n-len(b))
    return b
read(struct.unpack('!I',read(4))[0]);s.close();time.sleep(1000)
)");
    r.runner_module_root=module.parent_path();std::atomic<bool> cancel=false;r.is_cancelled=[&]{return cancel.load();};
    auto future=std::async(std::launch::async,[&]{return audioalgo::run_python_task(r);});
    const auto ready=future.wait_for(std::chrono::seconds(5));cancel=true;
    const auto result=future.get();CHECK(ready==std::future_status::ready);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));CHECK(std::get<audioalgo::Diagnostic>(result).code=="python_runner_disconnected");
}
audioalgo::PythonTaskRequest request(const std::filesystem::path& root) {
    audioalgo::PythonTaskRequest r;r.python_executable=AUDIOALGO_PYTHON_EXE;r.runner_module_root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"python";
    r.plugin_root=package(root);r.output_parent=root;r.type_id="test.cpp.python";r.node_id="python-node";
    r.output_ports={"audio_out"};r.maximum_block_frames=256;
    r.identity={"cpp-python-project",3,juce::Uuid().toString().toStdString(),7};
    audioalgo::AudioBuffer input(48000,{"reference","microphone"},513,{5,501});
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<513;++i)input.mutable_channel(c)[i]=static_cast<float>(.1*std::sin((i+1)*(c+1)*.03));
    r.inputs.emplace("audio_in",std::move(input));return r;
}
}

TEST_CASE("PY-02 C++ worker executes real Python state and returns owned PCM for native algorithms","[PY-02]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-python-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));auto r=request(root);const auto original=r.inputs.at("audio_in");
    const auto gained=audioalgo::apply_gain_db(original,6.020599913279624);REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(gained));
    r.inputs.at("audio_in")=std::get<audioalgo::AudioBuffer>(gained);r.parameters={{"gain_db",0.0},{"mode",std::string("stateful")}};
    audioalgo::PythonTaskStatistics stats;
    auto task=std::async(std::launch::async,[&]{return audioalgo::run_python_task(r,&stats);});const auto result=task.get();
    if(const auto* e=std::get_if<audioalgo::Diagnostic>(&result)){CAPTURE(e->code,e->message);REQUIRE(false);}
    REQUIRE(std::holds_alternative<audioalgo::PythonTaskSuccess>(result));const auto& success=std::get<audioalgo::PythonTaskSuccess>(result);
    const auto reduced=audioalgo::apply_gain_db(success.outputs.at("audio_out"),-6.020599913279624);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(reduced));const auto& output=std::get<audioalgo::AudioBuffer>(reduced);
    CHECK(output.channel_ids()==original.channel_ids());CHECK(output.valid_interval()==original.valid_interval());
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<513;++i)CHECK(std::abs(output.channel(c)[i]-(original.channel(c)[i]+.5*.00001*i))<1e-6);
    CHECK(stats.process_id!=0);CHECK(stats.exit_code==0);CHECK_FALSE(stats.forced_termination);
    CHECK(std::filesystem::is_directory(success.artifact_directory));CHECK_FALSE(success.provenance_json.empty());
}

TEST_CASE("PY-04 C++ caller retains startup traceback even without requesting log statistics","[PY-04]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-startup-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));auto r=request(root);const auto module=root/"fault-runner/audioalgo_runner";
    REQUIRE(std::filesystem::create_directories(module));write(module/"__init__.py","");
    write(module/"__main__.py","raise RuntimeError('controlled runner startup failure')\n");r.runner_module_root=module.parent_path();
    const auto result=audioalgo::run_python_task(r);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));const auto& error=std::get<audioalgo::Diagnostic>(result);
    CHECK(error.code=="python_runner_exited");CHECK(error.node_id==r.node_id);
    CHECK(error.message.find("controlled runner startup failure")!=std::string::npos);
    CHECK(error.message.find("__main__.py")!=std::string::npos);
}

TEST_CASE("PY-01 C++ host composition and CLI discover Python metadata without importing user code","[PY-01]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-catalog-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));const auto plugin=package(root);const auto marker=root/"imported.txt";
    const auto code=juce::File((plugin/"node.py").string()).loadFileAsString().toStdString();
    write(plugin/"node.py","from pathlib import Path\nPath(r'"+marker.generic_string()+"').write_text('imported')\n"+code);
    audioalgo::NodeRegistry registry;const auto found=audioalgo::register_host_nodes(registry,{plugin});
    REQUIRE(found.diagnostics.empty());const auto* d=registry.find("test.cpp.python");REQUIRE(d!=nullptr);
    CHECK(d->display_name=="C++ Python fixture");REQUIRE(d->ports.size()==2);CHECK(d->ports[0].id=="audio_in");CHECK(d->ports[1].id=="audio_out");
    REQUIRE(d->parameters.size()==2);CHECK(d->parameters[0].kind==audioalgo::ParameterKind::floating);CHECK(d->parameters[0].minimum==-60);CHECK(d->parameters[0].maximum==12);
    CHECK_FALSE(std::filesystem::exists(marker));
    juce::ChildProcess cli;REQUIRE(cli.start(juce::StringArray{AUDIOALGO_CLI_EXE,"--plugin-dir",juce::String(plugin.string()),"--list-nodes"}));
    const auto cli_json=cli.readAllProcessOutput();REQUIRE(cli.waitForProcessToFinish(5000));CHECK(cli.getExitCode()==0);
    CHECK(juce::JSON::toString(juce::JSON::parse(cli_json),true).toStdString()==audioalgo::node_catalog_json(registry,found));
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("P3-05 actual framed transport handles fragmented and coalesced messages rejects invalid peers and recovers","[P3-05][protocol]") {
    for(const auto& [mode,expected]:std::vector<std::pair<std::string,std::string>>{
        {"fragmented","fragmentation_probe"},{"token","python_authentication_failed"},{"pid","python_authentication_failed"},{"version","protocol_version_invalid"},
        {"zero","protocol_message_too_large"},{"oversized","protocol_message_too_large"},{"unknown","protocol_message_unknown"},{"identity","python_identity_mismatch"},
        {"json","python_protocol_json_invalid"},{"truncated","python_runner_disconnected"}}) {
        INFO(mode);const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-framing-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));auto r=request(root);
        const auto module=root/"fault-runner/audioalgo_runner";REQUIRE(std::filesystem::create_directories(module));write(module/"__init__.py","");
        write(module/"__main__.py","MODE='"+mode+"'\n"+R"py(import socket,os,struct,json,sys,time
from pathlib import Path
s=socket.create_connection(('127.0.0.1',int(sys.argv[sys.argv.index('--port')+1])))
def read(n):
    value=b''
    while len(value)<n:
        part=s.recv(n-len(value))
        if not part:raise EOFError()
        value+=part
    return value
def receive():return json.loads(read(struct.unpack('!I',read(4))[0]))
def frame(value):
    payload=json.dumps(value).encode();return struct.pack('!I',len(payload))+payload
token=os.environ.pop('AUDIOALGO_RUNNER_TOKEN')
hello={'version':2 if MODE=='version' else 1,'message_type':'hello','token':'wrong' if MODE=='token' else token,'pid':os.getpid()+(1 if MODE=='pid' else 0)}
if MODE in ('zero','oversized'):
    s.sendall(struct.pack('!I',0 if MODE=='zero' else 1048577));receive();raise SystemExit()
if MODE=='fragmented':
    for byte in frame(hello):s.sendall(bytes([byte]));time.sleep(.001)
else:s.sendall(frame(hello))
message=receive()
if message.get('message_type')!='execute':raise SystemExit()
(Path(__file__).parents[2]/'execute-received.txt').write_text('execute')
response=dict(message);response.update(message_type='diagnostic',code='fragmentation_probe',message='fragmented hello and coalesced messages accepted',traceback='',port_id='audio_out')
if MODE=='unknown':response['message_type']='invented'
if MODE=='identity':response['request_id']='other-request'
if MODE=='fragmented':
    progress=dict(message);progress['message_type']='progress';s.sendall(frame(progress)+frame(response))
elif MODE=='json':s.sendall(struct.pack('!I',1)+b'{')
elif MODE=='truncated':s.sendall(struct.pack('!I',100)+b'{');s.close();time.sleep(10);raise SystemExit()
else:s.sendall(frame(response))
receive()
)py");
        r.runner_module_root=module.parent_path();audioalgo::PythonTaskStatistics stats;const auto result=audioalgo::run_python_task(r,&stats);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));const auto& error=std::get<audioalgo::Diagnostic>(result);INFO(error.message);CHECK(error.code==expected);CHECK(error.node_id==r.node_id);CHECK(stats.exit_code!=259);
        if(mode=="fragmented")CHECK(std::filesystem::exists(root/"execute-received.txt"));
        r.runner_module_root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"python";r.parameters={{"mode",std::string("gain")}};
        const auto recovery=audioalgo::run_python_task(r);REQUIRE(std::holds_alternative<audioalgo::PythonTaskSuccess>(recovery));
        const auto& output=std::get<audioalgo::PythonTaskSuccess>(recovery).outputs.at("audio_out");for(std::size_t c=0;c<2;++c)CHECK(std::equal(output.channel(c).begin(),output.channel(c).end(),r.inputs.at("audio_in").channel(c).begin()));
    }
}

TEST_CASE("P3-05 outgoing one MiB control limit refuses execution before user import then recovers","[P3-05][protocol]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cpp-control-budget-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));auto r=request(root);
    const auto script=r.plugin_root/"node.py",marker=root/"imported.txt";const auto code=juce::File(script.string()).loadFileAsString().toStdString();
    write(script,"from pathlib import Path\nPath(r'"+marker.generic_string()+"').write_text('imported')\n"+code);
    r.parameters={{"mode",std::string(1048577,'x')}};const auto rejected=audioalgo::run_python_task(r);REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(rejected));CHECK(std::get<audioalgo::Diagnostic>(rejected).code=="protocol_message_too_large");CHECK_FALSE(std::filesystem::exists(marker));
    r.parameters={{"mode",std::string("gain")}};const auto recovery=audioalgo::run_python_task(r);REQUIRE(std::holds_alternative<audioalgo::PythonTaskSuccess>(recovery));CHECK(std::filesystem::exists(marker));
}
