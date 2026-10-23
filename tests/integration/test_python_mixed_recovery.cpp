#include "../fixtures/PythonMixedFault.h"
#include <atomic>
#include <thread>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
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
    for(const std::string mode:{"wait","spawn"}) {
        INFO(mode);auto request=fixture.request("cancel-"+mode,mode);std::atomic<bool> cancel=false,done=false;request.is_cancelled=[&]{return cancel.load();};
        std::filesystem::path entered;std::chrono::steady_clock::time_point cancelled_at;
#ifdef _WIN32
        HANDLE parent=nullptr,child=nullptr;
#endif
        std::thread watcher([&]{const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!done.load() && entered.empty() && std::chrono::steady_clock::now()<deadline) {
                std::error_code ec;for(std::filesystem::directory_iterator it(request.output_directory,ec),end;!ec && it!=end;it.increment(ec))if(std::filesystem::exists(it->path()/"entered.txt")){entered=it->path();break;}
                if(entered.empty())std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
#ifdef _WIN32
            if(!entered.empty()){DWORD pid=0;std::ifstream(entered/"entered.txt")>>pid;parent=OpenProcess(SYNCHRONIZE,FALSE,pid);if(mode=="spawn"){std::ifstream(entered/"child-pid.txt")>>pid;child=OpenProcess(SYNCHRONIZE,FALSE,pid);}}
#endif
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
