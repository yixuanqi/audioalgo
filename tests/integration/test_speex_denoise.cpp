#include "plugins/host/registration.h"
#include "audio_juce_backend/JuceGraph.h"
#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_algorithms/SpeexDenoise.h"
#include "audio_editor_application/EditorController.h"
#include "EditorNodeInspector.h"
#include <speex/speex_preprocess.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <limits>

namespace {
// Independent original-library oracle. No AudioAlgo algorithm/adapter helpers.
audioalgo::AudioBuffer original_speex(const audioalgo::AudioBuffer& input,int suppression) {
    constexpr std::size_t frame=480;
    audioalgo::AudioBuffer output(input.sample_rate_hz(),input.channel_ids(),input.frame_count());
    for(std::size_t channel=0;channel<input.channel_count();++channel) {
        auto* state=speex_preprocess_state_init(frame,input.sample_rate_hz());
        REQUIRE(state!=nullptr);
        std::unique_ptr<SpeexPreprocessState,decltype(&speex_preprocess_state_destroy)> owned(state,speex_preprocess_state_destroy);
        int enabled=1,disabled=0;
        REQUIRE(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_DENOISE,&enabled)==0);
        REQUIRE(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&suppression)==0);
        REQUIRE(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_AGC,&disabled)==0);
        int vad=-1;REQUIRE(speex_preprocess_ctl(state,SPEEX_PREPROCESS_GET_VAD,&vad)==0);REQUIRE(vad==0);
        std::vector<spx_int16_t> pcm(frame);
        // The original preprocess analysis has one frame overlap delay. Include
        // the next zero-padded frame, then compare the aligned valid interval.
        for(std::size_t offset=0;offset<input.frame_count()+frame;offset+=frame) {
            for(std::size_t i=0;i<frame;++i) {
                const auto sample=offset+i<input.frame_count()?input.channel(channel)[offset+i]:0.0f;
                const auto integer=std::lround(static_cast<double>(sample)*32768.0);
                pcm[i]=static_cast<spx_int16_t>(std::clamp<long>(integer,-32768,32767));
            }
            speex_preprocess_run(state,pcm.data());
            for(std::size_t i=0;i<frame;++i) if(offset+i>=frame && offset+i-frame<input.frame_count())
                output.mutable_channel(channel)[offset+i-frame]=static_cast<float>(pcm[i])/32768.0f;
        }
    }
    return output;
}

TEST_CASE("ALG-04 Speex core instances reset and reject invalid blocks without corrupting output","[ALG-04][AR-19.a]") {
    audioalgo::SpeexDenoiseProcessor a,b;
    std::vector<float> input(480),out_a(480,0.25f),out_b(480);
    audioalgo::AudioChannelView in{16000,0,input,{0,480}};
    audioalgo::MutableAudioChannelView oa{16000,0,out_a,{0,480}},ob{16000,0,out_b,{0,480}};
    REQUIRE(a.process(in,oa));CHECK(a.process(in,oa)->code=="denoise_not_prepared");
    REQUIRE_FALSE(a.prepare({-20},{16000,1}));REQUIRE_FALSE(b.prepare({-60},{16000,1}));
    input[31]=std::numeric_limits<float>::quiet_NaN();
    REQUIRE(a.process(in,oa));CHECK(a.process(in,oa)->code=="denoise_input_non_finite");
    CHECK(std::all_of(out_a.begin(),out_a.end(),[](float v){return v==0.25f;}));
    input[31]=0.0f;
    auto short_view=in;short_view.samples=std::span<const float>(input.data(),479);
    REQUIRE(a.process(short_view,oa));CHECK(a.process(short_view,oa)->code=="denoise_block_shape");
    audioalgo::AudioBuffer signal(16000,{"mono"},480*12);
    std::mt19937 random(9876);
    for(std::size_t i=0;i<signal.frame_count();++i) signal.mutable_channel(0)[i]=static_cast<float>((static_cast<int>(random()%10000)-5000)/10000.0);
    const auto oracle_a=original_speex(signal,-20),oracle_b=original_speex(signal,-60);
    for(int repeat=0;repeat<2;++repeat) {
        for(std::size_t offset=0;offset<signal.frame_count()+480;offset+=480) {
            for(std::size_t i=0;i<480;++i) input[i]=offset+i<signal.frame_count()?signal.channel(0)[offset+i]:0;
            REQUIRE_FALSE(a.process(in,oa));REQUIRE_FALSE(b.process(in,ob));
            if(offset>=480)for(std::size_t i=0;i<480 && offset+i-480<signal.frame_count();++i) {
                REQUIRE(out_a[i]==oracle_a.channel(0)[offset+i-480]);
                REQUIRE(out_b[i]==oracle_b.channel(0)[offset+i-480]);
            }
        }
        a.reset();b.reset();REQUIRE(a.process(in,oa));
        REQUIRE_FALSE(a.prepare({-20},{16000,1}));REQUIRE_FALSE(b.prepare({-60},{16000,1}));
    }
    REQUIRE(a.prepare({-61},{16000,1}));REQUIRE(a.process(in,oa));
    REQUIRE(a.prepare({-20},{44100,1}));REQUIRE(a.prepare({-20},{16000,3}));
    REQUIRE(a.prepare({-20},{16000,1},479));
}

