#include "DenoiseNode.h"
namespace audioalgo {
AudioStreamOutcome DenoiseNode::process_stream(const AudioStreamBlockView& b,AudioStreamProgress progress) {
    if(b.inputs.size()!=1 || b.outputs.size()!=1 || progress.consumed.size()!=1 || progress.produced.size()!=1 || progress.finished.size()!=1 ||
       b.inputs[0].id!="audio_in" || b.outputs[0].id!="audio_out" || b.inputs[0].frame_count!=480 ||
       b.outputs[0].frame_capacity<480 || b.inputs[0].channels.size()!=b.outputs[0].channels.size())
        return Diagnostic{"denoise_stream_shape","Speex denoise requires one complete prepared frame on its named PCM ports"};
    const auto outcome=process_block({b.sample_rate_hz,480,b.inputs[0].channels.size(),b.inputs[0].channels.data(),b.outputs[0].channels.data()});
    if(const auto* error=std::get_if<Diagnostic>(&outcome))return *error;
    progress.consumed[0]=progress.produced[0]=480;
    if(b.outputs[0].end_of_stream)progress.finished[0]=1;
    return AudioStreamStatus::progress;
}
std::optional<Diagnostic> DenoiseNode::prepare(const NodePrepareContext& context) {
    if(!context.parameters) return Diagnostic{"parameters_missing","Speex node requires a parameter snapshot"};
    SpeexDenoiseConfig config;
    if(auto it=context.parameters->find("noise_suppress_db");it!=context.parameters->end()) {
        const auto* value=std::get_if<std::int64_t>(&it->second);
        if(!value) return Diagnostic{"parameter_type","noise_suppress_db must be integer"};
        if(*value < -60 || *value > 0) return Diagnostic{"parameter_out_of_range","noise_suppress_db must be -60..0"};
        config.noise_suppress_db=static_cast<int>(*value);
    }
    return processor_.prepare(config,context.format,context.maximum_block_frames);
}
AudioBlockOutcome DenoiseNode::process_block(const AudioBlockView& b) {
    if(!b.input_channels || !b.output_channels || b.channel_count==0)
        return Diagnostic{"invalid_audio_view","Speex node requires borrowed PCM channels"};
    for(std::size_t c=0;c<b.channel_count;++c) {
        const AudioInterval valid{0,b.frame_count};
        if(auto error=processor_.process({b.sample_rate_hz,c,{b.input_channels[c],b.frame_count},valid},
                                        {b.sample_rate_hz,c,{b.output_channels[c],b.frame_count},valid})) return *error;
    }
    return AudioBlockResult{b.frame_count};
}
NodeProcessOutcome DenoiseNode::process(const NodeProcessRequest&) {
    return Diagnostic{"denoise_owned_processing_unsupported","Speex denoise runs through the fixed-frame native wrapper"};
}
bool register_speex_nodes(NodeRegistry& registry) {
    ParameterDescriptor suppression{"noise_suppress_db",ParameterKind::integer,std::int64_t{-20},-60.0,0.0,"dB"};
    suppression.step=1; suppression.display_name="Noise suppression";
    suppression.help="SpeexDSP 1.2.1 noise attenuation; AGC, VAD and dereverb are disabled. Offline changes start a new instance.";
    NodeDescriptor d{"speex.denoise","1.2.1","Speex Denoise",
        {{"audio_in","audio.pcm@1",PortDirection::input,true},{"audio_out","audio.pcm@1",PortDirection::output,false}},
        {std::move(suppression)}};
    d.audio_format={{16000,48000},{1,2,4}};
    d.capabilities.supports_variable_block_size=false;d.capabilities.fixed_block_frames=480;
    d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    d.capabilities.latency_frames=480; // Original analysis/synthesis overlap, in addition to FIFO479.
    d.capabilities.uses_streaming_frame_api=true;
    // No public Speex flush API. Only bounded zero frames recover the declared overlap delay;
    // output beyond source length is discarded, not presented as a speech tail.
    d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
    d.capabilities.tail_frames=0;d.capabilities.maximum_zero_padding_frames=959;
    return registry.register_node(std::move(d),[]{return std::make_unique<DenoiseNode>();}).registered;
}
}
