#include "audio_juce_backend/JuceGraph.h"
#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <iostream>

namespace {
constexpr std::array<float,4> taps{0.5f,0.25f,0.0f,0.0625f};
class FiniteFir final:public audioalgo::AudioNodeProcessor {
public:
    explicit FiniteFir(std::size_t* frames=nullptr):frames_(frames){}
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_unsupported","Use borrowed FIR"};
    }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {
        memory_.assign(c.format.channel_count*4,0);index_=0;if(frames_)*frames_=0;return {};
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(frames_)*frames_+=b.frame_count;
        for(std::size_t i=0;i<b.frame_count;++i) {
            for(std::size_t c=0;c<b.channel_count;++c) {
                memory_[c*4+index_]=b.input_channels[c][i];float sum=0;
                for(std::size_t j=0;j<4;++j)sum+=taps[j]*memory_[c*4+(index_+4-j)%4];
                b.output_channels[c][i]=sum;
            }
            index_=(index_+1)%4;
        }
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:std::vector<float> memory_;std::size_t index_=0;std::size_t* frames_;
};
audioalgo::NodeDescriptor tail_descriptor(std::size_t frame) {
    audioalgo::NodeDescriptor d{"test.fir"+std::to_string(frame),"1.0.0","Finite FIR",
        {{"in","audio.pcm@1",audioalgo::PortDirection::input,true},{"out","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}};
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=frame;
    d.capabilities.fixed_frame_end_policy=audioalgo::NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.tail_processing=audioalgo::NodeExecutionCapabilities::TailProcessing::zero_padding;
    d.capabilities.tail_frames=3;d.capabilities.maximum_zero_padding_frames=frame+2;
    return d;
}
audioalgo::ProjectModel project(const audioalgo::NodeDescriptor& first,const audioalgo::NodeDescriptor& second) {
    audioalgo::ProjectModel p;p.project_id="finite-tail";p.selected_output_node_id="out";
    p.nodes={{"file","builtin.file_source",{{"path",std::string("unused.wav")}}},
        {"first",first.type_id,{}},{"second",second.type_id,{}},{"mix","builtin.mixer",{}},{"out","builtin.output",{}}};
    p.connections={{"file","audio","first","in"},{"first","out","second","in"},
        {"second","out","mix","primary"},{"file","audio","mix","secondary"},{"mix","sum","out","audio"}};
    return p;
}
}

TEST_CASE("AR-15.e finite FIR tails survive fixed frames chain and compensated branch with bounded padding","[AR-15.e][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const int count:{1,479,480,481,513})for(const int host:{1,127,512}) {
        INFO("N="<<count<<" host="<<host);
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto d480=tail_descriptor(480),d512=tail_descriptor(512);
        std::size_t first_frames=0,second_frames=0;
        REQUIRE(nodes.register_node(d480,[&]{return std::make_unique<FiniteFir>(&first_frames);}).registered);
        REQUIRE(nodes.register_node(d512,[&]{return std::make_unique<FiniteFir>(&second_frames);}).registered);
        auto p=project(d480,d512);audioalgo::AudioBuffer input(48000,{"left","right"},count);
        for(int i=0;i<count;++i)for(std::size_t c=0;c<2;++c)
            input.mutable_channel(c)[i]=static_cast<float>(0.1*std::sin((i+1)*(c+1)*0.1));
        input.mutable_channel(0)[count-1]=0.25f;input.mutable_channel(1)[count-1]=-0.375f;
        audioalgo::NativeRenderStatistics stats;
        const auto rendered=audioalgo::render_audio_graph(p,nodes,input,host,{{},&stats,true});
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&rendered))INFO(e->code<<": "<<e->message);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
        const auto& output=std::get<audioalgo::AudioBuffer>(rendered);
        REQUIRE(output.frame_count()==static_cast<std::size_t>(count+6));
        CHECK(output.valid_interval().frame_count==output.frame_count());
        CHECK(stats.alignment_offset_frames==990);CHECK(stats.raw_frames==static_cast<std::size_t>(count+996));
        CHECK(first_frames>=static_cast<std::size_t>(count+3));
        CHECK(first_frames<=static_cast<std::size_t>(count)+*d480.capabilities.maximum_zero_padding_frames);
        CHECK(second_frames>=static_cast<std::size_t>(count+479+3+3));
        CHECK(second_frames<=static_cast<std::size_t>(count+479+3)+*d512.capabilities.maximum_zero_padding_frames);
        REQUIRE(stats.raw_audio);CHECK(stats.raw_audio->valid_interval().frame_count==output.frame_count());
        for(std::size_t c=0;c<2;++c)for(int i=0;i<count+6;++i) {
            double expected=i<count?input.channel(c)[i]:0;
            for(int j=0;j<4;++j)for(int k=0;k<4;++k)if(i-j-k>=0 && i-j-k<count)
                expected+=static_cast<double>(taps[j])*taps[k]*input.channel(c)[i-j-k];
            REQUIRE(std::abs(output.channel(c)[i]-expected)<1e-7);
            REQUIRE(stats.raw_audio->channel(c)[i+990]==output.channel(c)[i]);
        }
        if(count==481 && host==127) {
            const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("tail-"+juce::Uuid().toString().toStdString());
            REQUIRE(std::filesystem::create_directories(root));
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
            p.nodes.front().parameters["path"]=std::string("input.wav");
            REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"tail.aaproj",p)));
            const auto run=audioalgo::RunService(nodes).run({p,root/"tail.aaproj",root/"run",host});
            if(auto* e=std::get_if<audioalgo::Diagnostic>(&run))INFO(e->code<<": "<<e->message);
            REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(run));
            const auto manifest=juce::JSON::parse(juce::File((root/"run/run.json").string()).loadFileAsString());
            CHECK(static_cast<int>(manifest["source_frames"])==count);
            CHECK(static_cast<int>(manifest["retained_tail_frames"])==6);
            CHECK(static_cast<int>(manifest["zero_padding_frames"])==996);
            CHECK(static_cast<int>(manifest["raw_valid_frame_count"])==487);
            CHECK(static_cast<juce::int64>(manifest["raw_duration_ns"])==30770833);
            CHECK(static_cast<juce::int64>(manifest["aligned_duration_ns"])==10145833);
            CHECK(static_cast<juce::int64>(manifest["alignment_offset_ns"])==20625000);
            const auto wav=audioalgo::read_wav_file(root/"run/output.wav");
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(wav));
            CHECK(std::get<audioalgo::AudioBuffer>(wav).frame_count()==487);
            std::cout<<"finite_tail_artifact="<<root.string()<<'\n';
        }
    }
}

