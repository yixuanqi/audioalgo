#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <array>
#include <variant>

namespace audioalgo {

struct ChannelSwapConfig { bool enabled = true; };
enum class ChannelSwapResetReason { new_run, discontinuity, parameter_change };
struct ChannelSwapProcessResult {
    AudioInterval valid_interval;
    std::size_t processed_frames = 0;
};
using ChannelSwapPrepareOutcome = std::variant<std::monostate, Diagnostic>;
using ChannelSwapProcessOutcome = std::variant<ChannelSwapProcessResult, Diagnostic>;
using ChannelSwapOutcome = std::variant<AudioBuffer, Diagnostic>;

// Views are borrowed for this call only. Input/output channel storage must be separate.
class ChannelSwapProcessor {
public:
    [[nodiscard]] ChannelSwapPrepareOutcome prepare(ChannelSwapConfig config, AudioFormat format);
    [[nodiscard]] ChannelSwapProcessOutcome process(
        const std::array<AudioChannelView, 2>& input,
        const std::array<MutableAudioChannelView, 2>& output) const;
    void reset(ChannelSwapResetReason reason) noexcept;
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }
private:
    ChannelSwapConfig config_;
    AudioFormat format_{};
    bool prepared_ = false;
};

[[nodiscard]] ChannelSwapOutcome apply_channel_swap(const AudioBuffer& input, bool enabled = true);

} // namespace audioalgo
