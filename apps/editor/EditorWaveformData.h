#pragma once
#include "audio_core/AudioBuffer.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
namespace audioalgo::editor {
struct WaveformRange {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct WaveformPyramid {
    std::size_t frames = 0;
    std::uint32_t sample_rate_hz = 0;
    std::size_t finest_bucket_frames = 1;
    // Each level halves the preceding level; every bucket retains both extrema.
    std::vector<std::vector<std::vector<WaveformRange>>> levels;
};

struct WaveformChannels : std::vector<std::vector<WaveformRange>> {
    using std::vector<std::vector<WaveformRange>>::vector;
    std::shared_ptr<const WaveformPyramid> detail;
};

inline WaveformChannels make_waveform(const AudioBuffer& audio, std::size_t columns = 1200) {
    WaveformChannels result(audio.channel_count());
    if (audio.frame_count() == 0 || audio.channel_count() == 0 || columns == 0)
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
    // Bound all zoom levels to roughly 8 MiB of min/max data for long renders.
    // This is prepared by EditorRunJob's worker, before delivery to the UI.
    constexpr std::size_t maximum_cached_ranges = 1'048'576;
    const auto channels = audio.channel_count();
    const auto finest_limit = std::max<std::size_t>(1, maximum_cached_ranges / (2 * channels));
    auto detail = std::make_shared<WaveformPyramid>();
    detail->frames = audio.frame_count();
    detail->sample_rate_hz = audio.sample_rate_hz();
    while ((audio.frame_count() - 1) / detail->finest_bucket_frames + 1 > finest_limit)
        detail->finest_bucket_frames *= 2;
    detail->levels.emplace_back(channels);
    const auto finest_count = (audio.frame_count() - 1) / detail->finest_bucket_frames + 1;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto& buckets = detail->levels.front()[channel];
        buckets.reserve(finest_count);
        const auto samples = audio.channel(channel);
        for (std::size_t bucket = 0; bucket < finest_count; ++bucket) {
            const auto first = bucket * detail->finest_bucket_frames;
            const auto last = first + std::min(detail->finest_bucket_frames, audio.frame_count() - first);
            WaveformRange range{samples[first], samples[first]};
            for (auto frame = first + 1; frame < last; ++frame) {
                range.minimum = std::min(range.minimum, samples[frame]);
                range.maximum = std::max(range.maximum, samples[frame]);
            }
            buckets.push_back(range);
        }
    }
    while (detail->levels.back().front().size() > 1) {
        const auto& previous = detail->levels.back();
        auto next = std::vector<std::vector<WaveformRange>>(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto& source = previous[channel];
            auto& target = next[channel];
            target.reserve((source.size() + 1) / 2);
            for (std::size_t index = 0; index < source.size(); index += 2) {
                auto range = source[index];
                if (index + 1 < source.size()) {
                    range.minimum = std::min(range.minimum, source[index + 1].minimum);
                    range.maximum = std::max(range.maximum, source[index + 1].maximum);
                }
                target.push_back(range);
            }
        }
        detail->levels.push_back(std::move(next));
    }
    result.detail = std::move(detail);
    return result;
}

inline WaveformRange waveform_visible_range(const WaveformChannels& waveform, std::size_t channel,
                                            std::size_t first_frame, std::size_t last_frame,
                                            double frames_per_pixel) {
    const auto& pyramid = *waveform.detail;
    auto level = std::size_t{0};
    auto bucket_frames = pyramid.finest_bucket_frames;
    while (level + 1 < pyramid.levels.size() && bucket_frames <= frames_per_pixel / 2.0) {
        ++level;
        bucket_frames *= 2;
    }
    const auto& buckets = pyramid.levels[level][channel];
    const auto first = std::min(first_frame / bucket_frames, buckets.size() - 1);
    const auto last = std::min((last_frame - 1) / bucket_frames, buckets.size() - 1);
    auto range = buckets[first];
    for (auto index = first + 1; index <= last; ++index) {
        range.minimum = std::min(range.minimum, buckets[index].minimum);
        range.maximum = std::max(range.maximum, buckets[index].maximum);
    }
    return range;
}


} // namespace audioalgo::editor
