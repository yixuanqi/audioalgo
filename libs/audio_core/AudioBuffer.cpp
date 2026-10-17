#include "audio_core/AudioBuffer.h"

#include <stdexcept>
#include <unordered_set>

namespace audioalgo {

AudioBuffer::AudioBuffer(std::uint32_t sample_rate_hz, std::vector<std::string> channel_ids, std::size_t frames)
    : AudioBuffer(sample_rate_hz, std::move(channel_ids), frames, AudioInterval{0, frames}) {}

AudioBuffer::AudioBuffer(std::uint32_t sample_rate_hz, std::vector<std::string> channel_ids, std::size_t frames,
                         AudioInterval valid_interval)
    : sample_rate_hz_(sample_rate_hz), channel_ids_(std::move(channel_ids)), frame_count_(frames),
      valid_interval_(valid_interval) {
    channels_.resize(channel_ids_.size(), std::vector<float>(frames));
}

std::span<const float> AudioBuffer::channel(std::size_t index) const {
    if (index >= channels_.size())
        throw std::out_of_range("AudioBuffer channel index is out of range");
    return channels_[index];
}

std::span<float> AudioBuffer::mutable_channel(std::size_t index) {
    if (index >= channels_.size())
        throw std::out_of_range("AudioBuffer channel index is out of range");
    return channels_[index];
}

std::string AudioBuffer::format_error() const {
    if (sample_rate_hz_ != 16000 && sample_rate_hz_ != 48000)
        return "Only 16000 Hz and 48000 Hz audio is supported";
    if (channels_.size() != 1 && channels_.size() != 2 && channels_.size() != 4)
        return "Audio must have 1, 2, or 4 channels";
    if (channel_ids_.size() != channels_.size())
        return "Channel IDs do not match the planar audio channels";
    if (valid_interval_.first_frame > frame_count_ ||
        valid_interval_.frame_count > frame_count_ - valid_interval_.first_frame)
        return "Valid audio interval must fit inside the buffer frame range";

    std::unordered_set<std::string> unique_ids;
    for (const auto& channel_id : channel_ids_) {
        if (channel_id.empty())
            return "Channel IDs must not be empty";
        if (!unique_ids.insert(channel_id).second)
            return "Channel IDs must be unique";
    }
    return {};
}

} // namespace audioalgo
