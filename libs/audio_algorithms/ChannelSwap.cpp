#include "audio_algorithms/ChannelSwap.h"

#include <cmath>
#include <functional>

namespace audioalgo {
namespace {
bool overlaps(std::span<const float> a, std::span<const float> b) {
    if (a.empty() || b.empty())
        return false;
    const std::less<const float*> before;
    return before(a.data(), b.data() + b.size()) && before(b.data(), a.data() + a.size());
}
}

ChannelSwapPrepareOutcome ChannelSwapProcessor::prepare(ChannelSwapConfig config, AudioFormat format) {
    prepared_ = false;
    if ((format.sample_rate_hz != 16000 && format.sample_rate_hz != 48000) || format.channel_count != 2)
        return Diagnostic{"invalid_audio_format", "ChannelSwap requires 16000/48000 Hz stereo audio"};
    config_ = config;
    format_ = format;
    prepared_ = true;
    return std::monostate{};
}

ChannelSwapProcessOutcome ChannelSwapProcessor::process(const std::array<AudioChannelView, 2>& input,
    const std::array<MutableAudioChannelView, 2>& output) const {
    if (!prepared_)
        return Diagnostic{"not_prepared", "ChannelSwap must be prepared before processing"};
    const auto frames = input[0].samples.size();
    const auto valid = input[0].valid_interval;
    if (valid.first_frame > frames || valid.frame_count > frames - valid.first_frame)
        return Diagnostic{"invalid_audio_view", "ChannelSwap valid interval exceeds the borrowed view"};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        if (input[channel].sample_rate_hz != format_.sample_rate_hz ||
            output[channel].sample_rate_hz != format_.sample_rate_hz ||
            input[channel].channel_index != channel || output[channel].channel_index != channel)
            return Diagnostic{"invalid_audio_format", "ChannelSwap view does not match its prepared channel positions"};
        if (input[channel].samples.size() != frames || output[channel].samples.size() != frames ||
            input[channel].valid_interval != valid || output[channel].valid_interval != valid)
            return Diagnostic{"invalid_audio_view", "ChannelSwap requires matching stereo lengths and valid intervals"};
        for (std::size_t other = 0; other < 2; ++other)
            if (overlaps(input[channel].samples, output[other].samples))
                return Diagnostic{"aliased_audio_view", "ChannelSwap input and output storage must be separate"};
    }
    if (overlaps(output[0].samples, output[1].samples))
        return Diagnostic{"aliased_audio_view", "ChannelSwap output channels must have separate storage"};
    for (const auto& channel : input)
        for (const auto sample : channel.samples)
            if (!std::isfinite(sample))
                return Diagnostic{"non_finite_audio", "ChannelSwap input contains NaN or infinity"};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        output[0].samples[frame] = input[config_.enabled ? 1 : 0].samples[frame];
        output[1].samples[frame] = input[config_.enabled ? 0 : 1].samples[frame];
    }
    return ChannelSwapProcessResult{valid, frames};
}

void ChannelSwapProcessor::reset(ChannelSwapResetReason reason) noexcept {
    (void)reason;
    prepared_ = false;
    config_ = {};
    format_ = {};
}

ChannelSwapOutcome apply_channel_swap(const AudioBuffer& input, bool enabled) {
    if (const auto error = input.format_error(); !error.empty())
        return Diagnostic{"invalid_audio_format", error};
    ChannelSwapProcessor processor;
    const auto prepared = processor.prepare({enabled}, input.format());
    if (const auto* error = std::get_if<Diagnostic>(&prepared))
        return *error;
    AudioBuffer output = input;
    const auto result = processor.process({input.channel_view(0), input.channel_view(1)},
        {output.mutable_channel_view(0), output.mutable_channel_view(1)});
    if (const auto* error = std::get_if<Diagnostic>(&result))
        return *error;
    return output;
}

} // namespace audioalgo
