#include "audio_juce_backend/NativeNodeAudioProcessor.h"
#include "audio_juce_backend/JuceGraph.h"
#include "plugins/builtin/registration.h"
#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <filesystem>
#include <iostream>

namespace {
struct Probe {std::size_t prepares=0,resets=0,calls=0,frames=0;};
class FixedIdentity final:public audioalgo::AudioNodeProcessor {
public:
    FixedIdentity(std::size_t frame,Probe& probe):frame_(frame),probe_(probe){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_unsupported","Fixed identity requires borrowed blocks"};
    }
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {
        ++probe_.prepares;
        if(c.maximum_block_frames!=frame_) return audioalgo::Diagnostic{"fixed_prepare_size","Must prepare with algorithm frame length"};
        return {};
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(b.frame_count!=frame_) return audioalgo::Diagnostic{"fixed_frame_size","Algorithm received a partial frame"};
        ++probe_.calls;probe_.frames+=b.frame_count;
        for(std::size_t c=0;c<b.channel_count;++c)
            std::copy_n(b.input_channels[c],frame_,b.output_channels[c]);
        return audioalgo::AudioBlockResult{frame_};
    }
    void reset() noexcept override{++probe_.resets;}
private:
    std::size_t frame_;
    Probe& probe_;
};
audioalgo::NodeDescriptor fixed_descriptor(std::size_t frame) {
    audioalgo::NodeDescriptor d{"test.fixed_"+std::to_string(frame),"1.0.0","Fixed identity",
        {{"in","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"out","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}};
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=frame;
    d.capabilities.fixed_frame_end_policy=audioalgo::NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    return d;
}

namespace {
class KnownDelay final:public audioalgo::AudioNodeProcessor {
public:
    explicit KnownDelay(std::size_t delay):delay_(delay){}
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_unsupported","Use borrowed blocks"};
    }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {
        state_.assign(c.format.channel_count*delay_,0);position_=0;return {};
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        for(std::size_t i=0;i<b.frame_count;++i) {
            for(std::size_t c=0;c<b.channel_count;++c) {
                const auto index=c*delay_+position_;
                b.output_channels[c][i]=state_[index];state_[index]=b.input_channels[c][i];
            }
            position_=(position_+1)%delay_;
        }
        return audioalgo::AudioBlockResult{b.frame_count};
    }
    void reset() noexcept override {std::fill(state_.begin(),state_.end(),0);position_=0;}
private:
    std::size_t delay_,position_=0;std::vector<float> state_;
};
}

TEST_CASE("AR-15.d known algorithm delay aligns a real JUCE branch and unknown delay is refused","[AR-15.d][GRAPH-06]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const bool unknown:{false,true})for(const auto host:{1,127,512}) {
        INFO("unknown="<<unknown<<" host="<<host);
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto d=fixed_descriptor(480);
        d.type_id="test.delay37";d.capabilities.supports_variable_block_size=true;
        d.capabilities.fixed_block_frames=0;
        d.capabilities.fixed_frame_end_policy=audioalgo::NodeExecutionCapabilities::FixedFrameEndPolicy::reject_partial;
        d.capabilities.latency_frames=unknown?std::optional<std::size_t>{}:std::optional<std::size_t>{37};
        int creations=0;
        REQUIRE(nodes.register_node(d,[&]{++creations;return std::make_unique<KnownDelay>(37);}).registered);
        audioalgo::ProjectModel p;p.project_id="delay-branch";p.selected_output_node_id="output";
        p.nodes={{"file","builtin.file_source",{{"path",std::string("unused.wav")}}},{"delay",d.type_id,{}},
                 {"mix","builtin.mixer",{}},{"output","builtin.output",{}}};
        p.connections={{"file","audio","delay","in"},{"delay","out","mix","primary"},
                       {"file","audio","mix","secondary"},{"mix","sum","output","audio"}};
        audioalgo::AudioBuffer input(48000,{"left","right"},513);
        input.mutable_channel(0)[0]=0.125f;input.mutable_channel(0)[512]=-0.25f;
        input.mutable_channel(1)[79]=0.375f;
        audioalgo::NativeRenderStatistics stats;
        auto result=audioalgo::render_audio_graph(p,nodes,input,host,{{},&stats,true});
        if(unknown) {
            REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
            CHECK(std::get<audioalgo::Diagnostic>(result).code=="latency_unknown");
            CHECK(std::get<audioalgo::Diagnostic>(result).node_id=="delay");
            CHECK(creations==0);
        } else {
            if(auto* e=std::get_if<audioalgo::Diagnostic>(&result)) INFO(e->code+": "+e->message);
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
            auto& output=std::get<audioalgo::AudioBuffer>(result);
            CHECK(output.frame_count()==513);CHECK(stats.alignment_offset_frames==37);CHECK(stats.raw_frames==550);
            REQUIRE(stats.raw_audio.has_value());CHECK(stats.raw_audio->frame_count()==550);
            CHECK(stats.raw_audio->valid_interval().first_frame==37);
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<37;++i) CHECK(stats.raw_audio->channel(c)[i]==0);
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<513;++i) REQUIRE(output.channel(c)[i]==2*input.channel(c)[i]);
            if(host==127) {
                const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("delay-"+juce::Uuid().toString().toStdString());
                REQUIRE(std::filesystem::create_directories(root));
                REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
                p.nodes.front().parameters["path"]=std::string("input.wav");
                REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"delay.aaproj",p)));
                audioalgo::RunService service(nodes);
                auto run=service.run({p,root/"delay.aaproj",root/"run",127});
                if(auto* e=std::get_if<audioalgo::Diagnostic>(&run)) INFO(e->code+": "+e->message);
                REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(run));
                const auto raw=audioalgo::read_wav_file(root/"run/output.raw.wav");
                const auto aligned=audioalgo::read_wav_file(root/"run/output.wav");
                REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(raw));
                REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(aligned));
                CHECK(std::get<audioalgo::AudioBuffer>(raw).frame_count()==550);
                CHECK(std::get<audioalgo::AudioBuffer>(aligned).frame_count()==513);
                for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<513;++i) {
                    CHECK(std::get<audioalgo::AudioBuffer>(aligned).channel(c)[i]==2*input.channel(c)[i]);
                    CHECK(std::get<audioalgo::AudioBuffer>(raw).channel(c)[i+37]==2*input.channel(c)[i]);
                }
                const auto manifest=juce::JSON::parse(juce::File((root/"run/run.json").string()).loadFileAsString());
                CHECK(static_cast<int>(manifest["raw_frames"])==550);
                CHECK(static_cast<int>(manifest["aligned_frames"])==513);
                CHECK(static_cast<int>(manifest["alignment_offset_frames"])==37);
                CHECK(static_cast<int>(manifest["raw_valid_start_frame"])==37);
                CHECK(manifest["raw_output_file"].toString()=="output.raw.wav");
                REQUIRE(std::filesystem::is_regular_file(root/"run/complete.json"));
                std::cout<<"known_delay_artifact="<<root.string()<<'\n';
            }
        }
    }
}
}

