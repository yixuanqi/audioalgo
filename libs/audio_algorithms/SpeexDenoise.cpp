#include "SpeexDenoise.h"
#include <speex/speex_preprocess.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace audioalgo {
struct SpeexDenoiseProcessor::Impl {
    using State = std::unique_ptr<SpeexPreprocessState, decltype(&speex_preprocess_state_destroy)>;
    AudioFormat format;
    std::size_t frame_size=0;
    std::vector<State> states;
    std::vector<spx_int16_t> pcm;
};
SpeexDenoiseProcessor::SpeexDenoiseProcessor()=default;
SpeexDenoiseProcessor::~SpeexDenoiseProcessor()=default;
void SpeexDenoiseProcessor::reset() noexcept { impl_.reset(); }
std::optional<Diagnostic> SpeexDenoiseProcessor::prepare(SpeexDenoiseConfig config, AudioFormat format, std::size_t frame) {
    reset();
    if((format.sample_rate_hz!=16000 && format.sample_rate_hz!=48000) ||
       (format.channel_count!=1 && format.channel_count!=2 && format.channel_count!=4) || frame!=480)
        return Diagnostic{"denoise_format_unsupported","Speex profile requires frame480, 16/48kHz and 1/2/4 channels"};
    if(config.noise_suppress_db < -60 || config.noise_suppress_db > 0)
        return Diagnostic{"denoise_parameter_range","Noise suppression must be between -60 and 0 dB"};
    auto prepared=std::make_unique<Impl>();
    prepared->format=format;prepared->frame_size=frame;prepared->pcm.resize(frame);
    for(std::size_t c=0;c<format.channel_count;++c) {
        Impl::State state(speex_preprocess_state_init(static_cast<int>(frame),static_cast<int>(format.sample_rate_hz)),speex_preprocess_state_destroy);
        if(!state) return Diagnostic{"denoise_state_create_failed","Original Speex state allocation failed"};
        int enabled=1,disabled=0;
        if(speex_preprocess_ctl(state.get(),SPEEX_PREPROCESS_SET_DENOISE,&enabled)!=0 ||
           speex_preprocess_ctl(state.get(),SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&config.noise_suppress_db)!=0 ||
           speex_preprocess_ctl(state.get(),SPEEX_PREPROCESS_SET_AGC,&disabled)!=0 ||
           speex_preprocess_ctl(state.get(),SPEEX_PREPROCESS_SET_DEREVERB,&disabled)!=0)
            return Diagnostic{"denoise_parameter_rejected","Original Speex rejected the configuration"};
        // Pinned preprocess.c initializes VAD off. SET_VAD warns even when asked
        // to disable it; query and verify that default without enabling that mode.
        int vad=-1;
        if(speex_preprocess_ctl(state.get(),SPEEX_PREPROCESS_GET_VAD,&vad)!=0 || vad!=0)
            return Diagnostic{"denoise_vad_configuration","Speex VAD must remain disabled"};
        prepared->states.push_back(std::move(state));
    }
    impl_=std::move(prepared);
    return {};
}
std::optional<Diagnostic> SpeexDenoiseProcessor::process(AudioChannelView input, MutableAudioChannelView output) {
    if(!impl_) return Diagnostic{"denoise_not_prepared","Speex denoise requires prepare after reset"};
    if(input.sample_rate_hz!=impl_->format.sample_rate_hz || output.sample_rate_hz!=input.sample_rate_hz ||
       input.channel_index>=impl_->format.channel_count || output.channel_index!=input.channel_index ||
       input.samples.size()!=impl_->frame_size || output.samples.size()!=input.samples.size())
        return Diagnostic{"denoise_block_shape","Speex denoise requires one prepared, complete channel frame"};
    for(const auto sample:input.samples) if(!std::isfinite(sample))
        return Diagnostic{"denoise_input_non_finite","Speex input must be finite"};
    for(std::size_t i=0;i<impl_->frame_size;++i) {
        const auto scaled=std::lround(std::clamp(static_cast<double>(input.samples[i]),-1.0,1.0)*32768.0);
        impl_->pcm[i]=static_cast<spx_int16_t>(std::clamp<long>(scaled,-32768,32767));
    }
    speex_preprocess_run(impl_->states[input.channel_index].get(),impl_->pcm.data());
    for(std::size_t i=0;i<impl_->frame_size;++i) output.samples[i]=static_cast<float>(impl_->pcm[i])/32768.0f;
    return {};
}
}
