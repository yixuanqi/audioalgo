#pragma once
#include "audio_core/AudioBuffer.h"
#include <algorithm>
#include <vector>
namespace audioalgo::editor {
struct WaveformRange {
    float minimum = 0.0f;
    float maximum = 0.0f;
};
using WaveformChannels = std::vector<std::vector<WaveformRange>>;

inline WaveformChannels make_waveform(const AudioBuffer& audio, std::size_t columns = 1200) {
    WaveformChannels result(audio.channel_count());
    if (audio.frame_count() == 0 || columns == 0)
        return result;
    const auto count = std::min(columns, audio.frame_count());
    for (std::size_t channel = 0; channel < audio.channel_count(); ++channel) {
        auto& ranges = result[channel];
        ranges.reserve(count);
        const auto samples = audio.channel(channel);
        for (std::size_t column = 0; column < count; ++column) {
            const auto first = column * audio.frame_count() / count;
            const auto last = (column + 1) * audio.frame_count() / count;
            auto minimum = samples[first];
            auto maximum = samples[first];
            for (auto frame = first + 1; frame < last; ++frame) {
                minimum = std::min(minimum, samples[frame]);
                maximum = std::max(maximum, samples[frame]);
            }
            ranges.push_back({minimum, maximum});
        }
    }
    return result;
}


} // namespace audioalgo::editor
