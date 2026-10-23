#pragma once
#include "audio_node_sdk/NodeSDK.h"
#include "audio_algorithms/SpeexDenoise.h"
namespace audioalgo {
class DenoiseNode final:public AudioNodeProcessor {
public:
    bool supports_block_processing() const noexcept override{return true;}
    bool supports_stream_processing() const noexcept override{return true;}
    AudioStreamOutcome process_stream(const AudioStreamBlockView&, AudioStreamProgress) override;
    std::optional<Diagnostic> prepare(const NodePrepareContext&) override;
    AudioBlockOutcome process_block(const AudioBlockView&) override;
    NodeProcessOutcome process(const NodeProcessRequest&) override;
    void reset() noexcept override{processor_.reset();}
private:
    SpeexDenoiseProcessor processor_;
};
[[nodiscard]] bool register_speex_nodes(NodeRegistry&);
}
