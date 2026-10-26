#include "audio_run_service/RunService.h"
#include "audio_run_service/MixedExecution.h"
#include "audio_runtime/NativePlan.h"
#include "tests/fixtures/IndependentTailStream.h"
#include "audio_io/WavFile.h"
#include "plugins/host/registration.h"
#include "plugins/builtin/registration.h"
#include "EditorNodeInspector.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <future>
#include <cmath>
#include <array>
#include <random>
#include <fstream>
#include <iostream>

namespace {
audioalgo::RunOutcome worker_run(const audioalgo::RunService& service,const audioalgo::RunRequest& request) {
    auto future=std::async(std::launch::async,[&]{auto result=service.run(request);
        juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;});
    juce::MessageManager::getInstance()->runDispatchLoop();return future.get();
}
template<class T>T* control(juce::Component& c,const juce::String& id) {
    if(c.getComponentID()==id)return dynamic_cast<T*>(&c);
    for(auto* child:c.getChildren())if(auto* found=control<T>(*child,id))return found;return nullptr;
}
struct Count {int prepares=0,calls=0;};
class CountingNative final:public audioalgo::AudioNodeProcessor {
public:
    explicit CountingNative(std::shared_ptr<Count> c):count_(std::move(c)){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"unsupported","block only"};}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override{++count_->prepares;return {};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        ++count_->calls;for(std::size_t c=0;c<b.channel_count;++c)for(std::size_t i=0;i<b.frame_count;++i)b.output_channels[c][i]=b.input_channels[c][i]*2;
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:std::shared_ptr<Count> count_;
};
}

TEST_CASE("P3-03 RunService executes native Python native stateful chain with exact owned boundaries","[P3-03]") {
    using namespace audioalgo;
    const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR);const auto example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    auto loaded=load_project_file(example/"mixed.aaproj");REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto root=source/"build/runs"/("mixed-chain-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    AudioBuffer input(48000,{"left","right"},1025);for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i)input.mutable_channel(c)[i]=static_cast<float>(.125*std::sin((i+1)*(c+1)*.03));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    auto project=std::get<ProjectModel>(loaded);REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
    RunService service(registry);
    for(const int host:{1,127,512}) {
        juce::ScopedJuceInitialiser_GUI messages;
        RunRequest request{project,root/"mixed.aaproj",root/ std::to_string(host),host};
        auto future=std::async(std::launch::async,[&]{auto result=service.run(request);
            juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;});
        juce::MessageManager::getInstance()->runDispatchLoop();
        const auto result=future.get();
        const auto* error=std::get_if<Diagnostic>(&result);INFO(host);INFO((error?error->code:"success"));INFO((error?error->message:""));
        REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto decoded=read_wav_file(std::get<RunSuccess>(result).output_file);REQUIRE(std::holds_alternative<AudioBuffer>(decoded));const auto& output=std::get<AudioBuffer>(decoded);
        REQUIRE(output.frame_count()==1025);REQUIRE(output.channel_count()==2);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i) {
            const auto pre=static_cast<float>(input.channel(c)[i]*2.0f);
            const auto state=static_cast<float>(static_cast<float>(pre*.75f)+static_cast<float>(static_cast<float>(i)*.00001f));
            const double expected=static_cast<float>(state*.5f);
            CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        REQUIRE(std::filesystem::exists(request.output_directory/"complete.json"));
    }
}

TEST_CASE("PC-05.b mismatched mixed sources identify their output port before any factory", "[PC-05.b][PC0506]") {
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0506-source-port/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC05_SOURCE_ROOT="<<root.generic_string()<<'\n';
    struct OfflineAdd final:AudioNodeProcessor{NodeProcessOutcome process(const NodeProcessRequest& request)override{auto output=request.inputs.at("primary");const auto& other=request.inputs.at("reference");for(std::size_t c=0;c<output.channel_count();++c)for(std::size_t n=0;n<output.frame_count();++n)output.mutable_channel(c)[n]+=other.channel(c)[n];return AudioPorts{{"main",std::move(output)}};}};
    NodeRegistry registry;REQUIRE(register_builtin_nodes(registry));int factories=0;auto descriptor=*registry.find("builtin.gain");descriptor.type_id="fixture.pc0506_add";descriptor.parameters.clear();descriptor.ports={{"primary","audio.pcm@1",PortDirection::input,true},{"reference","audio.pcm@1",PortDirection::input,true},{"main","audio.pcm@1",PortDirection::output}};descriptor.capabilities.supports_named_audio_ports=true;descriptor.capabilities.requires_offline_task=true;REQUIRE(registry.register_node(descriptor,[&]{++factories;return std::make_unique<OfflineAdd>();}).registered);
    ProjectModel project;project.project_id="pc0506-source-preflight";project.selected_output_node_id="join";project.selected_output_port_id="main";project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},{"b","builtin.file_source",{{"path",std::string("b.wav")}}},{"join",descriptor.type_id,{}}};project.connections={{"a","audio","join","primary"},{"b","audio","join","reference"}};REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"project.aaproj",project)));
    AudioBuffer a(48000,{"left","right"},513);for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<513;++n)a.mutable_channel(c)[n]=static_cast<float>((static_cast<int>((3*n+7*c)%29)-14)/64.0);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"a.wav",a,WavEncoding::float32)));
    struct Variant{const char* kind;AudioBuffer audio;};std::vector<Variant> variants{{"rate",AudioBuffer(16000,{"left","right"},513)},{"channels",AudioBuffer(48000,{"mono"},513)},{"channel_ids",AudioBuffer(48000,{"right","left"},513)},{"frames",AudioBuffer(48000,{"left","right"},512)},{"interval",AudioBuffer(48000,{"left","right"},513,AudioInterval{1,512})}};
    juce::Array<juce::var> receipts;
    for(auto& variant:variants){for(std::size_t c=0;c<variant.audio.channel_count();++c)for(std::size_t n=0;n<variant.audio.frame_count();++n)variant.audio.mutable_channel(c)[n]=static_cast<float>((static_cast<int>((11*n+5*c)%31)-15)/128.0);const auto file=root/(std::string(variant.kind)+".wav");REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(file,variant.audio,WavEncoding::float32)));const auto hash=juce::SHA256(juce::File(file.generic_string())).toHexString();
        auto future=std::async(std::launch::async,[&]{return render_prepared_audio_graph(project,registry,{{"a",a},{"b",variant.audio}},127,{},nullptr,nullptr);});const auto result=future.get();const auto* error=std::get_if<Diagnostic>(&result);REQUIRE(error);INFO(variant.kind);CHECK(error->code=="source_format_mismatch");CHECK(error->node_id=="b");CHECK(error->port_id=="audio");CHECK(factories==0);CHECK(juce::SHA256(juce::File(file.generic_string())).toHexString()==hash);
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("kind",variant.kind);row->setProperty("code",juce::String(error->code));row->setProperty("node",juce::String(error->node_id));row->setProperty("port",juce::String(error->port_id));row->setProperty("factories",factories);row->setProperty("input_sha256",hash);row->setProperty("rate",static_cast<int>(variant.audio.sample_rate_hz()));row->setProperty("channels",static_cast<int>(variant.audio.channel_count()));row->setProperty("frames",static_cast<int>(variant.audio.frame_count()));row->setProperty("valid_start",static_cast<int>(variant.audio.valid_interval().first_frame));row->setProperty("valid_count",static_cast<int>(variant.audio.valid_interval().frame_count));juce::Array<juce::var> ids;for(const auto& id:variant.audio.channel_ids())ids.add(juce::String(id));row->setProperty("channel_ids",ids);receipts.add(juce::var(row.get()));
    }
    auto future=std::async(std::launch::async,[&]{return render_prepared_audio_graph(project,registry,{{"a",a},{"b",a}},127,{},nullptr,nullptr);});const auto result=future.get();if(auto* error=std::get_if<Diagnostic>(&result))INFO(error->code+": "+error->message);REQUIRE(std::holds_alternative<AudioBuffer>(result));CHECK(factories==1);const auto& output=std::get<AudioBuffer>(result);REQUIRE(output.frame_count()==513);CHECK(output.channel_ids()==a.channel_ids());CHECK(output.valid_interval()==a.valid_interval());for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<513;++n)CHECK(output.channel(c)[n]==a.channel(c)[n]*2.f);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"control.wav",output,WavEncoding::float32)));std::ofstream(root/"source-probe.json")<<juce::JSON::toString(receipts,false).toStdString();
}

