#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <cstddef>
#include <filesystem>
#include <variant>

namespace audioalgo {

enum class WavEncoding { pcm16, float32 };

struct WavWriteSummary {
    std::size_t frames = 0;
    std::size_t channels = 0;
    std::size_t clipped_samples = 0;
};

using AudioReadOutcome = std::variant<AudioBuffer, Diagnostic>;
using AudioWriteOutcome = std::variant<WavWriteSummary, Diagnostic>;

[[nodiscard]] AudioReadOutcome read_wav_file(const std::filesystem::path& path);
[[nodiscard]] AudioWriteOutcome write_wav_file(const std::filesystem::path& path, const AudioBuffer& audio,
                                                WavEncoding encoding, bool allow_overwrite = false);

} // namespace audioalgo
