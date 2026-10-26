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
    struct FixedFrameBufferStats {
        std::size_t input_capacity = 0, output_capacity = 0, peak_input = 0, peak_output = 0;
    };
    [[nodiscard]] FixedFrameBufferStats fixed_frame_buffer_stats() const noexcept { return fixed_stats_; }
    // Absolute compensated output timeline. Configure before prepare; absent for live/test hosts.
    void set_finite_output_frames(std::size_t frames) noexcept { finite_output_frames_ = frames; }
    void set_finite_input_ends(std::vector<std::size_t> frames) { finite_input_ends_ = std::move(frames); }
    void set_finite_output_ends(std::vector<std::size_t> frames) { finite_output_ends_ = std::move(frames); }
    // Worker-side terminal handshake, also used by a truly empty finite stream.
    void complete_finite_stream();
    [[nodiscard]] std::optional<Diagnostic> drain_logs(bool final);
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
    void process_fixed_block(juce::AudioBuffer<float>& buffer);
    [[nodiscard]] bool process_stream_frame(std::size_t frame_start, std::size_t extent);

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
    std::size_t fixed_frames_ = 0, fixed_fill_ = 0, fixed_pending_ = 0, fixed_read_ = 0, fixed_leading_ = 0;
    std::vector<float> fixed_input_, fixed_output_;
    FixedFrameBufferStats fixed_stats_;
    std::optional<std::size_t> finite_output_frames_;
    std::size_t timeline_cursor_ = 0;
    std::vector<std::size_t> finite_input_ends_;
    std::vector<std::size_t> finite_output_ends_;
    bool streaming_processing_ = false;
    std::size_t maximum_no_progress_calls_ = 8;
    std::vector<std::size_t> stream_input_totals_, stream_output_totals_, stream_consumed_, stream_produced_;
    std::vector<std::uint8_t> stream_finished_, stream_output_finished_;
    std::vector<const float*> stream_input_channels_;
    std::vector<float*> stream_output_channels_;
    std::vector<AudioStreamInputPortBlock> stream_inputs_;
    std::vector<AudioStreamOutputPortBlock> stream_outputs_;
    std::optional<Diagnostic> diagnostic_;
};

} // namespace audioalgo