TEST_CASE("P3-03 mixed instances clip multioutput and shared native branches preserve single execution","[P3-03]") {
    using namespace audioalgo;const std::string profile=GENERATE("two","side","clip","fork");INFO(profile);
    juce::ScopedJuceInitialiser_GUI messages;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    const auto count=std::make_shared<Count>();auto descriptor=*registry.find("builtin.gain");descriptor.type_id="fixture.counted";descriptor.parameters.clear();
    REQUIRE(registry.register_node(descriptor,[count]{return std::make_unique<CountingNative>(count);}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=descriptor.type_id;project.nodes[1].parameters.clear();
    if(profile=="two") {
        project.nodes.push_back({"second","example.python_stateful",{{"scale",.25},{"step",.00002}}});
        project.connections[2].from_node="second";project.connections.push_back({"python","main","second","audio_in"});
    }
    if(profile=="side")project.connections[2].from_port="side";
    if(profile=="clip")project.nodes[2].type_id="example.python_clip";
    if(profile=="fork") {
        project.nodes.push_back({"mix","builtin.mixer",{}});project.connections.back().from_node="mix";project.connections.back().from_port="sum";
        project.connections.push_back({"before","audio_out","mix","secondary"});project.connections.push_back({"after","audio_out","mix","primary"});
    }
    const auto root=source/"build/runs"/("mixed-profiles-"+profile+"-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
    RunService service(registry);const auto result=worker_run(service,{project,root/"mixed.aaproj",root/"run",127});
    const auto* error=std::get_if<Diagnostic>(&result);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
    const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav")),output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));
    CHECK(count->prepares==1);CHECK(count->calls==9);
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i) {
        const float pre=input.channel(c)[i]*2;float state=static_cast<float>(pre*.75f)+static_cast<float>(static_cast<float>(i)*.00001f);
        if(profile=="two")state=static_cast<float>(state*.25f)+static_cast<float>(static_cast<float>(i)*.00002f);
        if(profile=="side")state=-state;
        float expected=state*.5f;if(profile=="fork")expected+=pre;
        CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
    }
    int artifacts=0;for(const auto& item:std::filesystem::directory_iterator(root/"run"))if(item.path().filename().string().starts_with("python-task-")) {
        const auto lifecycle=juce::JSON::parse(juce::File((item.path()/"lifecycle.json").string()));
        CHECK(static_cast<int>(lifecycle["prepares"])==1);CHECK(static_cast<int>(lifecycle["calls"])==(profile=="clip"?1:9));++artifacts;
    }
    CHECK(artifacts==(profile=="two"?2:1));
}

TEST_CASE("P3-03 generated Python parameter control changes controller worker output and report","[P3-03][component]") {
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI messages;
    const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    const auto root=source/"build/runs"/("mixed-component-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
    editor::EditorController controller(registry);controller.adopt_project(std::get<ProjectModel>(load_project_file(example/"mixed.aaproj")));
    editor::NodeSelectionState selection;editor::NodeInspector inspector(controller,selection,[]{});inspector.set_descriptor_registry(registry);inspector.show_node("python");
    auto* slider=control<juce::Slider>(inspector,"parameter.scale");REQUIRE(slider);slider->onDragStart();slider->setValue(.5,juce::sendNotificationSync);slider->onDragEnd();
    REQUIRE(std::get<double>(controller.node_snapshot("python")->node.parameters.at("scale"))==.5);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",controller.project())));
    RunService service(registry);const auto result=worker_run(service,{controller.project(),root/"mixed.aaproj",root/"run",127});
    const auto* error=std::get_if<Diagnostic>(&result);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
    const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav")),output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i){const float expected=static_cast<float>(static_cast<float>(input.channel(c)[i]*2)*.5f+static_cast<float>(static_cast<float>(i)*.00001f))*.5f;CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));}
    const auto saved=std::get<ProjectModel>(load_project_file(root/"run/project.aaproj"));CHECK(std::get<double>(saved.nodes[2].parameters.at("scale"))==.5);
    // Direct UI-thread execution is refused before a Python process can start.
    const auto refused=service.run({controller.project(),root/"mixed.aaproj",root/"ui-refused",127});REQUIRE(std::holds_alternative<Diagnostic>(refused));CHECK(std::get<Diagnostic>(refused).code=="offline_thread_invalid");CHECK_FALSE(std::filesystem::exists(root/"ui-refused/complete.json"));
}

TEST_CASE("P3-03 task byte limit rejects before Python payload allocation and import","[P3-03][budget]") {
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI messages;
    const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    const auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));
    const auto root=source/"build/runs"/("mixed-budget-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
    RunRequest request{project,root/"mixed.aaproj",root/"rejected",127};request.maximum_task_payload_bytes=4096;
    RunService service(registry);const auto result=worker_run(service,request);
    REQUIRE(std::holds_alternative<Diagnostic>(result));CHECK(std::get<Diagnostic>(result).code=="python_payload_limit");CHECK(std::get<Diagnostic>(result).node_id=="python");
    CHECK_FALSE(std::filesystem::exists(request.output_directory));
}

namespace {
class MixedFixedScale final:public audioalgo::AudioNodeProcessor {
    std::size_t frame_;float gain_;
public:
    MixedFixedScale(std::size_t frame,float gain):frame_(frame),gain_(gain){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"owned_unsupported","Use fixed blocks"};}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override{
        if(c.maximum_block_frames!=frame_)return audioalgo::Diagnostic{"fixed_prepare_size","Wrong fixed frame"};return {};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override{
        if(b.frame_count!=frame_)return audioalgo::Diagnostic{"fixed_frame_size","Wrong fixed frame"};
        for(std::size_t c=0;c<b.channel_count;++c)for(std::size_t i=0;i<frame_;++i)b.output_channels[c][i]=b.input_channels[c][i]*gain_;
        return audioalgo::AudioBlockResult{frame_};
    }
};
}
TEST_CASE("P3 timeline fixed480 Python fixed512 aligns each owned boundary exactly once","[P3-timeline][mixed-fixed]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    for(const auto frame:{480,512}) {
        auto d=*registry.find("builtin.gain");d.type_id="fixture.mixed_fixed"+std::to_string(frame);d.parameters.clear();
        d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=frame;d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
        REQUIRE(registry.register_node(d,[frame]{return std::make_unique<MixedFixedScale>(frame,frame==480?2.f:.5f);}).registered);
    }
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id="fixture.mixed_fixed480";project.nodes[1].parameters.clear();project.nodes[3].type_id="fixture.mixed_fixed512";project.nodes[3].parameters.clear();
    const auto root=source/"build/runs"/("mixed-fixed-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
    const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));RunService service(registry);
    for(const int host:{1,127,256,480,512,1024}) {
        juce::ScopedJuceInitialiser_GUI messages;const auto run=root/std::to_string(host);const auto result=worker_run(service,{project,root/"mixed.aaproj",run,host});
        const auto* e=std::get_if<Diagnostic>(&result);INFO(host);INFO((e?e->code:"success"));INFO((e?e->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));REQUIRE(output.frame_count()==1025);REQUIRE(output.channel_count()==2);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i){const float expected=(input.channel(c)[i]*2.f*.75f+static_cast<float>(i)*.00001f)*.5f;CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));}
        const auto report=juce::JSON::parse(juce::File((run/"run.json").string()));const auto before=report["tasks"][1]["alignment"],after=report["tasks"][3]["alignment"];
        CHECK(static_cast<int>(before["offset_frames"])==479);CHECK(static_cast<int>(before["raw_frames"])==1504);CHECK(static_cast<int>(before["aligned_frames"])==1025);
        CHECK(static_cast<int>(after["offset_frames"])==511);CHECK(static_cast<int>(after["raw_frames"])==1536);CHECK(static_cast<int>(after["aligned_frames"])==1025);
        CHECK(static_cast<int>(report["alignment_offset_frames"])==0);CHECK(static_cast<int>(report["frames"])==1025);
    }
}

