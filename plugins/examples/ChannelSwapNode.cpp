#include "plugins/examples/ChannelSwapNode.h"

namespace audioalgo {

std::optional<Diagnostic> ChannelSwapNodeAdapter::read_enabled(const NodeParameters& parameters, bool& enabled) {
    const auto found = parameters.find("enabled");
    if (found == parameters.end())
        return std::nullopt;
    if (const auto* value = std::get_if<bool>(&found->second)) {
        enabled = *value;
        return std::nullopt;
    }
    return Diagnostic{"parameter_type", "ChannelSwap enabled must be a boolean", {}, "enabled"};
}

std::optional<Diagnostic> ChannelSwapNodeAdapter::prepare(const NodePrepareContext& context) {
    processor_.reset(ChannelSwapResetReason::new_run);
    if (context.parameters == nullptr)
        return Diagnostic{"parameters_missing", "ChannelSwap requires a prepared parameter snapshot"};
    bool enabled = true;
    if (const auto error = read_enabled(*context.parameters, enabled))
        return error;
    const auto prepared = processor_.prepare({enabled}, context.format);
    if (const auto* error = std::get_if<Diagnostic>(&prepared))
        return *error;
    return std::nullopt;
}

void ChannelSwapNodeAdapter::reset() noexcept { processor_.reset(ChannelSwapResetReason::new_run); }

AudioBlockOutcome ChannelSwapNodeAdapter::process_block(const AudioBlockView& block) {
    if (block.channel_count != 2 || block.input_channels == nullptr || block.output_channels == nullptr)
        return Diagnostic{"invalid_audio_view", "ChannelSwap requires two borrowed input/output channels"};
    for (std::size_t channel = 0; channel < 2; ++channel)
        if (block.frame_count != 0 && (block.input_channels[channel] == nullptr || block.output_channels[channel] == nullptr))
            return Diagnostic{"invalid_audio_view", "ChannelSwap borrowed channels must have sample storage"};
    const AudioInterval valid{0, block.frame_count};
    std::array<AudioChannelView, 2> input;
    std::array<MutableAudioChannelView, 2> output;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        input[channel] = {block.sample_rate_hz, channel, {block.input_channels[channel], block.frame_count}, valid};
        output[channel] = {block.sample_rate_hz, channel, {block.output_channels[channel], block.frame_count}, valid};
    }
    const auto result = processor_.process(input, output);
    if (const auto* error = std::get_if<Diagnostic>(&result))
        return *error;
    return AudioBlockResult{std::get<ChannelSwapProcessResult>(result).processed_frames};
}

NodeProcessOutcome ChannelSwapNodeAdapter::process(const NodeProcessRequest& request) {
    const auto input = request.inputs.find("audio_in");
    if (input == request.inputs.end())
        return Diagnostic{"input_missing", "ChannelSwap requires audio_in", {}, "audio_in"};
    bool enabled = true;
    if (const auto error = read_enabled(request.parameters, enabled))
        return *error;
    auto outcome = apply_channel_swap(input->second, enabled);
    if (const auto* error = std::get_if<Diagnostic>(&outcome))
        return *error;
    AudioPorts output;
    output.emplace("audio_out", std::move(std::get<AudioBuffer>(outcome)));
    return output;
}

bool register_channel_swap_node(NodeRegistry& registry) {
    ParameterDescriptor enabled{"enabled", ParameterKind::boolean, true, {}, {}, {}};
    enabled.display_name = "Swap channels";
    enabled.help = "Exchange left and right samples. Disable for unchanged channel positions.";
    NodeDescriptor descriptor{"example.cpp_channel_swap", "1.0.0", "Channel Swap",
        {{"audio_in", "audio.pcm@1", PortDirection::input, true},
         {"audio_out", "audio.pcm@1", PortDirection::output, false}},
        {enabled}, NodeExecutionKind::block};
    descriptor.audio_format.sample_rates_hz = {16000, 48000};
    descriptor.audio_format.channel_counts = {2};
    return registry.register_node(std::move(descriptor), [] { return std::make_unique<ChannelSwapNodeAdapter>(); }).registered;
}

} // namespace audioalgo
