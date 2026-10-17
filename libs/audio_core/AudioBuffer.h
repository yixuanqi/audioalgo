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

    [[nodiscard]] std::string format_error() const;

private:
    std::uint32_t sample_rate_hz_;
    std::vector<std::string> channel_ids_;
    std::vector<std::vector<float>> channels_;
    std::size_t frame_count_;
    AudioInterval valid_interval_;
};

} // namespace audioalgo