namespace {
class MixedFiniteFir final:public audioalgo::AudioNodeProcessor {
    std::vector<std::array<float,2>> history_;
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"owned_unsupported","Use finite fixed blocks"};}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override{history_.assign(c.format.channel_count,{});return {};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override{
        if(b.frame_count!=480)return audioalgo::Diagnostic{"fixed_frame_size","Expected 480 frames"};
        for(std::size_t c=0;c<b.channel_count;++c)for(std::size_t i=0;i<b.frame_count;++i){const auto x=b.input_channels[c][i];b.output_channels[c][i]=x+history_[c][0]*.5f+history_[c][1]*.25f;history_[c][1]=history_[c][0];history_[c][0]=x;}
        return audioalgo::AudioBlockResult{b.frame_count};
    }
};
}
TEST_CASE("P3 timeline finite FIR tail crosses Python and native output with exact extended valid interval","[P3-timeline][mixed-tail]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());auto d=*registry.find("builtin.gain");d.type_id="fixture.mixed_finite_fir";d.parameters.clear();
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.tail_frames=2;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;d.capabilities.maximum_zero_padding_frames=481;
    REQUIRE(registry.register_node(d,[]{return std::make_unique<MixedFiniteFir>();}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();
    const auto root=source/"build/runs"/("mixed-tail-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));RunService service(registry);
    for(const int host:{1,127,256,480,512,1024}) {
        juce::ScopedJuceInitialiser_GUI messages;const auto run=root/std::to_string(host);const auto result=worker_run(service,{project,root/"mixed.aaproj",run,host});
        const auto* e=std::get_if<Diagnostic>(&result);INFO(host);INFO((e?e->code:"success"));INFO((e?e->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));REQUIRE(output.frame_count()==1027);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1027;++i) {
            const auto x=[&](std::size_t n){return n<1025?input.channel(c)[n]:0.f;};
            const float fir=x(i)+(i>=1?x(i-1)*.5f:0.f)+(i>=2?x(i-2)*.25f:0.f);
            const float expected=(fir*.75f+static_cast<float>(i)*.00001f)*.5f;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        const auto report=juce::JSON::parse(juce::File((run/"run.json").string()));CHECK(static_cast<int>(report["source_frames"])==1025);CHECK(static_cast<int>(report["retained_tail_frames"])==2);CHECK(static_cast<int>(report["valid_frame_count"])==1027);
        CHECK(static_cast<int>(report["tasks"][1]["alignment"]["raw_frames"])==1506);CHECK(static_cast<int>(report["tasks"][1]["alignment"]["aligned_frames"])==1027);
        const auto interval=report["task_artifacts"][0]["provenance"]["outputs"]["main"]["valid_interval"];CHECK(static_cast<int>(interval[0])==0);CHECK(static_cast<int>(interval[1])==1027);
    }
}

TEST_CASE("P3 timeline unknown latency tail flush and independent tail outputs reject before factories","[P3-timeline][mixed-admission]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    for(const std::string kind:{"latency","tail","flush","independent"}) {
        INFO(kind);NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());auto d=*registry.find("builtin.gain");d.type_id="fixture.tail_admission";d.parameters.clear();
        d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
        if(kind=="latency")d.capabilities.latency_frames.reset();
        if(kind=="tail")d.capabilities.tail_frames.reset();
        if(kind=="flush")d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::requires_flush;
        if(kind=="independent") {d.capabilities.tail_frames=2;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;d.capabilities.maximum_zero_padding_frames=481;
            d.capabilities.supports_named_audio_ports=true;d.ports.push_back({"side","audio.pcm@1",PortDirection::output});}
        int created=0;REQUIRE(registry.register_node(d,[&]{++created;return std::make_unique<MixedFiniteFir>();}).registered);
        auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();
        if(kind=="independent") {project.nodes.push_back({"mix","builtin.mixer",{}});project.connections.back().from_node="mix";project.connections.back().from_port="sum";
            project.connections.push_back({"after","audio_out","mix","primary"});project.connections.push_back({"before","side","mix","secondary"});}
        const auto root=source/"build/runs"/("mixed-admission-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));RunService service(registry);juce::ScopedJuceInitialiser_GUI messages;
        const auto result=worker_run(service,{project,root/"mixed.aaproj",root/"rejected",127});REQUIRE(std::holds_alternative<Diagnostic>(result));
        const auto expected=kind=="latency"?"latency_unknown":kind=="tail"?"tail_unknown":kind=="flush"?"flush_unsupported":"mixed_tail_ports_unsupported";
        CHECK(std::get<Diagnostic>(result).code==expected);CHECK(std::get<Diagnostic>(result).node_id=="before");CHECK(created==0);CHECK_FALSE(std::filesystem::exists(root/"rejected"));
    }
}

TEST_CASE("P3 timeline finite tail preserves leading trailing and empty intervals on both Python outputs","[P3-timeline][mixed-interval]") {
    using namespace audioalgo;
    const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    auto d=*registry.find("builtin.gain");d.type_id="fixture.interval_fir";d.parameters.clear();
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;
    d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.tail_frames=2;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
    d.capabilities.maximum_zero_padding_frames=481;
    REQUIRE(registry.register_node(d,[]{return std::make_unique<MixedFiniteFir>();}).registered);
    const auto root=source/"build/runs"/("mixed-interval-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));
    // Explicit boundary expectations: only a nonempty valid suffix reaching EOS gains tail.
    const std::array<AudioInterval,4> input_intervals{{{7,1018},{7,1000},{7,0},{1025,0}}};
    const std::array<AudioInterval,4> output_intervals{{{7,1020},{7,1000},{7,0},{1025,0}}};
    for(std::size_t profile=0;profile<input_intervals.size();++profile)for(const std::string port:{"main","side"}) {
        auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));
        project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();project.connections[2].from_port=port;
        AudioBuffer input(48000,{"left","right"},1025,input_intervals[profile]);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i)
            input.mutable_channel(c)[i]=static_cast<float>(.125*std::sin((i+1)*(c+1)*.03));
        const auto run=root/(std::to_string(profile)+"-"+port);REQUIRE(std::filesystem::create_directory(run));
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(run/"input.wav",input,WavEncoding::float32)));
        NodeExecutionContext context;context.project_file=(example/"mixed.aaproj").string();context.output_directory=run.string();
        context.identity={project.project_id,project.revision,juce::Uuid().toString().toStdString(),1};
        juce::Array<juce::var> artifacts;
        context.record_task_artifact=[&](std::string directory,std::string provenance){
            juce::DynamicObject::Ptr item=new juce::DynamicObject;
            item->setProperty("directory",juce::String::fromUTF8(directory.c_str()));item->setProperty("provenance",juce::JSON::parse(provenance));
            artifacts.add(juce::var(item.get()));
        };
        NativeRenderStatistics statistics;std::string tasks;
        juce::ScopedJuceInitialiser_GUI messages;
        auto future=std::async(std::launch::async,[&]{
            auto result=render_prepared_audio_graph(project,registry,{{"file",input}},127,context,&statistics,&tasks);
            juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;
        });
        juce::MessageManager::getInstance()->runDispatchLoop();const auto result=future.get();
        const auto* error=std::get_if<Diagnostic>(&result);INFO(profile);INFO(port);INFO((error?error->code:"success"));INFO((error?error->message:""));
        REQUIRE(std::holds_alternative<AudioBuffer>(result));const auto& output=std::get<AudioBuffer>(result);
        REQUIRE(output.frame_count()==1027);CHECK(output.valid_interval()==output_intervals[profile]);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1027;++i) {
            const auto x=[&](std::size_t n){return n<1025?input.channel(c)[n]:0.f;};
            const float fir=x(i)+(i>=1?x(i-1)*.5f:0.f)+(i>=2?x(i-2)*.25f:0.f);
            const float state=fir*.75f+static_cast<float>(i)*.00001f;
            const float expected=(port=="main"?state:-state)*.5f;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        const auto task_values=juce::JSON::parse(tasks);
        for(const auto& value:*task_values.getArray())for(const auto& output_value:*value["outputs"].getArray()) {
            if(output_value["node_id"].toString()=="file")continue;
            CHECK(static_cast<int>(output_value["valid_start_frame"])==static_cast<int>(output_intervals[profile].first_frame));
            CHECK(static_cast<int>(output_value["valid_frame_count"])==static_cast<int>(output_intervals[profile].frame_count));
        }
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(run/"output.wav",output,WavEncoding::float32)));
        juce::DynamicObject::Ptr report=new juce::DynamicObject;
        report->setProperty("profile",static_cast<int>(profile));report->setProperty("selected_port",juce::String::fromUTF8(port.c_str()));
        report->setProperty("tasks",task_values);report->setProperty("task_artifacts",artifacts);
        REQUIRE(juce::File((run/"boundary-report.json").string()).replaceWithText(juce::JSON::toString(juce::var(report.get()))));
    }
}

