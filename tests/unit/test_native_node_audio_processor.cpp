#include "audio_juce_backend/NativeNodeAudioProcessor.h"
#include "plugins/builtin/GainNodeAdapter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class OffsetNodeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported",
                                     "offset test node only implements the native block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        for (std::size_t channel = 0; channel < block.channel_count; ++channel) {
            const auto offset = static_cast<float>(0.125 * static_cast<double>(channel + 1));
            for (std::size_t frame = 0; frame < block.frame_count; ++frame)
                block.output_channels[channel][frame] = block.input_channels[channel][frame] + offset;
        }
        return audioalgo::AudioBlockResult{block.frame_count};
    }
};

audioalgo::NodeDescriptor block_descriptor(std::string type_id,
                                           std::string input_port,
                                           std::string output_port) {
    return {std::move(type_id), "1.0.0", "Block processor",
            {{std::move(input_port), "audio.pcm@1", audioalgo::PortDirection::input, true},
             {std::move(output_port), "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
}

audioalgo::AudioBuffer run_wrapper(std::unique_ptr<audioalgo::AudioNodeProcessor> processor,
                                   std::string node_id,
                                   audioalgo::NodeDescriptor descriptor,
                                   audioalgo::NodeParameters parameters,
                                   const audioalgo::AudioBuffer& input) {
    audioalgo::NativeNodeAudioProcessor wrapper(std::move(processor), std::move(node_id), descriptor,
                                                std::move(parameters), {},
                                                static_cast<int>(input.channel_count()));
    wrapper.prepareToPlay(static_cast<double>(input.sample_rate_hz()), static_cast<int>(input.frame_count()));
    juce::AudioBuffer<float> block(static_cast<int>(input.channel_count()),
                                   static_cast<int>(input.frame_count()));
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        block.copyFrom(static_cast<int>(channel), 0, input.channel(channel).data(), static_cast<int>(input.frame_count()));
    juce::MidiBuffer midi;
    wrapper.processBlock(block, midi);
    if (wrapper.processing_diagnostic())
        throw std::runtime_error(wrapper.processing_diagnostic()->message);

    audioalgo::AudioBuffer output(input.sample_rate_hz(), input.channel_ids(), input.frame_count());
    for (std::size_t channel = 0; channel < output.channel_count(); ++channel)
        std::copy_n(block.getReadPointer(static_cast<int>(channel)), input.frame_count(),
                    output.mutable_channel(channel).data());
    return output;
}

} // namespace

TEST_CASE("AR-07.a Gain and a test processor execute through one generic JUCE wrapper", "[R1][AR-07.a]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        auto samples = input.mutable_channel(channel);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
            samples[frame] = static_cast<float>((channel == 0 ? 0.31 : -0.27) *
                                                std::sin(static_cast<double>(frame + 1) * (0.019 + 0.011 * channel)));
    }

    constexpr double gain_db = 6.020599913279624;
    const auto gain = std::pow(10.0, gain_db / 20.0);
    const auto gain_output = run_wrapper(std::make_unique<audioalgo::GainNodeAdapter>(), "gain-instance",
                                         block_descriptor("builtin.gain", "audio_in", "audio_out"),
                                         {{"gain_db", gain_db}}, input);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            CHECK(gain_output.channel(channel)[frame] == Catch::Approx(input.channel(channel)[frame] * gain).margin(1.0e-6));

    const auto offset_output = run_wrapper(std::make_unique<OffsetNodeProcessor>(), "offset-instance",
                                           block_descriptor("test.offset", "input", "output"), {}, input);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        const auto offset = static_cast<double>(0.125 * static_cast<double>(channel + 1));
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            CHECK(offset_output.channel(channel)[frame] == Catch::Approx(input.channel(channel)[frame] + offset).margin(1.0e-7));
    }
}
