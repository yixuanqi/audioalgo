#pragma once
#include "audio_node_sdk/NodeSDK.h"
#include <algorithm>
#include <array>

namespace audioalgo::test {
struct TailStreamProbe {
    std::size_t prepares=0,calls=0,frames=0;
    std::optional<std::size_t> input_end;
    std::array<std::optional<std::size_t>,2> output_ends;
    std::array<std::size_t,2> produced{},finished{};
};
inline NodeDescriptor independent_tail_stream_descriptor(std::size_t frame) {
    NodeDescriptor d{"fixture.independent_tail_stream","1","Independent tail stream",
        {{"audio_in","audio.pcm@1",PortDirection::input,true},{"main","audio.pcm@1",PortDirection::output},{"side","audio.pcm@1",PortDirection::output}}, {}};
    auto& c=d.capabilities;c.supports_named_audio_ports=true;c.supports_variable_block_size=false;c.fixed_block_frames=frame;
    c.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;c.uses_streaming_frame_api=true;
    c.tail_frames=5;c.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
    c.maximum_zero_padding_frames=frame+4;c.output_tail_frames={{"main",2},{"side",5}};return d;
}
// One input frame is retained while the two outputs make independent partial
// progress. All buffers are allocated in prepare, never in the stream callback.
class IndependentTailStream final:public AudioNodeProcessor {
    std::size_t frame_,channels_=0,position_=0,extent_=0;bool computed_=false;
    std::array<std::size_t,2> cursor_{};std::array<bool,2> finished_{};
    std::vector<float> output_;std::vector<std::array<float,5>> history_;
    TailStreamProbe& probe_;
    bool omit_long_finish_;
public:
    IndependentTailStream(std::size_t frame,TailStreamProbe& probe,bool omit_long_finish=false):frame_(frame),probe_(probe),omit_long_finish_(omit_long_finish){}
    NodeProcessOutcome process(const NodeProcessRequest&) override{return Diagnostic{"owned_unsupported","Stream callback required"};}
    bool supports_stream_processing() const noexcept override{return true;}
    std::optional<Diagnostic> prepare(const NodePrepareContext& c) override {
        const auto prepares=probe_.prepares+1;probe_={};probe_.prepares=prepares;
        channels_=c.format.channel_count;position_=extent_=0;computed_=false;cursor_={};finished_={};
        output_.assign(2*channels_*frame_,0.f);history_.assign(channels_,{});return {};
    }
    AudioStreamOutcome process_stream(const AudioStreamBlockView& b,AudioStreamProgress progress) override {
        ++probe_.calls;
        if(!computed_) {
            extent_=b.inputs[0].frame_count;
            if(b.inputs[0].end_of_stream && !probe_.input_end)probe_.input_end=position_+b.inputs[0].frames_before_eos;
            for(std::size_t c=0;c<channels_;++c)for(std::size_t i=0;i<extent_;++i) {
                const auto x=b.inputs[0].channels[c][i];
                output_[c*frame_+i]=x+history_[c][1]*.25f;
                output_[(channels_+c)*frame_+i]=x*2.f+history_[c][4]*.125f;
                for(std::size_t k=4;k>0;--k)history_[c][k]=history_[c][k-1];history_[c][0]=x;
            }
            progress.consumed[0]=extent_;computed_=true;probe_.frames+=extent_;
        }
        for(std::size_t p=0;p<2;++p) {
            if(finished_[p])continue;
            if(b.outputs[p].end_of_stream && !probe_.output_ends[p])
                probe_.output_ends[p]=position_+cursor_[p]+b.outputs[p].frames_before_eos;
            auto count=std::min<std::size_t>(7,b.outputs[p].frame_capacity);
            if(b.outputs[p].end_of_stream)count=std::min(count,b.outputs[p].frames_before_eos);
            for(std::size_t c=0;c<channels_;++c)
                std::copy_n(output_.data()+(p*channels_+c)*frame_+cursor_[p],count,b.outputs[p].channels[c]);
            cursor_[p]+=count;progress.produced[p]=count;probe_.produced[p]+=count;
            if(b.outputs[p].end_of_stream && count==b.outputs[p].frames_before_eos && !(p==1 && omit_long_finish_)) {
                progress.finished[p]=1;finished_[p]=true;++probe_.finished[p];
            }
        }
        if((finished_[0] || cursor_[0]==extent_) && (finished_[1] || cursor_[1]==extent_)) {
            position_+=extent_;cursor_={};computed_=false;
        }
        return AudioStreamStatus::progress;
    }
};
}