namespace {
class IndependentTailPair final:public audioalgo::AudioNodeProcessor {
    std::shared_ptr<Count> count_;std::vector<std::array<float,5>> history_;
public:
    explicit IndependentTailPair(std::shared_ptr<Count> count):count_(std::move(count)){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"owned_unsupported","Use named fixed blocks"};}
    bool supports_block_processing() const noexcept override{return true;}
    bool supports_named_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override{++count_->prepares;history_.assign(c.format.channel_count,{});return {};}
    audioalgo::AudioBlockOutcome process_named_block(const audioalgo::NamedAudioBlockView& b) override {
        ++count_->calls;
        for(std::size_t c=0;c<history_.size();++c)for(std::size_t i=0;i<b.frame_count;++i) {
            const auto x=b.inputs[0].channels[c][i];
            b.outputs[0].channels[c][i]=x+history_[c][1]*.25f;
            b.outputs[1].channels[c][i]=x*2.f+history_[c][4]*.125f;
            for(std::size_t k=4;k>0;--k)history_[c][k]=history_[c][k-1];history_[c][0]=x;
        }
        return audioalgo::AudioBlockResult{b.frame_count};
    }
};
}
TEST_CASE("P3 timeline independent native output tails reach Python with exact selected lengths","[P3-timeline][mixed-port-tail]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    auto d=*registry.find("builtin.gain");d.type_id="fixture.independent_tails";d.parameters.clear();
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;
    d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.supports_named_audio_ports=true;d.ports.push_back({"side","audio.pcm@1",PortDirection::output});
    d.capabilities.tail_frames=5;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
    d.capabilities.maximum_zero_padding_frames=484;d.capabilities.output_tail_frames={{"audio_out",2},{"side",5}};
    const auto count=std::make_shared<Count>();REQUIRE(registry.register_node(d,[count]{return std::make_unique<IndependentTailPair>(count);}).registered);
    const auto root=source/"build/runs"/("mixed-port-tail-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));
    for(const std::string selected:{"audio_out","side"})for(const int host:{1,127,256,480,512,1024}) {
        auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();project.connections[1].from_port=selected;
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
        const auto previous_prepares=count->prepares,previous_calls=count->calls;
        juce::ScopedJuceInitialiser_GUI messages;RunService service(registry);const auto run=root/(selected+"-"+std::to_string(host));
        const auto result=worker_run(service,{project,root/"mixed.aaproj",run,host});const auto* error=std::get_if<Diagnostic>(&result);
        INFO(selected);INFO(host);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));const std::size_t frames=selected=="audio_out"?1027:1030;
        REQUIRE(output.frame_count()==frames);CHECK(count->prepares-previous_prepares==1);CHECK(count->calls-previous_calls==3);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<frames;++i) {
            const auto x=[&](std::size_t n){return n<1025?input.channel(c)[n]:0.f;};
            const auto delay=selected=="audio_out"?2u:5u;
            const float fir=x(i)*(selected=="audio_out"?1.f:2.f)+(i>=delay?x(i-delay)*(selected=="audio_out"?.25f:.125f):0.f);
            const float expected=(fir*.75f+static_cast<float>(i)*.00001f)*.5f;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        const auto report=juce::JSON::parse(juce::File((run/"run.json").string()));const auto alignment=report["tasks"][1]["alignment"];
        CHECK(static_cast<int>(alignment["offset_frames"])==479);CHECK(static_cast<int>(alignment["aligned_frames"])==static_cast<int>(frames));
        CHECK(static_cast<int>(alignment["raw_frames"])==static_cast<int>(frames+479));CHECK(static_cast<int>(report["valid_frame_count"])==static_cast<int>(frames));
    }
}

