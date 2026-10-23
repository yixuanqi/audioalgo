#include "audio_juce_backend/JuceGraph.h"
#include "audio_juce_backend/NativeNodeAudioProcessor.h"
#include "plugins/builtin/registration.h"
#include "tests/fixtures/AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <algorithm>
#include <cmath>

namespace {
enum class Failure {none,stall,input_count,output_count,early_finish,missing_finish,invalid_finish};
struct Probe {
    std::size_t calls=0,needs_input=0;std::array<std::size_t,2> inputs{},outputs{};
    std::array<std::optional<std::size_t>,2> eos_at{};std::array<std::size_t,2> finished{};
    bool require_eos=false;Failure failure=Failure::none;
};
class PartialFrame final:public audioalgo::AudioNodeProcessor {
public:
    PartialFrame(std::size_t frame,Probe& probe):frame_(frame),probe_(probe){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {return audioalgo::Diagnostic{"owned_unsupported","Borrowed streaming fixture"};}
    bool supports_stream_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {
        channels_=c.format.channel_count;data_.assign(2*channels_*frame_,0);input_={};output_={};sequence_=0;
        probe_.calls=probe_.needs_input=0;probe_.inputs={};probe_.outputs={};probe_.eos_at={};probe_.finished={};return {};
    }
    audioalgo::AudioStreamOutcome process_stream(const audioalgo::AudioStreamBlockView& b,audioalgo::AudioStreamProgress p) override {
        ++probe_.calls;
        if(probe_.require_eos && p.finished.size()!=2)return audioalgo::Diagnostic{"eos_progress_missing","Host did not supply output EOS progress"};
        if(probe_.failure==Failure::early_finish || probe_.failure==Failure::invalid_finish) {
            p.finished[1]=probe_.failure==Failure::early_finish?1:3;return audioalgo::AudioStreamStatus::progress;
        }
        if(std::all_of(b.inputs.begin(),b.inputs.end(),[](const auto& v){return v.end_of_stream && v.frame_count==0;}) &&
           std::all_of(b.outputs.begin(),b.outputs.end(),[](const auto& v){return v.end_of_stream && v.frame_capacity==0;})) {
            if(probe_.failure==Failure::missing_finish)return audioalgo::AudioStreamStatus::need_more_input;
            for(std::size_t port=0;port<2;++port) {
                probe_.eos_at[port]=0;p.finished[port]=1;++probe_.finished[port];
            }
            return audioalgo::AudioStreamStatus::progress;
        }
        if(probe_.failure==Failure::stall)return audioalgo::AudioStreamStatus::need_more_input;
        if(probe_.failure==Failure::input_count){p.consumed[1]=b.inputs[1].frame_count+1;return audioalgo::AudioStreamStatus::progress;}
        if(probe_.failure==Failure::output_count){p.produced[1]=b.outputs[1].frame_capacity+1;return audioalgo::AudioStreamStatus::progress;}
        for(std::size_t port=0;port<2;++port) {
            if(b.inputs[port].end_of_stream && !probe_.eos_at[port])
                probe_.eos_at[port]=sequence_+input_[port]+b.inputs[port].frames_before_eos;
            const auto count=std::min(b.inputs[port].frame_count,port==0?std::size_t{17}:std::size_t{23});
            for(std::size_t c=0;c<channels_;++c)
                std::copy_n(b.inputs[port].channels[c],count,data_.data()+(port*channels_+c)*frame_+input_[port]);
            input_[port]+=count;p.consumed[port]=count;probe_.inputs[port]+=count;
        }
        if(input_[0]!=frame_ || input_[1]!=frame_) {++probe_.needs_input;return audioalgo::AudioStreamStatus::need_more_input;}
        for(std::size_t port=0;port<2;++port) {
            const auto count=std::min(b.outputs[port].frame_capacity,port==0?std::size_t{11}:std::size_t{19});
            for(std::size_t c=0;c<channels_;++c)for(std::size_t i=0;i<count;++i) {
                const auto index=output_[port]+i;
                const auto a=data_[c*frame_+index],r=data_[(channels_+c)*frame_+index];
                b.outputs[port].channels[c][i]=port==0?static_cast<float>(2.0*a+3.0*r+0.00001*(sequence_+index)):a-r;
            }
            output_[port]+=count;p.produced[port]=count;probe_.outputs[port]+=count;
            if(probe_.failure!=Failure::missing_finish && count!=0 && !p.finished.empty() && b.outputs[port].end_of_stream && output_[port]==frame_) {
                p.finished[port]=1;++probe_.finished[port];
            }
        }
        if(output_[0]==frame_ && output_[1]==frame_) {input_={};output_={};sequence_+=frame_;}
        return audioalgo::AudioStreamStatus::progress;
    }
private:
    std::size_t frame_,channels_=0,sequence_=0;Probe& probe_;
    std::vector<float> data_;std::array<std::size_t,2> input_{},output_{};
};
audioalgo::NodeDescriptor descriptor(std::size_t frame) {
    audioalgo::NodeDescriptor d{"test.partial_frame","1.0.0","Partial frame",
        {{"primary","audio.pcm@1",audioalgo::PortDirection::input,true},{"reference","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"main","audio.pcm@1",audioalgo::PortDirection::output,false},{"side","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}};
    d.capabilities.supports_named_audio_ports=true;d.capabilities.supports_variable_block_size=false;
    d.capabilities.fixed_block_frames=frame;d.capabilities.fixed_frame_end_policy=audioalgo::NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.uses_streaming_frame_api=true;d.capabilities.maximum_no_progress_calls=3;return d;
}

namespace {
// A real causal three-sample delay with a retained tail; no separate flush operation.
class TailDelay final:public audioalgo::AudioNodeProcessor {
public:
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {return audioalgo::Diagnostic{"owned_unsupported","Borrowed only"};}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {state_.assign(c.format.channel_count*3,0);position_=0;return {};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        for(std::size_t i=0;i<b.frame_count;++i) {
            for(std::size_t c=0;c<b.channel_count;++c) {
                const auto index=c*3+position_;b.output_channels[c][i]=state_[index];state_[index]=b.input_channels[c][i];
            }
            position_=(position_+1)%3;
        }
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:std::vector<float> state_;std::size_t position_=0;
};
}
TEST_CASE("AR-15.c EOS follows each compensated input and independently finishes every output","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto count:{1,479,480,481,1025})for(const auto host:{1,127,512}) {
        INFO("N="<<count<<" host="<<host);
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto delay=descriptor(480);delay.type_id="test.tail_delay";delay.ports.resize(1);
        delay.ports.push_back({"out","audio.pcm@1",audioalgo::PortDirection::output,false});
        delay.capabilities.uses_streaming_frame_api=false;delay.capabilities.supports_named_audio_ports=false;
        delay.capabilities.tail_processing=audioalgo::NodeExecutionCapabilities::TailProcessing::zero_padding;
        delay.capabilities.tail_frames=3;delay.capabilities.maximum_zero_padding_frames=482;
        REQUIRE(nodes.register_node(delay,[]{return std::make_unique<TailDelay>();}).registered);
        Probe probe;probe.require_eos=true;auto d=descriptor(512);
        REQUIRE(nodes.register_node(d,[&]{return std::make_unique<PartialFrame>(512,probe);}).registered);
        audioalgo::ProjectModel p;p.project_id="independent-eos";p.selected_output_node_id="stream";p.selected_output_port_id="main";
        p.nodes={{"file","builtin.file_source",{{"path",std::string("unused.wav")}}},{"delay",delay.type_id,{}},{"stream",d.type_id,{}}};
        p.connections={{"file","audio","delay","primary"},{"delay","out","stream","primary"},{"file","audio","stream","reference"}};
        audioalgo::AudioBuffer input(48000,{"mono"},count);
        for(int i=0;i<count;++i)input.mutable_channel(0)[i]=static_cast<float>(0.1*std::sin((i+1)*0.2));
        const auto rendered=audioalgo::render_audio_graph(p,nodes,input,host);
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&rendered))INFO(e->code<<": "<<e->message);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
        const auto& output=std::get<audioalgo::AudioBuffer>(rendered);REQUIRE(output.frame_count()==static_cast<std::size_t>(count+3));
        for(int i=0;i<count+3;++i) {
            const double primary=i>=3 && i-3<count?input.channel(0)[i-3]:0;
            const double reference=i<count?input.channel(0)[i]:0;
            CHECK(std::abs(output.channel(0)[i]-(2*primary+3*reference+0.00001*(i+479)))<1e-6);
        }
        REQUIRE(probe.eos_at[0]);REQUIRE(probe.eos_at[1]);
        CHECK(*probe.eos_at[0]==static_cast<std::size_t>(count+482));
        CHECK(*probe.eos_at[1]==static_cast<std::size_t>(count+479));
        CHECK(probe.finished==std::array<std::size_t,2>{1,1});
    }
}
}

