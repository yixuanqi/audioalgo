#include "audio_algorithms/Gain.h"

#include <cmath>
#include <functional>

namespace audioalgo {

GainPrepareOutcome GainProcessor::prepare(GainConfig config, AudioFormat format) {
    prepared_ = false;
    if (format.sample_rate_hz != 16000 && format.sample_rate_hz != 48000)
        return Diagnostic{"invalid_audio_format", "Only 16000 Hz and 48000 Hz audio is supported"};
    if (format.channel_count != 1 && format.channel_count != 2 && format.channel_count != 4)
        return Diagnostic{"invalid_audio_format", "Audio must have 1, 2, or 4 channels"};
    if (!std::isfinite(config.gain_db))
        return Diagnostic{"invalid_parameter", "Gain must be a finite number of decibels"};
    if (config.gain_db < -60.0 || config.gain_db > 12.0)
        return Diagnostic{"parameter_out_of_range", "Gain must be between -60 dB and +12 dB"};

    linear_gain_ = std::pow(10.0, config.gain_db / 20.0);
    format_ = format;
    prepared_ = true;
    return std::monostate{};
}

GainProcessOutcome GainProcessor::process(AudioChannelView input, MutableAudioChannelView output) const {
    if (!prepared_)
        return Diagnostic{"not_prepared", "Gain processor must be prepared before processing"};
    if (input.sample_rate_hz != format_.sample_rate_hz || output.sample_rate_hz != format_.sample_rate_hz ||
        input.channel_index >= format_.channel_count || output.channel_index != input.channel_index)
        return Diagnostic{"invalid_audio_format", "Audio channel view does not match the prepared format"};
    if (input.samples.size() != output.samples.size() || input.valid_interval != output.valid_interval ||
        input.valid_interval.first_frame > input.samples.size() ||
        input.valid_interval.frame_count > input.samples.size() - input.valid_interval.first_frame)
        return Diagnostic{"invalid_audio_view", "Input and output channel views must have matching valid ranges"};

    if (!input.samples.empty()) {
        const float* input_begin = input.samples.data();
        const float* input_end = input_begin + input.samples.size();
        const float* output_begin = output.samples.data();
        const float* output_end = output_begin + output.samples.size();
        const std::less<const float*> before;
        if (before(input_begin, output_end) && before(output_begin, input_end))
            return Diagnostic{"aliased_audio_view", "Gain requires separate input and output channel storage"};
    }

    for (const float sample : input.samples) {
        if (!std::isfinite(sample))
            return Diagnostic{"non_finite_audio", "Audio contains NaN or infinity"};
    }
    for (std::size_t frame = 0; frame < input.samples.size(); ++frame)
        output.samples[frame] = static_cast<float>(static_cast<double>(input.samples[frame]) * linear_gain_);

    return GainProcessResult{input.valid_interval, input.samples.size()};
}

void GainProcessor::reset(GainResetReason reason) noexcept {
    (void)reason;
    prepared_ = false;
    format_ = {};
    linear_gain_ = 1.0;
}

GainOutcome apply_gain_db(const AudioBuffer& input, double gain_db) {
    if (const auto error = input.format_error(); !error.empty())
        return Diagnostic{"invalid_audio_format", error};
    AudioBuffer output = input;
    GainProcessor processor;
    const auto prepared = processor.prepare(GainConfig{gain_db}, input.format());
    if (const auto* diagnostic = std::get_if<Diagnostic>(&prepared))
        return *diagnostic;
    for (std::size_t channel_index = 0; channel_index < input.channel_count(); ++channel_index) {
        const auto outcome = processor.process(input.channel_view(channel_index),
                                               output.mutable_channel_view(channel_index));
        if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
            return *diagnostic;
    }
    return output;
}

} // namespace audioalgo