TEST_CASE("AR-15.b fixed 480 and 512 frames preserve samples in the real JUCE graph","[AR-15.b][GRAPH-05]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto host:{1,127,256,480,512,1024,0}) for(const auto length:{1,479,480,481,511,512,513,16037}) {
        INFO("host="<<host<<" frames="<<length);
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        Probe p480,p512;
        auto d480=fixed_descriptor(480),d512=fixed_descriptor(512);
        REQUIRE(nodes.register_node(d480,[&]{return std::make_unique<FixedIdentity>(480,p480);}).registered);
        REQUIRE(nodes.register_node(d512,[&]{return std::make_unique<FixedIdentity>(512,p512);}).registered);
        audioalgo::ProjectModel project;
        project.project_id="fixed-frame-series";project.selected_output_node_id="output";
        project.nodes={{"file","builtin.file_source",{{"path",std::string("unused.wav")}}},
            {"a",d480.type_id,{}},{"b",d512.type_id,{}},{"output","builtin.output",{}}};
        project.connections={{"file","audio","a","in"},{"a","out","b","in"},{"b","out","output","audio"}};
        audioalgo::AudioBuffer input(48000,{"left","right"},length);
        for(std::size_t c=0;c<2;++c)for(int i=0;i<length;++i)
            input.mutable_channel(c)[i]=static_cast<float>(0.2*std::sin((i+1)*(c+1)*0.031));
        std::mt19937 random(20261022);
        std::vector<int> partitions;
        if(host==0) for(int i=0;i<173;++i) partitions.push_back(1+static_cast<int>(random()%1024));
        audioalgo::NativeRenderStatistics statistics;
        const auto result=audioalgo::render_audio_graph(project,nodes,input,host==0?1024:host,{partitions,&statistics});
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&result)) INFO(e->code+": "+e->message);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
        const auto& output=std::get<audioalgo::AudioBuffer>(result);
        REQUIRE(output.frame_count()==static_cast<std::size_t>(length));
        REQUIRE(output.valid_interval()==input.valid_interval());
        CHECK(statistics.alignment_offset_frames==479+511);
        CHECK(statistics.aligned_frames==static_cast<std::size_t>(length));
        CHECK(statistics.raw_frames==static_cast<std::size_t>(length)+479+511);
        for(std::size_t c=0;c<2;++c)for(int i=0;i<length;++i)
            REQUIRE(std::abs(output.channel(c)[i]-input.channel(c)[i])<1e-7);
        CHECK(p480.prepares==1);CHECK(p512.prepares==1);
        CHECK(p480.calls>0);CHECK(p512.calls>0);
        CHECK(p480.frames==p480.calls*480);CHECK(p512.frames==p512.calls*512);
    }
}

