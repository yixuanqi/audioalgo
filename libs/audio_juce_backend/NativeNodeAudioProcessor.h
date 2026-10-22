#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_node_sdk/NodeSDK.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace audioalgo {

// Adapts one SDK node instance to JUCE's block processor contract. It is deliberately
// unaware of node type IDs and delegates all node behavior to the injected instance.
class NativeNodeAudioProcessor final : public juce::AudioProcessor {
public:
    NativeNodeAudioProcessor(std::unique_ptr<AudioNodeProcessor> processor,
                             std::string node_id,
                             const NodeDescriptor& descriptor,
                             NodeParameters parameters,
                             NodeExecutionContext context,
                             int channel_count);

    const juce::String getName() const override { return "AudioAlgo native node"; }
    void prepareToPlay(double sample_rate_hz, int maximum_expected_samples_per_block) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    [[nodiscard]] const std::optional<Diagnostic>& processing_diagnostic() const noexcept { return diagnostic_; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

protected:
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    [[nodiscard]] static BusesProperties buses_for(const NodeDescriptor& descriptor, int channel_count);

    std::unique_ptr<AudioNodeProcessor> processor_;
    std::string node_id_;
    std::string input_port_id_;
    std::string output_port_id_;
    NodeParameters parameters_;
    NodeExecutionContext context_;
    int channel_count_ = 0;
    int maximum_block_frames_ = 0;
    std::uint32_t sample_rate_hz_ = 0;
    bool structurally_invalid_ = false;
    bool named_processing_ = false;
    std::vector<std::string> input_port_ids_;
    std::vector<std::string> output_port_ids_;
    // Preallocated in prepareToPlay(); processBlock only assigns into these, never resizes them.
    std::vector<float> input_scratch_;
    std::vector<const float*> input_channels_;
    std::vector<float*> output_channels_;
    std::vector<AudioInputPortBlock> named_inputs_;
    std::vector<AudioOutputPortBlock> named_outputs_;
    std::optional<Diagnostic> diagnostic_;
};

} // namespace audioalgo
