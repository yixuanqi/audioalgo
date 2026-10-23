#pragma once
#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"
#include <memory>
#include <optional>

namespace audioalgo {
struct SpeexDenoiseConfig { int noise_suppress_db = -20; };
// Independent, stateful algorithm. One original Speex state per channel. Float32
// converts to saturated PCM16 by round-half-away-from-zero *32768, then /32768.
// Current explicit profile: frame480, 16/48kHz, 1/2/4 channels, denoise on,
// AGC/VAD/dereverb off, original overlap delay480. Reset releases state; prepare
// starts a new experiment. No graph, SDK, GUI, filesystem or JUCE dependency.
class SpeexDenoiseProcessor {
public:
    SpeexDenoiseProcessor();
    ~SpeexDenoiseProcessor();
    SpeexDenoiseProcessor(const SpeexDenoiseProcessor&) = delete;
    SpeexDenoiseProcessor& operator=(const SpeexDenoiseProcessor&) = delete;
    [[nodiscard]] std::optional<Diagnostic> prepare(SpeexDenoiseConfig, AudioFormat, std::size_t frame_size = 480);
    [[nodiscard]] std::optional<Diagnostic> process(AudioChannelView, MutableAudioChannelView);
    void reset() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
