#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <variant>

namespace audioalgo {

using GainOutcome = std::variant<AudioBuffer, Diagnostic>;

[[nodiscard]] GainOutcome apply_gain_db(const AudioBuffer& input, double gain_db);

} // namespace audioalgo
