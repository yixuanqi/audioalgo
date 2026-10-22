#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <cstddef>
#include <variant>

namespace audioalgo {

// Production mixer contract: sum = 10^(primary_gain_db/20) * primary
//                             + 10^(secondary_gain_db/20) * secondary.
struct MixerConfig {
    double primary_gain_db = 0.0;
    double secondary_gain_db = 0.0;
};

enum class MixerResetReason {
    new_run,
    discontinuity,
    parameter_change,
};

struct MixerProcessResult {
    AudioInterval valid_interval;
    std::size_t processed_frames = 0;
};

using MixerPrepareOutcome = std::variant<std::monostate, Diagnostic>;
using MixerProcessOutcome = std::variant<MixerProcessResult, Diagnostic>;
using MixerOutcome = std::variant<AudioBuffer, Diagnostic>;

// A prepared processor borrows channel views only for the duration of process().
class MixerProcessor {
public:
    [[nodiscard]] MixerPrepareOutcome prepare(MixerConfig config, AudioFormat format);
    [[nodiscard]] MixerProcessOutcome process(AudioChannelView primary, AudioChannelView secondary,
                                              MutableAudioChannelView output) const;
    void reset(MixerResetReason reason) noexcept;
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }

private:
    double primary_weight_ = 1.0;
    double secondary_weight_ = 1.0;
    AudioFormat format_{};
    bool prepared_ = false;
};

[[nodiscard]] MixerOutcome mix_audio_db(const AudioBuffer& primary, const AudioBuffer& secondary,
                                        double primary_gain_db, double secondary_gain_db);

} // namespace audioalgo