TEST_CASE("AR-15.c partial named port consumption and production retains every sample","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto frame:{480,512})for(const auto host:{1,127,512})for(const auto count:{1,513,2033})for(const bool swap:{false,true})for(const std::string selected:{"main","side"}) {
        INFO("F="<<frame<<" host="<<host<<" count="<<count<<" swap="<<swap<<" port="<<selected);
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));Probe probe;auto d=descriptor(frame);
        REQUIRE(nodes.register_node(d,[&]{return std::make_unique<PartialFrame>(frame,probe);}).registered);
        audioalgo::ProjectModel p;p.project_id="partial-ports";p.selected_output_node_id="stream";p.selected_output_port_id=selected;
        p.nodes={{"a","builtin.file_source",{{"path",std::string("unused-a.wav")}}},{"b","builtin.file_source",{{"path",std::string("unused-b.wav")}}},{"stream",d.type_id,{}}};
        p.connections={{swap?"b":"a","audio","stream","primary"},{swap?"a":"b","audio","stream","reference"}};
        audioalgo::AudioBuffer a(48000,{"left","right"},count),b(48000,{"left","right"},count);
        for(std::size_t c=0;c<2;++c)for(int i=0;i<count;++i) {
            a.mutable_channel(c)[i]=static_cast<float>(0.1*std::sin((i+1)*(c+1)*0.03));
            b.mutable_channel(c)[i]=static_cast<float>(0.07*std::cos((i+2)*(c+1)*0.05));
        }
        auto result=audioalgo::render_audio_graph(p,nodes,std::vector<audioalgo::AudioSourceBinding>{{"a",a},{"b",b}},host);
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&result))INFO(e->code+": "+e->message);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
        const auto& output=std::get<audioalgo::AudioBuffer>(result);
        for(std::size_t c=0;c<2;++c)for(int i=0;i<count;++i) {
            const auto primary=(swap?b:a).channel(c)[i],reference=(swap?a:b).channel(c)[i];
            const auto expected=selected=="main"?2.0*primary+3.0*reference+0.00001*i:primary-reference;
            REQUIRE(std::abs(output.channel(c)[i]-expected)<1e-6);
        }
        const auto padded=((static_cast<std::size_t>(count)+frame-1)/frame)*frame;
        CHECK(probe.inputs[0]==padded);CHECK(probe.inputs[1]==padded);CHECK(probe.outputs[0]==padded);CHECK(probe.outputs[1]==padded);
        CHECK(probe.needs_input>0);CHECK(probe.calls>padded/static_cast<std::size_t>(frame));
    }
}