TEST_CASE("P3 timeline invalid per-output tail contracts reject before factories","[P3-timeline][mixed-port-admission]") {
    using namespace audioalgo;NodeRegistry registry;REQUIRE(register_host_nodes(registry,{}).diagnostics.empty());
    for(const std::string kind:{"missing","extra","maximum","stream","variable","unnamed"}) {
        INFO(kind);auto d=*registry.find("builtin.gain");d.parameters.clear();
        d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;
        d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
        d.capabilities.supports_named_audio_ports=true;d.ports.push_back({"side","audio.pcm@1",PortDirection::output});
        d.capabilities.tail_frames=5;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
        d.capabilities.maximum_zero_padding_frames=484;d.capabilities.output_tail_frames={{"audio_out",2},{"side",5}};
        if(kind=="missing")d.capabilities.output_tail_frames.erase("side");
        if(kind=="extra")d.capabilities.output_tail_frames.emplace("audio_in",1);
        if(kind=="maximum")d.capabilities.tail_frames=6;
        if(kind=="stream"){d.capabilities.uses_streaming_frame_api=true;d.capabilities.maximum_no_progress_calls=0;}
        if(kind=="variable")d.capabilities.supports_variable_block_size=true;
        if(kind=="unnamed")d.capabilities.supports_named_audio_ports=false;
        const auto error=validate_native_plan_node(d,"pair",{48000,2},1025,127);REQUIRE(error.has_value());
        CHECK(error->code==((kind=="missing" || kind=="extra" || kind=="maximum")?"tail_ports_invalid":kind=="stream"?"stream_progress_budget_invalid":"tail_ports_profile_unsupported"));
        CHECK(error->node_id=="pair");
    }
}