TEST_CASE("AR-15.e finite tail never invents valid samples from an empty or truncated valid interval","[AR-15.e][CORE-02]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const audioalgo::AudioInterval interval: {audioalgo::AudioInterval{0,0},{481,0},{2,479},{2,100}}) {
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto a=tail_descriptor(480),b=tail_descriptor(512);
        REQUIRE(nodes.register_node(a,[]{return std::make_unique<FiniteFir>();}).registered);
        REQUIRE(nodes.register_node(b,[]{return std::make_unique<FiniteFir>();}).registered);
        audioalgo::AudioBuffer input(48000,{"reference","microphone"},481,interval);
        audioalgo::NativeRenderStatistics stats;
        const auto result=audioalgo::render_audio_graph(project(a,b),nodes,input,127,{{},&stats,true});
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
        const auto& out=std::get<audioalgo::AudioBuffer>(result);
        const auto expected_count=interval.frame_count+(interval.frame_count!=0 && interval.first_frame+interval.frame_count==481?6:0);
        CHECK(out.valid_interval()==audioalgo::AudioInterval{interval.first_frame,expected_count});
        CHECK(out.channel_ids()==input.channel_ids());REQUIRE(stats.raw_audio);
        CHECK(stats.raw_audio->valid_interval()==audioalgo::AudioInterval{interval.first_frame+990,expected_count});
    }
}

namespace {
class Bias final:public audioalgo::AudioNodeProcessor {
public:
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {return audioalgo::Diagnostic{"owned_unsupported","Borrowed only"};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        for(std::size_t c=0;c<b.channel_count;++c)for(std::size_t i=0;i<b.frame_count;++i)
            b.output_channels[c][i]=b.input_channels[c][i]+0.01f;
        return audioalgo::AudioBlockResult{b.frame_count};
    }
};
}
TEST_CASE("AR-15.e completed shorter branch stays silent while another branch drains","[AR-15.e][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
    auto a=tail_descriptor(480),b=tail_descriptor(512);
    REQUIRE(nodes.register_node(a,[]{return std::make_unique<FiniteFir>();}).registered);
    REQUIRE(nodes.register_node(b,[]{return std::make_unique<FiniteFir>();}).registered);
    audioalgo::NodeDescriptor bias{"test.bias","1.0.0","Bias",a.ports,{}};
    REQUIRE(nodes.register_node(bias,[]{return std::make_unique<Bias>();}).registered);
    auto p=project(a,b);p.nodes.push_back({"bias",bias.type_id,{}});
    p.connections[3]={"bias","out","mix","secondary"};p.connections.push_back({"file","audio","bias","in"});
    audioalgo::AudioBuffer input(48000,{"mono"},481); // FIR branch is exactly zero.
    const auto result=audioalgo::render_audio_graph(p,nodes,input,127);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
    const auto& output=std::get<audioalgo::AudioBuffer>(result);REQUIRE(output.frame_count()==487);
    for(std::size_t i=0;i<487;++i)CHECK(output.channel(0)[i]==(i<481?0.01f:0.0f));
}

TEST_CASE("AR-15.e unknown tails flush-only algorithms and insufficient padding budgets fail before factory","[AR-15.e][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const std::string failure:{"unknown","flush","budget","missing_budget"}) {
        INFO(failure);audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto a=tail_descriptor(480),b=tail_descriptor(512);int factories=0;
        if(failure=="unknown")a.capabilities.tail_frames.reset();
        if(failure=="flush")a.capabilities.tail_processing=audioalgo::NodeExecutionCapabilities::TailProcessing::requires_flush;
        if(failure=="budget")a.capabilities.maximum_zero_padding_frames=481;
        if(failure=="missing_budget")a.capabilities.maximum_zero_padding_frames.reset();
        REQUIRE(nodes.register_node(a,[&]{++factories;return std::make_unique<FiniteFir>();}).registered);
        REQUIRE(nodes.register_node(b,[&]{++factories;return std::make_unique<FiniteFir>();}).registered);
        const auto result=audioalgo::render_audio_graph(project(a,b),nodes,audioalgo::AudioBuffer(48000,{"mono"},1),127);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
        const auto& e=std::get<audioalgo::Diagnostic>(result);CHECK(e.node_id=="first");
        CHECK(e.code==(failure=="unknown"?"tail_unknown":failure=="flush"?"flush_unsupported":"tail_padding_budget_invalid"));
        CHECK(factories==0);
    }
}
