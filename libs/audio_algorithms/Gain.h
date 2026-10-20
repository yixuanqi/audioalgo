#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <cstddef>
#include <variant>

namespace audioalgo {

using GainOutcome = std::variant<AudioBuffer, Diagnostic>;

struct GainConfig {
    double gain_db = 0.0;
};

enum class GainResetReason {
    new_run,
    discontinuity,
    parameter_change,
};

struct GainProcessResult {
    AudioInterval valid_interval;
    std::size_t processed_frames = 0;
};

using GainPrepareOutcome = std::variant<std::monostate, Diagnostic>;
using GainProcessOutcome = std::variant<GainProcessResult, Diagnostic>;

// A prepared processor borrows channel views only for the duration of process().
class GainProcessor {
public:
    [[nodiscard]] GainPrepareOutcome prepare(GainConfig config, AudioFormat format);
    [[nodiscard]] GainProcessOutcome process(AudioChannelView input, MutableAudioChannelView output) const;
    void reset(GainResetReason reason) noexcept;
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }

private:
    double linear_gain_ = 1.0;
    AudioFormat format_{};
    bool prepared_ = false;
};

[[nodiscard]] GainOutcome apply_gain_db(const AudioBuffer& input, double gain_db);

} // namespace audioalgo
