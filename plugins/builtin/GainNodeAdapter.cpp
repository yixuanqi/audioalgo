#include "plugins/builtin/GainNodeAdapter.h"

#include <span>

namespace audioalgo {

std::optional<Diagnostic> GainNodeAdapter::read_gain(const NodeParameters& parameters, double& gain_db) {
    const auto parameter = parameters.find("gain_db");
    if (parameter == parameters.end())
        return std::nullopt;
    const auto* value = std::get_if<double>(&parameter->second);
    if (value == nullptr)
        return Diagnostic{"parameter_type", "Gain parameter gain_db must be a floating-point value"};
    gain_db = *value;
    return std::nullopt;
}

std::optional<Diagnostic> GainNodeAdapter::prepare(const NodePrepareContext& context) {
    prepared_ = false;
    if (context.parameters == nullptr)
        return Diagnostic{"parameters_missing", "Gain node requires a prepared parameter snapshot"};
    double gain_db = 0.0;
    if (const auto error = read_gain(*context.parameters, gain_db))
        return error;
    const auto prepared = processor_.prepare(GainConfig{gain_db}, context.format);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&prepared))
        return *diagnostic;
    prepared_ = true;
    return std::nullopt;
}

void GainNodeAdapter::reset() noexcept {
    processor_.reset(GainResetReason::new_run);
    prepared_ = false;
}

AudioBlockOutcome GainNodeAdapter::process_block(const AudioBlockView& block) {
    if (!prepared_)
        return Diagnostic{"not_prepared", "Gain node must be prepared before processing a block"};
    if (block.frame_count == 0 || block.channel_count == 0 || block.input_channels == nullptr ||
        block.output_channels == nullptr)
        return Diagnostic{"invalid_audio_view", "Gain requires a non-empty borrowed input and output block"};

    const AudioInterval interval{0, block.frame_count};
    std::size_t processed_frames = 0;
    for (std::size_t channel = 0; channel < block.channel_count; ++channel) {
        const AudioChannelView input{block.sample_rate_hz, channel,
                                     std::span<const float>(block.input_channels[channel], block.frame_count), interval};
        MutableAudioChannelView output{block.sample_rate_hz, channel,
                                       std::span<float>(block.output_channels[channel], block.frame_count), interval};
        const auto outcome = processor_.process(input, output);
        if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
            return *diagnostic;
        processed_frames = std::get<GainProcessResult>(outcome).processed_frames;
    }
    return AudioBlockResult{processed_frames};
}

NodeProcessOutcome GainNodeAdapter::process(const NodeProcessRequest& request) {
    const auto input = request.inputs.find("audio_in");
    if (input == request.inputs.end())
        return Diagnostic{"input_missing", "Gain requires the audio_in port"};

    double gain_db = 0.0;
    if (const auto error = read_gain(request.parameters, gain_db))
        return *error;

    auto outcome = apply_gain_db(input->second, gain_db);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
        return *diagnostic;

    AudioPorts outputs;
    outputs.emplace("audio_out", std::move(std::get<AudioBuffer>(outcome)));
    return outputs;
}

} // namespace audioalgo