TEST_CASE("AR-15.c invalid per port counts and no progress fail with bounded diagnostics then recover","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto failure:{Failure::stall,Failure::input_count,Failure::output_count}) {
        Probe probe;probe.failure=failure;const auto d=descriptor(480);
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<PartialFrame>(480,probe),"stream",d,{}, {},2);
        wrapper.prepareToPlay(48000,512);REQUIRE_FALSE(wrapper.processing_diagnostic());
        juce::AudioBuffer<float> block(4,480);for(int c=0;c<4;++c)std::fill_n(block.getWritePointer(c),480,0.125f);
        juce::MidiBuffer midi;wrapper.processBlock(block,midi);REQUIRE(wrapper.processing_diagnostic());
        const auto& error=*wrapper.processing_diagnostic();CHECK(error.node_id=="stream");
        if(failure==Failure::stall){CHECK(error.code=="stream_no_progress");CHECK(probe.calls==3);}
        if(failure==Failure::input_count){CHECK(error.code=="stream_input_count_invalid");CHECK(error.port_id=="reference");}
        if(failure==Failure::output_count){CHECK(error.code=="stream_output_count_invalid");CHECK(error.port_id=="side");}
        for(int c=0;c<4;++c)for(int i=0;i<480;++i)REQUIRE(block.getSample(c,i)==0);
        probe.failure=Failure::none;wrapper.prepareToPlay(48000,512);REQUIRE_FALSE(wrapper.processing_diagnostic());
        wrapper.processBlock(block,midi);CHECK_FALSE(wrapper.processing_diagnostic());
    }
}

