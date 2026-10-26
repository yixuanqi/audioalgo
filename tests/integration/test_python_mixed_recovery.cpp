#include "../fixtures/PythonMixedFault.h"
#include <atomic>
#include <thread>
#include <fstream>
#include <iostream>
#include <set>
#ifdef _WIN32
#include <windows.h>

namespace {
bool complete_process_marker(const std::filesystem::path& path,DWORD& pid) {
    std::ifstream input(path);std::uint64_t value=0;
    if(!(input>>value) || value==0 || value>MAXDWORD)return false;
    input>>std::ws;if(!input.eof())return false;
    pid=static_cast<DWORD>(value);return true;
}
}

TEST_CASE("PC-03.c mixed cancellation probe refuses an existing incomplete PID marker", "[PC-03.c][PC0604][cancel-probe]") {
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0604-native-text/cancel-probe"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);const auto marker=root/"entered.txt";
    {std::ofstream output(marker);}REQUIRE(std::filesystem::exists(marker));DWORD old_pid=0;std::ifstream(marker)>>old_pid;
    const auto old_handle=OpenProcess(SYNCHRONIZE,FALSE,old_pid);const auto old_error=GetLastError();CHECK(old_pid==0);CHECK(old_handle==nullptr);CHECK(old_error==ERROR_INVALID_PARAMETER);if(old_handle)CloseHandle(old_handle);
    DWORD pid=0;CHECK_FALSE(complete_process_marker(marker,pid));
    for(const auto* invalid:{"0","12suffix","4294967296"}){std::ofstream(marker)<<invalid;CHECK_FALSE(complete_process_marker(marker,pid));}
    const auto pending=root/"entered.pending";{std::ofstream(pending)<<GetCurrentProcessId();}std::filesystem::rename(pending,root/"published.txt");CHECK(complete_process_marker(root/"published.txt",pid));CHECK(pid==GetCurrentProcessId());const auto handle=OpenProcess(SYNCHRONIZE,FALSE,pid);REQUIRE(handle);CHECK(WaitForSingleObject(handle,0)==WAIT_TIMEOUT);CloseHandle(handle);
    std::ofstream(root/"probe.json")<<"{\"old_exists_only_ready\":true,\"old_pid\":0,\"old_open_error\":"<<old_error<<",\"current_pid\":"<<pid<<",\"complete_ready\":true}";std::cout<<"PC0604_CANCEL_PROBE_ROOT="<<root.generic_string()<<'\n';
}
#endif

TEST_CASE("P3-05 mixed failures retain traceback clean temporary output preserve success and recover","[P3-05]") {
    using namespace audioalgo;test::PythonMixedFault fixture;RunService service(fixture.registry);
    const auto baseline=test::mixed_worker(service,fixture.request("baseline","ok"));REQUIRE(std::holds_alternative<RunSuccess>(baseline));
    const auto old=std::get<RunSuccess>(baseline);juce::MemoryBlock old_bytes;REQUIRE(juce::File(old.output_file.string()).loadFileAsData(old_bytes));
    for(const std::string mode:{"exception","missing","dtype","shape","nan","inf","crash","syntax","publish","complete","self-modify"}) {
        INFO(mode);if(mode=="syntax")REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).replaceWithText(fixture.code+"\ndef broken(\n"));
        const auto request=fixture.request("failed-"+mode,mode=="syntax"?"ok":mode);const auto result=test::mixed_worker(service,request);
        const auto* error=std::get_if<Diagnostic>(&result);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(error);
        if(mode=="exception" || mode=="missing" || mode=="syntax") {CHECK(error->code=="python_exception");CHECK(error->node_id=="python");CHECK(error->message.find("Traceback")!=std::string::npos);CHECK(error->message.find("stateful.py")!=std::string::npos);CHECK(error->message.find("line ")!=std::string::npos);}
        if(mode=="dtype" || mode=="shape" || mode=="nan" || mode=="inf") {
            CHECK(error->node_id=="python");CHECK(error->port_id=="main");
            CHECK(error->code==(mode=="dtype"?"python_output_dtype_invalid":mode=="shape"?"python_output_shape_invalid":"python_output_non_finite"));
        }
        if(mode=="self-modify")CHECK(error->code=="python_snapshot_modified");
        if(mode=="crash")CHECK((error->code=="python_runner_exited" || error->code=="python_runner_disconnected"));
        CHECK_FALSE(std::filesystem::exists(request.output_directory/"complete.json"));CHECK_FALSE(std::filesystem::exists(request.output_directory));
        juce::MemoryBlock unchanged;REQUIRE(juce::File(old.output_file.string()).loadFileAsData(unchanged));CHECK(unchanged==old_bytes);CHECK(std::filesystem::exists(old.complete_file));
        REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).replaceWithText(fixture.code));
        const auto recovered=test::mixed_worker(service,fixture.request("recovered-"+mode,"ok"));REQUIRE(std::holds_alternative<RunSuccess>(recovered));CHECK(std::get<RunSuccess>(recovered).output_sha256==old.output_sha256);
    }
    const auto flood=test::mixed_worker(service,fixture.request("flood","flood"));REQUIRE(std::holds_alternative<RunSuccess>(flood));
    const auto report=juce::JSON::parse(juce::File(std::get<RunSuccess>(flood).manifest_file.string()));const auto logs=report["task_artifacts"][0]["provenance"]["logs"];
    CHECK(static_cast<juce::int64>(logs["stdout_bytes"])>1024*1024);CHECK(static_cast<juce::int64>(logs["stderr_bytes"])>1024*1024);
    CHECK(logs["stdout_tail"].toString().getNumBytesAsUTF8()<=65536);CHECK(logs["stderr_tail"].toString().getNumBytesAsUTF8()<=65536);
    for(const std::string mode:{"exception","dtype"}) {
        auto request=fixture.request("cli-"+mode,mode);const auto project=fixture.root/("cli-"+mode+".aaproj");REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project,request.project)));
        juce::ChildProcess cli;REQUIRE(cli.start(juce::StringArray{AUDIOALGO_CLI_EXE,"--plugin-dir",(fixture.root/"plugin").string(),"run",project.string(),request.output_directory.string(),"127"}));
        const auto output=cli.readAllProcessOutput();REQUIRE(cli.waitForProcessToFinish(10000));CHECK(cli.getExitCode()!=0);CHECK(output.contains("node=python"));if(mode=="dtype")CHECK(output.contains("port=main"));
        CHECK_FALSE(std::filesystem::exists(request.output_directory/"complete.json"));
    }
}

