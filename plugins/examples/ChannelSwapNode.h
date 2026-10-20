#pragma once

#include "audio_algorithms/ChannelSwap.h"
#include "audio_node_sdk/NodeSDK.h"

namespace audioalgo {

class ChannelSwapNodeAdapter final : public AudioNodeProcessor {
public:
    [[nodiscard]] NodeProcessOutcome process(const NodeProcessRequest& request) override;
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] AudioBlockOutcome process_block(const AudioBlockView& block) override;
    [[nodiscard]] std::optional<Diagnostic> prepare(const NodePrepareContext& context) override;
    void reset() noexcept override;
private:
    [[nodiscard]] static std::optional<Diagnostic> read_enabled(const NodeParameters& parameters, bool& enabled);
    ChannelSwapProcessor processor_;
};

[[nodiscard]] bool register_channel_swap_node(NodeRegistry& registry);

} // namespace audioalgo
