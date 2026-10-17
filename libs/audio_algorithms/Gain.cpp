#include "audio_algorithms/Gain.h"

#include <cmath>

namespace audioalgo {

GainOutcome apply_gain_db(const AudioBuffer& input, double gain_db) {
    if (const auto error = input.format_error(); !error.empty())
        return Diagnostic{"invalid_audio_format", error};
    if (!std::isfinite(gain_db))
        return Diagnostic{"invalid_parameter", "Gain must be a finite number of decibels"};
    if (gain_db < -60.0 || gain_db > 12.0)
        return Diagnostic{"parameter_out_of_range", "Gain must be between -60 dB and +12 dB"};

    const double scale = std::pow(10.0, gain_db / 20.0);
    AudioBuffer output = input;
    for (std::size_t channel_index = 0; channel_index < input.channel_count(); ++channel_index) {
        const auto source = input.channel(channel_index);
        auto destination = output.mutable_channel(channel_index);
        for (std::size_t frame = 0; frame < source.size(); ++frame) {
            if (!std::isfinite(source[frame]))
                return Diagnostic{"non_finite_audio", "Audio contains NaN or infinity"};
            destination[frame] = static_cast<float>(static_cast<double>(source[frame]) * scale);
        }
    }
    return output;
}

} // namespace audioalgo
