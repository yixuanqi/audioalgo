#pragma once

#include "audio_algorithms/Mixer.h"
#include "audio_node_sdk/NodeSDK.h"

namespace audioalgo {

class MixerNodeAdapter final : public AudioNodeProcessor {
public:
    [[nodiscard]] NodeProcessOutcome process(const NodeProcessRequest& request) override;
    [[nodiscard]] bool supports_named_block_processing() const noexcept override { return true; }
    [[nodiscard]] AudioBlockOutcome process_named_block(const NamedAudioBlockView& block) override;
    [[nodiscard]] std::optional<Diagnostic> prepare(const NodePrepareContext& context) override;
    void reset() noexcept override;

private:
    [[nodiscard]] static std::optional<Diagnostic> read_gain(const NodeParameters& parameters,
                                                             const char* parameter_id, double& gain_db);

    MixerProcessor processor_;
    bool prepared_ = false;
};

} // namespace audioalgo