TEST_CASE("P3-05 RunService cancellation reclaims real Python process tree within five seconds and recovers","[P3-05][cancel]") {
    using namespace audioalgo;test::PythonMixedFault fixture;RunService service(fixture.registry);
    // Publish the complete process marker atomically. The original fixture
    // exposes exists() before write_text has filled the PID, allowing cancel
    // and task cleanup before the watcher opens its process handle.
    const auto old_marker="(context.run_directory/'entered.txt').write_text(str(os.getpid()))";
    REQUIRE(fixture.code.contains(old_marker));fixture.code=fixture.code.replace(old_marker,"(context.run_directory/'entered.pending').write_text(str(os.getpid()))\n            (context.run_directory/'entered.pending').replace(context.run_directory/'entered.txt')");
    REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).replaceWithText(fixture.code));
    std::cout<<"PC0604_CANCEL_ROOT="<<fixture.root.generic_string()<<'\n';
    for(const std::string mode:{"wait","spawn"}) {
        INFO(mode);auto request=fixture.request("cancel-"+mode,mode);std::atomic<bool> cancel=false,done=false;request.is_cancelled=[&]{return cancel.load();};
        std::filesystem::path entered;std::chrono::steady_clock::time_point cancelled_at;
#ifdef _WIN32
        HANDLE parent=nullptr,child=nullptr;
#endif
        std::thread watcher([&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!done.load() && entered.empty() && std::chrono::steady_clock::now()<deadline) {
                std::error_code ec;for(std::filesystem::directory_iterator it(request.output_directory,ec),end;!ec && it!=end;it.increment(ec)) {
#ifdef _WIN32
                    DWORD parent_pid=0,child_pid=0;
                    if(!complete_process_marker(it->path()/"entered.txt",parent_pid) || (mode=="spawn"&&!complete_process_marker(it->path()/"child-pid.txt",child_pid)))continue;
                    const auto opened_parent=OpenProcess(SYNCHRONIZE,FALSE,parent_pid);if(!opened_parent)continue;
                    const auto opened_child=mode=="spawn"?OpenProcess(SYNCHRONIZE,FALSE,child_pid):nullptr;
                    if(mode=="spawn"&&!opened_child){CloseHandle(opened_parent);continue;}
                    parent=opened_parent;child=opened_child;
#else
                    std::ifstream marker(it->path()/"entered.txt");std::uint64_t pid=0;if(!(marker>>pid)||pid==0)continue;
#endif
                    entered=it->path();break;
                }
                if(entered.empty())std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            cancelled_at=std::chrono::steady_clock::now();cancel=true;
        });
        const auto result=test::mixed_worker(service,request);const auto finished=std::chrono::steady_clock::now();done=true;watcher.join();
        REQUIRE_FALSE(entered.empty());REQUIRE(std::holds_alternative<Diagnostic>(result));CHECK(std::get<Diagnostic>(result).code=="run_cancelled");
        CHECK(finished-cancelled_at<std::chrono::seconds(5));if(mode=="wait")CHECK(finished-cancelled_at<std::chrono::seconds(2));
        CHECK_FALSE(std::filesystem::exists(request.output_directory));
#ifdef _WIN32
        REQUIRE(parent);CHECK(WaitForSingleObject(parent,0)==WAIT_OBJECT_0);CloseHandle(parent);
        if(mode=="spawn"){REQUIRE(child);CHECK(WaitForSingleObject(child,0)==WAIT_OBJECT_0);CloseHandle(child);}
#endif
        const auto recovered=test::mixed_worker(service,fixture.request("after-"+mode,"ok"));REQUIRE(std::holds_alternative<RunSuccess>(recovered));
    }
}