TEST_CASE("AR-15.b fixed FIFO is bounded under seeded random host partitions","[AR-15.b][GRAPH-05]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto frame:{480,512}) {
        Probe probe;
        auto d=fixed_descriptor(frame);
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<FixedIdentity>(frame,probe),"fixed",d,{}, {},2);
        wrapper.prepareToPlay(48000,1024);
        REQUIRE_FALSE(wrapper.processing_diagnostic().has_value());
        REQUIRE(wrapper.getLatencySamples()==frame-1);
        std::mt19937 random(20261022);
        const std::size_t source_frames=16037,total=source_frames+frame-1;
        std::vector<std::vector<float>> raw(2,std::vector<float>(total));
        std::size_t position=0;
        while(position<total) {
            auto count=std::min<std::size_t>(1+random()%1024,total-position);
            juce::AudioBuffer<float> block(2,static_cast<int>(count));
            for(int c=0;c<2;++c)for(std::size_t i=0;i<count;++i)
                block.setSample(c,static_cast<int>(i),position+i<source_frames?static_cast<float>(0.1*std::sin((position+i+1)*(c+1)*0.07)):0);
            juce::MidiBuffer midi;
            wrapper.processBlock(block,midi);
            REQUIRE_FALSE(wrapper.processing_diagnostic().has_value());
            for(int c=0;c<2;++c) std::copy_n(block.getReadPointer(c),count,raw[c].data()+position);
            position+=count;
        }
        for(int c=0;c<2;++c) {
            for(int i=0;i<frame-1;++i) CHECK(raw[c][i]==0.0f);
            for(std::size_t i=0;i<source_frames;++i)
                REQUIRE(std::abs(raw[c][i+frame-1]-static_cast<float>(0.1*std::sin((i+1)*(c+1)*0.07)))<1e-7);
        }
        CHECK(probe.prepares==1);
        const auto stats=wrapper.fixed_frame_buffer_stats();
        CHECK(stats.input_capacity==static_cast<std::size_t>(frame));
        CHECK(stats.output_capacity==static_cast<std::size_t>(frame));
        CHECK(stats.peak_input<=stats.input_capacity);CHECK(stats.peak_input==stats.input_capacity);
        CHECK(stats.peak_output<=stats.output_capacity);CHECK(stats.peak_output==stats.output_capacity);
        wrapper.releaseResources();CHECK(probe.resets>=1);
        wrapper.prepareToPlay(48000,1024);
        REQUIRE_FALSE(wrapper.processing_diagnostic());
        juce::AudioBuffer<float> impulse(2,frame);
        impulse.clear();impulse.setSample(0,0,0.25f);impulse.setSample(1,0,-0.25f);
        juce::MidiBuffer midi;wrapper.processBlock(impulse,midi);
        REQUIRE_FALSE(wrapper.processing_diagnostic());
        for(int i=0;i<frame-1;++i) { CHECK(impulse.getSample(0,i)==0);CHECK(impulse.getSample(1,i)==0); }
        CHECK(impulse.getSample(0,frame-1)==0.25f);CHECK(impulse.getSample(1,frame-1)==-0.25f);
    }
}