namespace {
class UncalledIntervalJoin final:public audioalgo::AudioNodeProcessor {
    std::shared_ptr<int> calls_;
public:
    explicit UncalledIntervalJoin(std::shared_ptr<int> calls):calls_(std::move(calls)){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{++*calls_;return audioalgo::Diagnostic{"join_must_not_run","Input intervals must be checked first"};}
};
}
TEST_CASE("P3 timeline two native tail ports materialize once before incompatible join rejects","[P3-timeline][mixed-port-join]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{}).diagnostics.empty());auto d=*registry.find("builtin.gain");
    d.type_id="fixture.independent_join_tails";d.parameters.clear();d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;
    d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;d.capabilities.supports_named_audio_ports=true;
    d.ports.push_back({"side","audio.pcm@1",PortDirection::output});d.capabilities.tail_frames=5;
    d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;d.capabilities.maximum_zero_padding_frames=484;
    d.capabilities.output_tail_frames={{"audio_out",2},{"side",5}};const auto count=std::make_shared<Count>();
    REQUIRE(registry.register_node(d,[count]{return std::make_unique<IndependentTailPair>(count);}).registered);
    NodeDescriptor join{"fixture.interval_join","1","Interval join",{{"a","audio.pcm@1",PortDirection::input,true},{"b","audio.pcm@1",PortDirection::input,true},{"sum","audio.pcm@1",PortDirection::output}}, {}};
    join.capabilities.requires_offline_task=true;const auto calls=std::make_shared<int>(0);
    REQUIRE(registry.register_node(join,[calls]{return std::make_unique<UncalledIntervalJoin>(calls);}).registered);
    ProjectModel project;project.project_id="independent-join";project.revision=1;project.selected_output_node_id="output";
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},{"pair",d.type_id,{}},{"join",join.type_id,{}},{"output","builtin.output",{}}};
    project.connections={{"file","audio","pair","audio_in"},{"pair","audio_out","join","a"},{"pair","side","join","b"},{"join","sum","output","audio"}};
    const auto root=source/"build/runs"/("mixed-port-join-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));
    NodeExecutionContext context;context.output_directory=root.string();context.identity={project.project_id,1,juce::Uuid().toString().toStdString(),1};
    juce::ScopedJuceInitialiser_GUI messages;auto future=std::async(std::launch::async,[&]{
        auto result=render_prepared_audio_graph(project,registry,{{"file",input}},127,context,nullptr,nullptr);
        juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;
    });juce::MessageManager::getInstance()->runDispatchLoop();const auto result=future.get();
    REQUIRE(std::holds_alternative<Diagnostic>(result));CHECK(std::get<Diagnostic>(result).code=="mixed_input_timeline_mismatch");
    CHECK(std::get<Diagnostic>(result).node_id=="join");CHECK(*calls==0);CHECK(count->prepares==1);CHECK(count->calls==3);
    for(int p=0;p<2;++p) {
        const auto prefix=root/"native-regions/1"/("port-"+std::to_string(p));
        const auto raw=std::get<AudioBuffer>(read_wav_file(prefix.string()+".raw.wav")),aligned=std::get<AudioBuffer>(read_wav_file(prefix.string()+".aligned.wav"));
        const std::size_t frames=p==0?1027:1030;CHECK(raw.frame_count()==frames+479);CHECK(aligned.frame_count()==frames);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<frames;++i) {
            const auto x=[&](std::size_t n){return n<1025?input.channel(c)[n]:0.f;};const auto delay=p==0?2u:5u;
            const float expected=x(i)*(p==0?1.f:2.f)+(i>=delay?x(i-delay)*(p==0?.25f:.125f):0.f);
            REQUIRE(aligned.channel(c)[i]==expected);REQUIRE(raw.channel(c)[i+479]==expected);
        }
    }
    REQUIRE(juce::File((root/"probe.json").string()).replaceWithText("{\"code\":\"mixed_input_timeline_mismatch\",\"node_id\":\"join\",\"join_calls\":0,\"pair_prepares\":1,\"pair_calls\":3}"));
}

