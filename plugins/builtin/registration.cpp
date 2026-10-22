#include "plugins/builtin/registration.h"

#include "plugins/builtin/GainNodeAdapter.h"
#include "plugins/builtin/MixerNodeAdapter.h"
#include "plugins/examples/ChannelSwapNode.h"

#include <memory>
#include <string>
#include <utility>

namespace audioalgo {

bool register_builtin_nodes(NodeRegistry& registry) {
    const auto pcm = std::string("audio.pcm@1");
    ParameterDescriptor source_path{"path", ParameterKind::path, std::string{}, {}, {}, {}};
    source_path.display_name = "Audio file";
    source_path.help = "Path to the audio file used as the graph input.";
    source_path.input_resource = true;
    if (!registry.register_descriptor(NodeDescriptor{
            "builtin.file_source", "1.0.0", "File",
            {{"audio", pcm, PortDirection::output, false}},
            {std::move(source_path)}, NodeExecutionKind::source}))
        return false;

    ParameterDescriptor gain_parameter{"gain_db", ParameterKind::floating, 0.0, -60.0, 12.0, "dB"};
    gain_parameter.step = 0.1;
    gain_parameter.display_name = "Gain";
    gain_parameter.help = "Apply gain in decibels.";
    const auto gain = registry.register_node(NodeDescriptor{
        "builtin.gain", "1.0.0", "Gain",
        {{"audio_in", pcm, PortDirection::input, true}, {"audio_out", pcm, PortDirection::output, false}},
        {std::move(gain_parameter)}, NodeExecutionKind::block},
        [] { return std::make_unique<GainNodeAdapter>(); });
    if (!gain.registered)
        return false;

    ParameterDescriptor mixer_primary{"primary_gain_db", ParameterKind::floating, 0.0, -60.0, 12.0, "dB"};
    mixer_primary.step = 0.1;
    mixer_primary.display_name = "Primary gain";
    mixer_primary.help = "Gain applied to the primary input before summing, in decibels.";
    ParameterDescriptor mixer_secondary{"secondary_gain_db", ParameterKind::floating, 0.0, -60.0, 12.0, "dB"};
    mixer_secondary.step = 0.1;
    mixer_secondary.display_name = "Secondary gain";
    mixer_secondary.help = "Gain applied to the secondary input before summing, in decibels.";
    NodeDescriptor mixer{"builtin.mixer", "1.0.0", "Mixer",
        {{"primary", pcm, PortDirection::input, true},
         {"secondary", pcm, PortDirection::input, true},
         {"sum", pcm, PortDirection::output, false}},
        {std::move(mixer_primary), std::move(mixer_secondary)}, NodeExecutionKind::block};
    mixer.capabilities.supports_named_audio_ports = true;
    const auto mixer_registered = registry.register_node(std::move(mixer),
                                                         [] { return std::make_unique<MixerNodeAdapter>(); });
    if (!mixer_registered.registered)
        return false;

    if (!registry.register_descriptor(NodeDescriptor{
        "builtin.output", "1.0.0", "Output",
        {{"audio", pcm, PortDirection::input, true}}, {}, NodeExecutionKind::sink}))
        return false;
    return register_channel_swap_node(registry);
}

} // namespace audioalgo
