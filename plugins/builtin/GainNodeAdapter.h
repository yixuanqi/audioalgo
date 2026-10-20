#pragma once

#include "audio_algorithms/Gain.h"
#include "audio_node_sdk/NodeSDK.h"

namespace audioalgo {

class GainNodeAdapter final : public AudioNodeProcessor {
public:
    [[nodiscard]] NodeProcessOutcome process(const NodeProcessRequest& request) override;
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] AudioBlockOutcome process_block(const AudioBlockView& block) override;
    [[nodiscard]] std::optional<Diagnostic> prepare(const NodePrepareContext& context) override;
    void reset() noexcept override;

private:
    [[nodiscard]] static std::optional<Diagnostic> read_gain(const NodeParameters& parameters, double& gain_db);

    GainProcessor processor_;
    bool prepared_ = false;
};

} // namespace audioalgo