namespace {
struct PartitionTrace {std::array<std::size_t,512> frames{};std::size_t count=0;};
class HostPartitionProbe final:public audioalgo::AudioNodeProcessor {
    std::shared_ptr<PartitionTrace> trace_;
public:
    explicit HostPartitionProbe(std::shared_ptr<PartitionTrace> trace):trace_(std::move(trace)){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"owned_unsupported","Use native blocks"};}
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(trace_->count==trace_->frames.size())return audioalgo::Diagnostic{"trace_capacity","Too many blocks"};
        trace_->frames[trace_->count++]=b.frame_count;
        for(std::size_t c=0;c<b.channel_count;++c)std::copy_n(b.input_channels[c],b.frame_count,b.output_channels[c]);
        return audioalgo::AudioBlockResult{b.frame_count};
    }
};
}
TEST_CASE("P3 timeline seeded mixed host partitions reach native callbacks and replay finite tails exactly","[P3-timeline][mixed-random]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
    auto fir=*registry.find("builtin.gain");fir.type_id="fixture.partition_fir";fir.parameters.clear();
    fir.capabilities.supports_variable_block_size=false;fir.capabilities.fixed_block_frames=480;
    fir.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    fir.capabilities.tail_frames=2;fir.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;fir.capabilities.maximum_zero_padding_frames=481;
    REQUIRE(registry.register_node(fir,[]{return std::make_unique<MixedFiniteFir>();}).registered);
    auto after=fir;after.type_id="fixture.partition_after";after.capabilities.fixed_block_frames=512;after.capabilities.tail_frames=0;
    after.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::none;after.capabilities.maximum_zero_padding_frames.reset();
    REQUIRE(registry.register_node(after,[]{return std::make_unique<MixedFixedScale>(512,.5f);}).registered);
    auto probe=*registry.find("builtin.gain");probe.type_id="fixture.partition_probe";probe.parameters.clear();
    const auto trace=std::make_shared<PartitionTrace>();REQUIRE(registry.register_node(probe,[trace]{return std::make_unique<HostPartitionProbe>(trace);}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=fir.type_id;project.nodes[1].parameters.clear();
    project.nodes[3].type_id=after.type_id;project.nodes[3].parameters.clear();project.nodes.push_back({"probe",probe.type_id,{}});
    project.connections[0].to_node="probe";project.connections.push_back({"probe","audio_out","before","audio_in"});
    const auto root=source/"build/runs"/("mixed-random-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));
    std::vector<int> partitions{127,1};std::mt19937 random(20261023);
    for(int i=0;i<173;++i)partitions.push_back(1+static_cast<int>(random()%1024));
    for(const int replay:{0,1}) {
        trace->count=0;const auto run=root/std::to_string(replay);REQUIRE(std::filesystem::create_directory(run));
        NodeExecutionContext context;context.output_directory=run.string();context.identity={project.project_id,project.revision,juce::Uuid().toString().toStdString(),1};
        juce::Array<juce::var> artifacts;context.record_task_artifact=[&](std::string directory,std::string provenance){
            juce::DynamicObject::Ptr item=new juce::DynamicObject;item->setProperty("directory",juce::String::fromUTF8(directory.c_str()));item->setProperty("provenance",juce::JSON::parse(provenance));artifacts.add(juce::var(item.get()));
        };
        std::string tasks;NativeRenderStatistics statistics;juce::ScopedJuceInitialiser_GUI messages;
        auto future=std::async(std::launch::async,[&]{
            auto result=render_prepared_audio_graph(project,registry,{{"file",input}},1024,context,&statistics,&tasks,partitions);
            juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;
        });juce::MessageManager::getInstance()->runDispatchLoop();const auto result=future.get();
        const auto* error=std::get_if<Diagnostic>(&result);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<AudioBuffer>(result));
        REQUIRE(trace->count>0);CHECK(trace->frames[0]==127);
        std::size_t cursor=0,index=0;while(cursor<1025) {
            const auto expected=std::min<std::size_t>(partitions[index%partitions.size()],1025-cursor);
            REQUIRE(index<trace->count);CHECK(trace->frames[index]==expected);cursor+=expected;++index;
        }CHECK(trace->count==index);
        const auto& output=std::get<AudioBuffer>(result);REQUIRE(output.frame_count()==1027);CHECK(output.valid_interval()==(AudioInterval{0,1027}));
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1027;++i) {
            const auto x=[&](std::size_t n){return n<1025?input.channel(c)[n]:0.f;};
            const float value=x(i)+(i>=1?x(i-1)*.5f:0.f)+(i>=2?x(i-2)*.25f:0.f);
            const float expected=(value*.75f+static_cast<float>(i)*.00001f)*.5f;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(run/"output.wav",output,WavEncoding::float32)));
        juce::DynamicObject::Ptr report=new juce::DynamicObject;report->setProperty("tasks",juce::JSON::parse(tasks));report->setProperty("task_artifacts",artifacts);
        juce::Array<juce::var> actual,pattern;for(std::size_t i=0;i<trace->count;++i)actual.add(static_cast<int>(trace->frames[i]));for(const auto value:partitions)pattern.add(value);
        report->setProperty("actual_probe_frames",actual);report->setProperty("host_partitions",pattern);
        REQUIRE(juce::File((run/"boundary-report.json").string()).replaceWithText(juce::JSON::toString(juce::var(report.get()))));
    }
}

TEST_CASE("P3 timeline invalid mixed partitions reject before native or offline factories","[P3-timeline][mixed-random-admission]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    for(const int invalid:{0,-1,1025}) {
        NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
        int created=0;auto d=*registry.find("builtin.gain");d.type_id="fixture.uncreated_partition_probe";d.parameters.clear();
        REQUIRE(registry.register_node(d,[&]{++created;return std::make_unique<CountingNative>(std::make_shared<Count>());}).registered);
        auto offline=*registry.find("example.python_stateful");offline.type_id="fixture.uncreated_offline_partition";
        REQUIRE(registry.register_node(offline,[&]{++created;return std::make_unique<UncalledIntervalJoin>(std::make_shared<int>(0));}).registered);
        auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();
        project.nodes[2].type_id=offline.type_id;
        const auto root=source/"build/runs"/("mixed-random-invalid-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
        const auto input=std::get<AudioBuffer>(read_wav_file(example/"input.wav"));NodeExecutionContext context;context.output_directory=root.string();
        const std::array<int,2> partitions{127,invalid};
        auto future=std::async(std::launch::async,[&]{return render_prepared_audio_graph(project,registry,{{"file",input}},1024,context,nullptr,nullptr,partitions);});
        const auto result=future.get();REQUIRE(std::holds_alternative<Diagnostic>(result));CHECK(std::get<Diagnostic>(result).code=="invalid_host_partition");
        CHECK(created==0);CHECK(std::filesystem::is_empty(root));
    }
}

