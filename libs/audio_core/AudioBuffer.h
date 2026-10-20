#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace audioalgo {

struct AudioInterval {
    std::size_t first_frame = 0;
    std::size_t frame_count = 0;

    bool operator==(const AudioInterval&) const = default;
};

struct AudioFormat {
    std::uint32_t sample_rate_hz = 0;
    std::size_t channel_count = 0;

    bool operator==(const AudioFormat&) const = default;
};

// Non-owning channel views. Their sample spans are valid only while the owning AudioBuffer
// remains alive and unmodified in a way that reallocates its channel storage.
struct AudioChannelView {
    std::uint32_t sample_rate_hz = 0;
    std::size_t channel_index = 0;
    std::span<const float> samples;
    AudioInterval valid_interval;
};

struct MutableAudioChannelView {
    std::uint32_t sample_rate_hz = 0;
    std::size_t channel_index = 0;
    std::span<float> samples;
    AudioInterval valid_interval;
};

class AudioBuffer {
public:
    AudioBuffer(std::uint32_t sample_rate_hz, std::vector<std::string> channel_ids, std::size_t frames);
    AudioBuffer(std::uint32_t sample_rate_hz, std::vector<std::string> channel_ids, std::size_t frames,
                AudioInterval valid_interval);

    [[nodiscard]] std::uint32_t sample_rate_hz() const noexcept { return sample_rate_hz_; }
    [[nodiscard]] std::size_t channel_count() const noexcept { return channels_.size(); }
    [[nodiscard]] std::size_t frame_count() const noexcept { return channels_.empty() ? frame_count_ : channels_.front().size(); }
    [[nodiscard]] AudioInterval valid_interval() const noexcept { return valid_interval_; }
    [[nodiscard]] const std::vector<std::string>& channel_ids() const noexcept { return channel_ids_; }
    [[nodiscard]] std::span<const float> channel(std::size_t index) const;
    [[nodiscard]] std::span<float> mutable_channel(std::size_t index);
    [[nodiscard]] AudioChannelView channel_view(std::size_t index) const;
    [[nodiscard]] MutableAudioChannelView mutable_channel_view(std::size_t index);
    [[nodiscard]] AudioFormat format() const noexcept { return {sample_rate_hz_, channels_.size()}; }

    [[nodiscard]] std::string format_error() const;

private:
    std::uint32_t sample_rate_hz_;
    std::vector<std::string> channel_ids_;
    std::vector<std::vector<float>> channels_;
    std::size_t frame_count_;
    AudioInterval valid_interval_;
};

} // namespace audioalgo