TEST_CASE("PC-05.b mixed output metadata failures identify the exact port preserve success and recover", "[PC-05.b][PC0505]") {
    using namespace audioalgo;test::PythonMixedFault fixture;RunService service(fixture.registry);
    std::cout<<"PC05_METADATA_ROOT="<<fixture.root.generic_string()<<'\n';
    const auto code=fixture.code+R"py(
MetadataBase=Stateful
class Stateful(MetadataBase):
    def process(self,inputs,context):
        result=super().process(inputs,context)
        mode=context.parameters['mode']
        value=result['main']
        if mode in ('rate','channels','interval'):
            print('pc05-metadata='+mode,flush=True)
        if mode=='rate':result['main']=AudioPort(value.samples,16000 if value.sample_rate_hz==48000 else 48000,value.channel_ids,value.valid_interval)
        if mode=='channels':result['main']=AudioPort(value.samples,value.sample_rate_hz,tuple(reversed(value.channel_ids)),value.valid_interval)
        if mode=='interval':result['main']=AudioPort(value.samples,value.sample_rate_hz,value.channel_ids,(0,0))
        return result
)py";
    REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).replaceWithText(code));
    juce::MemoryBlock source_before;REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).loadFileAsData(source_before));REQUIRE(std::filesystem::copy_file(fixture.root/"plugin/stateful.py",fixture.root/"metadata-source-before.py"));
    const auto baseline=test::mixed_worker(service,fixture.request("metadata-baseline","ok"));REQUIRE(std::holds_alternative<RunSuccess>(baseline));const auto old=std::get<RunSuccess>(baseline);juce::MemoryBlock before;REQUIRE(juce::File(old.output_file.string()).loadFileAsData(before));
    juce::Array<juce::var> receipts;
    for(const std::string mode:{"rate","channels","interval"}){
        auto request=fixture.request("metadata-failed-"+mode,mode);request.identity={request.project.project_id,71,juce::Uuid().toString().toStdString(),request.project.revision};std::set<std::string> archives;request.on_log=[&](NodeLogEvent event){archives.insert(event.report_file);};
        const auto result=test::mixed_worker(service,request);const auto* error=std::get_if<Diagnostic>(&result);REQUIRE(error);INFO(mode);INFO(error->code+": "+error->message);CHECK(error->node_id=="python");CHECK(error->port_id=="main");CHECK(error->code==(mode=="interval"?"python_output_interval_invalid":"python_output_format_invalid"));CHECK_FALSE(std::filesystem::exists(request.output_directory));CHECK_FALSE(std::filesystem::exists(request.output_directory/"complete.json"));
        juce::MemoryBlock unchanged;REQUIRE(juce::File(old.output_file.string()).loadFileAsData(unchanged));CHECK(unchanged==before);CHECK(std::filesystem::is_regular_file(old.complete_file));
        const auto recovered=test::mixed_worker(service,fixture.request("metadata-recovered-"+mode,"ok"));REQUIRE(std::holds_alternative<RunSuccess>(recovered));CHECK(std::get<RunSuccess>(recovered).output_sha256==old.output_sha256);
        REQUIRE(archives.size()==1);CHECK_FALSE(archives.begin()->empty());juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("mode",juce::String(mode));row->setProperty("code",juce::String(error->code));row->setProperty("node",juce::String(error->node_id));row->setProperty("port",juce::String(error->port_id));row->setProperty("request",juce::String(request.identity.request_id));row->setProperty("archive",juce::String(*archives.begin()));row->setProperty("failed",juce::String(request.output_directory.filename().string()));row->setProperty("recovered",juce::String(std::get<RunSuccess>(recovered).output_file.parent_path().filename().string()));receipts.add(juce::var(row.get()));
    }
    juce::MemoryBlock source_after;REQUIRE(juce::File((fixture.root/"plugin/stateful.py").string()).loadFileAsData(source_after));CHECK(source_after==source_before);std::ofstream(fixture.root/"metadata-demo.json")<<juce::JSON::toString(receipts,false).toStdString();
}