namespace {
template<typename T> T* control(juce::Component& c,const juce::String& id) {
    if(c.getComponentID()==id)return dynamic_cast<T*>(&c);
    for(auto* child:c.getChildren())if(auto* found=control<T>(*child,id))return found;
    return nullptr;
}
}
TEST_CASE("ALG-01 generated Speex parameter control drives the original algorithm","[ALG-01][AR-19.a]") {
    juce::ScopedJuceInitialiser_GUI messages;
    audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_host_nodes(nodes,{}).diagnostics.empty());
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("speex-control-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));
    audioalgo::AudioBuffer input(16000,{"mono"},16037);
    std::mt19937 random(319);
    for(std::size_t i=0;i<input.frame_count();++i) input.mutable_channel(0)[i]=static_cast<float>((static_cast<int>(random()%10000)-5000)/25000.0);
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel p;p.project_id="speex-controls";p.selected_output_node_id="output";
    p.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},{"output","builtin.output",{}}};
    audioalgo::editor::EditorController controller(nodes);controller.adopt_project(p);
    using Edit=audioalgo::editor::ProjectEditStatus;
    REQUIRE(controller.add_node("speex.denoise","denoise",200,20).status==Edit::applied);
    REQUIRE(controller.connect({"file","audio","denoise","audio_in"}).status==Edit::applied);
    REQUIRE(controller.connect({"denoise","audio_out","output","audio"}).status==Edit::applied);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller,selection,[]{});
    inspector.set_descriptor_registry(nodes);inspector.show_node("denoise");
    auto* slider=control<juce::Slider>(inspector,"parameter.noise_suppress_db");
    REQUIRE(slider!=nullptr);slider->onDragStart();slider->setValue(0,juce::sendNotificationSync);slider->onDragEnd();
    REQUIRE(std::get<std::int64_t>(controller.node_snapshot("denoise")->node.parameters.at("noise_suppress_db"))==0);
    const auto oracle=original_speex(input,0),default_oracle=original_speex(input,-20);
    double changed=0;
    for(std::size_t i=0;i<input.frame_count();++i)changed=std::max(changed,std::abs(static_cast<double>(oracle.channel(0)[i]-default_oracle.channel(0)[i])));
    REQUIRE(changed>1e-5);
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"controls.aaproj",controller.project())));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"oracle.wav",oracle,audioalgo::WavEncoding::float32)));
    audioalgo::RunService service(nodes);
    auto result=service.run({controller.project(),root/"controls.aaproj",root/"run",127});
    if(auto* e=std::get_if<audioalgo::Diagnostic>(&result)) INFO(e->code+": "+e->message);
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(result));
    auto audio=audioalgo::read_wav_file(root/"run/output.wav");REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(audio));
    for(std::size_t i=0;i<input.frame_count();++i) REQUIRE(std::get<audioalgo::AudioBuffer>(audio).channel(0)[i]==oracle.channel(0)[i]);
    std::cout<<"speex_control_artifact="<<root.string()<<'\n';
}
}

