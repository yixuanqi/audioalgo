#pragma once

#include "audio_core/Diagnostic.h"

#include <filesystem>
#include <variant>

namespace audioalgo {

using AudioPreviewOutcome = std::variant<std::monostate, Diagnostic>;

// Host-provided output preview. Device and audio-framework types stay outside the application API.
class AudioPreviewService {
public:
    virtual ~AudioPreviewService() = default;
    [[nodiscard]] virtual AudioPreviewOutcome play(const std::filesystem::path& audio_file) = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;
    [[nodiscard]] virtual bool is_playing() const noexcept = 0;
    [[nodiscard]] virtual double position_seconds() const noexcept = 0;
};

} // namespace audioalgo