TEST_CASE("AR-15.c partial stream callbacks allocate no memory after prepare","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto frame:{480,512})for(const bool finite:{false,true}) {
        Probe probe;const auto d=descriptor(frame);
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<PartialFrame>(frame,probe),"stream",d,{}, {},2);
        if(finite) {wrapper.set_finite_output_frames(1025+frame-1);wrapper.set_finite_input_ends({1025,1025});}
        wrapper.prepareToPlay(48000,512);REQUIRE_FALSE(wrapper.processing_diagnostic());
        juce::AudioBuffer<float> block(4,127);juce::MidiBuffer midi;block.clear();
        wrapper.processBlock(block,midi);
        audioalgo::test::begin_allocation_count();
        for(int i=0;i<100;++i)wrapper.processBlock(block,midi);
        const auto allocations=audioalgo::test::end_allocation_count();
        CHECK(allocations==0);CHECK_FALSE(wrapper.processing_diagnostic());CHECK(probe.calls>100);
        if(finite)CHECK(probe.finished==std::array<std::size_t,2>{1,1});
    }
}

TEST_CASE("AR-15.c empty streams acknowledge EOS without inventing a padded algorithm frame","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto frame:{480,512})for(const auto host:{1,127,512}) {
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));Probe probe;probe.require_eos=true;
        auto d=descriptor(frame);REQUIRE(nodes.register_node(d,[&]{return std::make_unique<PartialFrame>(frame,probe);}).registered);
        audioalgo::ProjectModel p;p.project_id="empty-eos";p.selected_output_node_id="stream";p.selected_output_port_id="main";
        p.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},{"b","builtin.file_source",{{"path",std::string("b.wav")}}},{"stream",d.type_id,{}}};
        p.connections={{"a","audio","stream","primary"},{"b","audio","stream","reference"}};
        const auto result=audioalgo::render_audio_graph(p,nodes,std::vector<audioalgo::AudioSourceBinding>{
            {"a",audioalgo::AudioBuffer(48000,{"mono"},0)},{"b",audioalgo::AudioBuffer(48000,{"mono"},0)}},host);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
        CHECK(std::get<audioalgo::AudioBuffer>(result).frame_count()==0);
        CHECK(probe.calls==1);CHECK(probe.inputs==std::array<std::size_t,2>{0,0});CHECK(probe.outputs==std::array<std::size_t,2>{0,0});
        CHECK(probe.finished==std::array<std::size_t,2>{1,1});
        CHECK(probe.eos_at[0]==0);CHECK(probe.eos_at[1]==0);
    }
}

TEST_CASE("AR-15.c premature invalid and missing output EOS cannot publish success","[AR-15.c][GRAPH-07]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for(const auto failure:{Failure::early_finish,Failure::missing_finish,Failure::invalid_finish}) {
        CAPTURE(static_cast<int>(failure));
        audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));Probe probe;probe.failure=failure;
        auto d=descriptor(480);REQUIRE(nodes.register_node(d,[&]{return std::make_unique<PartialFrame>(480,probe);}).registered);
        audioalgo::ProjectModel p;p.project_id="bad-eos";p.selected_output_node_id="stream";p.selected_output_port_id="main";
        p.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},{"b","builtin.file_source",{{"path",std::string("b.wav")}}},{"stream",d.type_id,{}}};
        p.connections={{"a","audio","stream","primary"},{"b","audio","stream","reference"}};
        const auto result=audioalgo::render_audio_graph(p,nodes,std::vector<audioalgo::AudioSourceBinding>{
            {"a",audioalgo::AudioBuffer(48000,{"mono"},1025)},{"b",audioalgo::AudioBuffer(48000,{"mono"},1025)}},127);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
        const auto& error=std::get<audioalgo::Diagnostic>(result);CHECK(error.node_id=="stream");
        if(failure==Failure::missing_finish)CHECK(error.code=="stream_no_progress");
        else {CHECK(error.code=="stream_eos_invalid");CHECK(error.port_id=="side");}
        CHECK(probe.calls<300);
    }
}