TEST_CASE("P3 timeline independent stream EOS survives Python while short output stays short","[P3-timeline][mixed-stream-eos]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    const auto root=source/"build/runs"/("mixed-stream-eos-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    for(const std::size_t frame:{480,512})for(const std::string selected:{"main","side"})for(const int host:{1,127,256,480,512,1024}) {
        const auto frames=frame-4;NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());
        test::TailStreamProbe probe;const auto d=test::independent_tail_stream_descriptor(frame);
        REQUIRE(registry.register_node(d,[&]{return std::make_unique<test::IndependentTailStream>(frame,probe);}).registered);
        auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();project.connections[1].from_port=selected;
        const auto base=root/(std::to_string(frame)+"-"+selected+"-"+std::to_string(host));REQUIRE(std::filesystem::create_directory(base));
        AudioBuffer input(48000,{"left","right"},frames);for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<frames;++i)
            input.mutable_channel(c)[i]=static_cast<float>(.125*std::sin((i+1)*(c+1)*.03));
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(base/"input.wav",input,WavEncoding::float32)));
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(base/"mixed.aaproj",project)));
        RunService service(registry);juce::ScopedJuceInitialiser_GUI messages;const auto result=worker_run(service,{project,base/"mixed.aaproj",base/"run",host});
        const auto* error=std::get_if<Diagnostic>(&result);INFO(frame);INFO(selected);INFO(host);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));const auto tail=selected=="main"?2u:5u;
        REQUIRE(output.frame_count()==frames+tail);CHECK(probe.finished==(std::array<std::size_t,2>{1,1}));CHECK(probe.prepares==1);
        CHECK(probe.output_ends[0]==frames+2);CHECK(probe.output_ends[1]==frames+5);CHECK(probe.produced==(std::array<std::size_t,2>{frames+2,frames+5}));
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<output.frame_count();++i) {
            const auto x=[&](std::size_t n){return n<frames?input.channel(c)[n]:0.f;};
            const float fir=x(i)*(selected=="main"?1.f:2.f)+(i>=tail?x(i-tail)*(selected=="main"?.25f:.125f):0.f);
            const float expected=(fir*.75f+static_cast<float>(i)*.00001f)*.5f;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        const auto report=juce::JSON::parse(juce::File((base/"run/run.json").string()));const auto alignment=report["tasks"][1]["alignment"];
        CHECK(static_cast<int>(report["valid_frame_count"])==static_cast<int>(frames+tail));
        CHECK(static_cast<int>(report["drain_padding_frames"])==(selected=="main"?3:0));
        CHECK(static_cast<int>(alignment["drain_padding_frames"])==(selected=="main"?3:0));
        CHECK(static_cast<int>(alignment["raw_frames"])==static_cast<int>(frames+tail+frame-1));
        juce::DynamicObject::Ptr observation=new juce::DynamicObject;juce::Array<juce::var> ends,produced,finished;
        for(std::size_t p=0;p<2;++p){ends.add(static_cast<int>(*probe.output_ends[p]));produced.add(static_cast<int>(probe.produced[p]));finished.add(static_cast<int>(probe.finished[p]));}
        observation->setProperty("output_ends",ends);observation->setProperty("produced",produced);observation->setProperty("finished",finished);
        observation->setProperty("prepares",static_cast<int>(probe.prepares));observation->setProperty("algorithm_input_frames",static_cast<int>(probe.frames));
        REQUIRE(juce::File((base/"probe.json").string()).replaceWithText(juce::JSON::toString(juce::var(observation.get()))));
    }
}

TEST_CASE("P3 timeline missing long output EOS cannot publish short success and clean retry recovers","[P3-timeline][mixed-stream-eos-recovery]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());test::TailStreamProbe probe;bool omit=false;
    const auto d=test::independent_tail_stream_descriptor(480);REQUIRE(registry.register_node(d,[&]{return std::make_unique<test::IndependentTailStream>(480,probe,omit);}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=d.type_id;project.nodes[1].parameters.clear();project.connections[1].from_port="main";
    const auto root=source/"build/runs"/("mixed-stream-recovery-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    AudioBuffer input(48000,{"mono"},476);for(std::size_t i=0;i<476;++i)input.mutable_channel(0)[i]=static_cast<float>(.1*std::sin((i+1)*.03));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));RunService service(registry);
    const auto execute=[&](const std::string& name){juce::ScopedJuceInitialiser_GUI messages;return worker_run(service,{project,root/"mixed.aaproj",root/name,127});};
    const auto first=execute("first");REQUIRE(std::holds_alternative<RunSuccess>(first));
    const auto bytes=[&](const std::filesystem::path& path){juce::MemoryBlock data;REQUIRE(juce::File(path.string()).loadFileAsData(data));return data;};
    const auto original=bytes(root/"first/output.wav");
    omit=true;const auto failed=execute("failed");REQUIRE(std::holds_alternative<Diagnostic>(failed));
    CHECK(std::get<Diagnostic>(failed).code=="stream_no_progress");CHECK(std::get<Diagnostic>(failed).node_id=="before");
    CHECK(std::get<Diagnostic>(failed).port_id=="side");
    CHECK(probe.finished==(std::array<std::size_t,2>{1,0}));CHECK(probe.calls<200);CHECK_FALSE(std::filesystem::exists(root/"failed"));
    const auto failed_finished=probe.finished;const auto failed_calls=probe.calls;
    omit=false;const auto retry=execute("retry");REQUIRE(std::holds_alternative<RunSuccess>(retry));CHECK(probe.finished==(std::array<std::size_t,2>{1,1}));
    CHECK(bytes(root/"first/output.wav")==original);
    CHECK(bytes(root/"retry/output.wav")==original);
    juce::DynamicObject::Ptr observation=new juce::DynamicObject;
    observation->setProperty("code",std::get<Diagnostic>(failed).code.c_str());observation->setProperty("node_id",std::get<Diagnostic>(failed).node_id.c_str());
    observation->setProperty("port_id",std::get<Diagnostic>(failed).port_id.c_str());
    juce::Array<juce::var> observed_finished;for(const auto n:failed_finished)observed_finished.add(static_cast<int>(n));
    observation->setProperty("failed_finished",observed_finished);observation->setProperty("failed_calls",static_cast<int>(failed_calls));
    observation->setProperty("failed_directory_exists",std::filesystem::exists(root/"failed"));
    REQUIRE(juce::File((root/"recovery-observation.json").string()).replaceWithText(juce::JSON::toString(juce::var(observation.get()))));
}
