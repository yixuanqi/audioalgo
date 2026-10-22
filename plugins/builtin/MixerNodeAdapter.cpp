#include "plugins/builtin/MixerNodeAdapter.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace audioalgo {
namespace {

constexpr std::string_view primary_port = "primary";
constexpr std::string_view secondary_port = "secondary";
constexpr std::string_view sum_port = "sum";

const AudioInputPortBlock* find_input_port(std::span<const AudioInputPortBlock> ports, std::string_view id) {
    const auto found = std::find_if(ports.begin(), ports.end(),
                                    [&](const AudioInputPortBlock& port) { return port.id == id; });
    return found == ports.end() ? nullptr : &*found;
}

const AudioOutputPortBlock* find_output_port(std::span<const AudioOutputPortBlock> ports, std::string_view id) {
    const auto found = std::find_if(ports.begin(), ports.end(),
                                    [&](const AudioOutputPortBlock& port) { return port.id == id; });
    return found == ports.end() ? nullptr : &*found;
}

} // namespace

std::optional<Diagnostic> MixerNodeAdapter::read_gain(const NodeParameters& parameters,
                                                      const char* parameter_id, double& gain_db) {
    const auto parameter = parameters.find(parameter_id);
    if (parameter == parameters.end())
        return std::nullopt; // Missing parameters keep the descriptor default of 0 dB.
    const auto* value = std::get_if<double>(&parameter->second);
    if (value == nullptr)
        return Diagnostic{"parameter_type", std::string("Mixer parameter ") + parameter_id +
                                                " must be a floating-point value"};
    gain_db = *value;
    return std::nullopt;
}

std::optional<Diagnostic> MixerNodeAdapter::prepare(const NodePrepareContext& context) {
    prepared_ = false;
    if (context.parameters == nullptr)
        return Diagnostic{"parameters_missing", "Mixer node requires a prepared parameter snapshot"};
    double primary_gain_db = 0.0;
    double secondary_gain_db = 0.0;
    if (const auto error = read_gain(*context.parameters, "primary_gain_db", primary_gain_db))
        return error;
    if (const auto error = read_gain(*context.parameters, "secondary_gain_db", secondary_gain_db))
        return error;
    const auto prepared = processor_.prepare(MixerConfig{primary_gain_db, secondary_gain_db}, context.format);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&prepared))
        return *diagnostic;
    prepared_ = true;
    return std::nullopt;
}

void MixerNodeAdapter::reset() noexcept {
    processor_.reset(MixerResetReason::new_run);
    prepared_ = false;
}

AudioBlockOutcome MixerNodeAdapter::process_named_block(const NamedAudioBlockView& block) {
    if (!prepared_)
        return Diagnostic{"not_prepared", "Mixer node must be prepared before processing a block"};
    const auto* primary = find_input_port(block.inputs, primary_port);
    const auto* secondary = find_input_port(block.inputs, secondary_port);
    const auto* sum = find_output_port(block.outputs, sum_port);
    if (primary == nullptr || secondary == nullptr || sum == nullptr)
        return Diagnostic{"named_port_shape", "Mixer requires primary/secondary inputs and a sum output"};
    if (primary->channels.size() != sum->channels.size() || secondary->channels.size() != sum->channels.size())
        return Diagnostic{"named_port_shape", "Mixer ports must share one channel count"};
    if (block.frame_count == 0)
        return AudioBlockResult{0};

    const AudioInterval interval{0, block.frame_count};
    std::size_t processed_frames = 0;
    for (std::size_t channel = 0; channel < sum->channels.size(); ++channel) {
        const AudioChannelView primary_view{block.sample_rate_hz, channel,
                                            std::span<const float>(primary->channels[channel], block.frame_count),
                                            interval};
        const AudioChannelView secondary_view{
            block.sample_rate_hz, channel,
            std::span<const float>(secondary->channels[channel], block.frame_count), interval};
        MutableAudioChannelView sum_view{block.sample_rate_hz, channel,
                                         std::span<float>(sum->channels[channel], block.frame_count), interval};
        const auto outcome = processor_.process(primary_view, secondary_view, sum_view);
        if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
            return *diagnostic;
        processed_frames = std::get<MixerProcessResult>(outcome).processed_frames;
    }
    return AudioBlockResult{processed_frames};
}

NodeProcessOutcome MixerNodeAdapter::process(const NodeProcessRequest& request) {
    const auto primary = request.inputs.find(std::string(primary_port));
    const auto secondary = request.inputs.find(std::string(secondary_port));
    if (primary == request.inputs.end() || secondary == request.inputs.end())
        return Diagnostic{"input_missing", "Mixer requires the primary and secondary ports"};

    double primary_gain_db = 0.0;
    double secondary_gain_db = 0.0;
    if (const auto error = read_gain(request.parameters, "primary_gain_db", primary_gain_db))
        return *error;
    if (const auto error = read_gain(request.parameters, "secondary_gain_db", secondary_gain_db))
        return *error;

    auto outcome = mix_audio_db(primary->second, secondary->second, primary_gain_db, secondary_gain_db);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&outcome))
        return *diagnostic;

    AudioPorts outputs;
    outputs.emplace(std::string(sum_port), std::move(std::get<AudioBuffer>(outcome)));
    return outputs;
}

} // namespace audioalgo
