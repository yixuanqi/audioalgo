#include "audio_algorithms/Mixer.h"

#include <cmath>
#include <functional>

namespace audioalgo {
namespace {

constexpr double minimum_gain_db = -60.0;
constexpr double maximum_gain_db = 12.0;

} // namespace

MixerPrepareOutcome MixerProcessor::prepare(MixerConfig config, AudioFormat format) {
    prepared_ = false;
    if (format.sample_rate_hz == 0 || format.channel_count == 0)
        return Diagnostic{"invalid_audio_format", "Mixer requires a nonempty audio format"};
    if (!std::isfinite(config.primary_gain_db) || !std::isfinite(config.secondary_gain_db))
        return Diagnostic{"invalid_parameter", "Mixer gains must be finite numbers of decibels"};
    if (config.primary_gain_db < minimum_gain_db || config.primary_gain_db > maximum_gain_db)
        return Diagnostic{"parameter_out_of_range", "Primary gain must be between -60 dB and +12 dB"};
    if (config.secondary_gain_db < minimum_gain_db || config.secondary_gain_db > maximum_gain_db)
        return Diagnostic{"parameter_out_of_range", "Secondary gain must be between -60 dB and +12 dB"};

    primary_weight_ = std::pow(10.0, config.primary_gain_db / 20.0);
    secondary_weight_ = std::pow(10.0, config.secondary_gain_db / 20.0);
    format_ = format;
    prepared_ = true;
    return std::monostate{};
}

MixerProcessOutcome MixerProcessor::process(AudioChannelView primary, AudioChannelView secondary,
                                            MutableAudioChannelView output) const {
    if (!prepared_)
        return Diagnostic{"not_prepared", "Mixer processor must be prepared before processing"};
    if (primary.sample_rate_hz != format_.sample_rate_hz || secondary.sample_rate_hz != format_.sample_rate_hz ||
        output.sample_rate_hz != format_.sample_rate_hz)
        return Diagnostic{"invalid_audio_format", "Audio channel views do not match the prepared sample rate"};
    if (primary.channel_index >= format_.channel_count || primary.channel_index != secondary.channel_index ||
        primary.channel_index != output.channel_index)
        return Diagnostic{"invalid_audio_format", "Audio channel views must share one prepared channel index"};
    if (primary.samples.size() != secondary.samples.size() || primary.samples.size() != output.samples.size() ||
        primary.valid_interval != secondary.valid_interval || primary.valid_interval != output.valid_interval ||
        primary.valid_interval.first_frame > primary.samples.size() ||
        primary.valid_interval.frame_count > primary.samples.size() - primary.valid_interval.first_frame)
        return Diagnostic{"invalid_audio_view", "Mixer channel views must have matching valid ranges"};

    if (!primary.samples.empty()) {
        const std::less<const float*> before;
        const float* primary_begin = primary.samples.data();
        const float* primary_end = primary_begin + primary.samples.size();
        const float* secondary_begin = secondary.samples.data();
        const float* secondary_end = secondary_begin + secondary.samples.size();
        float* const output_begin = output.samples.data();
        float* const output_end = output_begin + output.samples.size();
        if (before(primary_begin, output_end) && before(output_begin, primary_end))
            return Diagnostic{"aliased_audio_view", "Mixer requires input storage separate from the output"};
        if (before(secondary_begin, output_end) && before(output_begin, secondary_end))
            return Diagnostic{"aliased_audio_view", "Mixer requires input storage separate from the output"};
    }

    for (const float sample : primary.samples)
        if (!std::isfinite(sample))
            return Diagnostic{"non_finite_audio", "Primary audio contains NaN or infinity"};
    for (const float sample : secondary.samples)
        if (!std::isfinite(sample))
            return Diagnostic{"non_finite_audio", "Secondary audio contains NaN or infinity"};

    for (std::size_t frame = 0; frame < primary.samples.size(); ++frame)
        output.samples[frame] = static_cast<float>(static_cast<double>(primary.samples[frame]) * primary_weight_ +
                                                   static_cast<double>(secondary.samples[frame]) * secondary_weight_);

    return MixerProcessResult{primary.valid_interval, primary.samples.size()};
}

void MixerProcessor::reset(MixerResetReason reason) noexcept {
    (void)reason;
    prepared_ = false;
    format_ = {};
    primary_weight_ = 1.0;
    secondary_weight_ = 1.0;
}

MixerOutcome mix_audio_db(const AudioBuffer& primary, const AudioBuffer& secondary,
                          double primary_gain_db, double secondary_gain_db) {
    if (const auto error = primary.format_error(); !error.empty())
        return Diagnostic{"invalid_audio_format", error};
    if (const auto error = secondary.format_error(); !error.empty())
        return Diagnostic{"invalid_audio_format", error};
    if (primary.format() != secondary.format() || primary.frame_count() != secondary.frame_count())
        return Diagnostic{"invalid_audio_format", "Mixer inputs must share one audio format and frame count"};

    AudioBuffer output = primary;
    MixerProcessor processor;
    const auto prepared = processor.prepare(MixerConfig{primary_gain_db, secondary_gain_db}, primary.format());
    if (const auto* diagnostic = std::get_if<Diagnostic>(&prepared))
        return *diagnostic;
    for (std::size_t channel_index = 0; channel_index < primary.channel_count(); ++channel_index) {
        const auto outcome = processor.process(primary.channel_view(channel_index),
                                               secondary.channel_view(channel_index),
                                               output.mutable_channel_view(channel_index));
        if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
            return *diagnostic;
    }
    return output;
}

} // namespace audioalgo