TEST_CASE("ALG-01 real Speex denoise matches original library through graph run service and CLI","[ALG-01][ALG-04][AR-19.a]") {
    juce::ScopedJuceInitialiser_GUI messages;
    audioalgo::NodeRegistry nodes;
    REQUIRE(audioalgo::register_host_nodes(nodes,{}).diagnostics.empty());
    REQUIRE(nodes.find("speex.denoise")!=nullptr);
    for(const auto rate:{16000,48000})for(const auto channels:{1,2,4})for(const auto scene:{0,1,2}) {
        INFO("rate="<<rate<<" channels="<<channels<<" scene="<<scene);
        std::vector<std::string> ids;
        for(int c=0;c<channels;++c) ids.push_back("channel-"+std::to_string(c));
        audioalgo::AudioBuffer input(rate,ids,2033);
        std::mt19937 random(12345);
        for(int c=0;c<channels;++c)for(std::size_t i=0;i<input.frame_count();++i) {
            if(scene==1) input.mutable_channel(c)[i]=i==17?0.25f:0.0f;
            else if(scene==2) input.mutable_channel(c)[i]=static_cast<float>(0.13*std::sin((i+1)*(c+1)*0.091)+0.04*(static_cast<double>(random()%10000)/5000.0-1.0));
        }
        const auto oracle=original_speex(input,-20);
        audioalgo::ProjectModel p;p.project_id="speex-real";p.name="Speex Denoise";p.selected_output_node_id="output";
        p.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},
                 {"denoise","speex.denoise",{{"noise_suppress_db",std::int64_t{-20}}}}, {"output","builtin.output",{}}};
        p.connections={{"file","audio","denoise","audio_in"},{"denoise","audio_out","output","audio"}};
        for(const auto host:{1,127,512}) {
            audioalgo::NativeRenderStatistics stats;
            auto result=audioalgo::render_audio_graph(p,nodes,input,host,{{},&stats});
            if(auto* e=std::get_if<audioalgo::Diagnostic>(&result)) INFO(e->code+": "+e->message);
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
            const auto& actual=std::get<audioalgo::AudioBuffer>(result);
            CHECK(stats.alignment_offset_frames==959);
            for(int c=0;c<channels;++c)for(std::size_t i=0;i<input.frame_count();++i)
                REQUIRE(std::abs(actual.channel(c)[i]-oracle.channel(c)[i])<1e-6f);
        }
        if(scene==2) {
            const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("speex-"+juce::Uuid().toString().toStdString());
            REQUIRE(std::filesystem::create_directories(root));
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"oracle.wav",oracle,audioalgo::WavEncoding::float32)));
            REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"denoise.aaproj",p)));
            audioalgo::RunService service(nodes);
            auto run=service.run({p,root/"denoise.aaproj",root/"component",127});
            if(auto* e=std::get_if<audioalgo::Diagnostic>(&run)) INFO(e->code+": "+e->message);
            REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(run));
            juce::ChildProcess cli;
            const auto cli_file=std::filesystem::u8path(AUDIOALGO_CLI_EXECUTABLE);
            REQUIRE(cli.start(juce::StringArray{cli_file.string(),"run",(root/"denoise.aaproj").string(),(root/"cli").string(),"127"}));
            REQUIRE(cli.waitForProcessToFinish(5000));
            INFO(cli.readAllProcessOutput());REQUIRE(cli.getExitCode()==0);
            for(auto folder:{"component","cli"}) {
                auto loaded=audioalgo::read_wav_file(root/folder/"output.wav");
                REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(loaded));
                const auto& actual=std::get<audioalgo::AudioBuffer>(loaded);
                REQUIRE(actual.frame_count()==2033);
                for(int c=0;c<channels;++c)for(std::size_t i=0;i<input.frame_count();++i)
                    REQUIRE(std::abs(actual.channel(c)[i]-oracle.channel(c)[i])<1e-6f);
            }
            std::cout<<"speex_artifact="<<root.string()<<'\n';
        }
    }
}
