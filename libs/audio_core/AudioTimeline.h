#pragma once
#include "audio_core/AudioBuffer.h"
#include <cstdint>
#include <optional>

namespace audioalgo {
// Maps a position, never a separately rounded block duration. Rounding is floor;
// invalid rates and overflow have no representable result. This does not resample PCM.
[[nodiscard]] std::optional<std::uint64_t> map_sample_frame(std::uint64_t position,
    std::uint32_t source_rate, std::uint32_t target_rate) noexcept;
[[nodiscard]] std::optional<AudioInterval> map_sample_interval(AudioInterval,
    std::uint32_t source_rate, std::uint32_t target_rate) noexcept;

// Exact streaming timebase with a carried rational remainder. A failed advance
// leaves all counters unchanged. Allocation and floating-point arithmetic are absent.
class SampleFrameClock {
public:
    SampleFrameClock(std::uint32_t source_rate, std::uint32_t target_rate);
    [[nodiscard]] bool advance(std::uint64_t source_frames) noexcept;
    void reset() noexcept { source_frames_ = target_frames_ = remainder_ = 0; }
    [[nodiscard]] std::uint64_t source_frames() const noexcept { return source_frames_; }
    [[nodiscard]] std::uint64_t target_frames() const noexcept { return target_frames_; }
    [[nodiscard]] std::uint64_t remainder() const noexcept { return remainder_; }
private:
    std::uint32_t source_rate_, target_rate_;
    std::uint64_t source_frames_ = 0, target_frames_ = 0, remainder_ = 0;
};
}
