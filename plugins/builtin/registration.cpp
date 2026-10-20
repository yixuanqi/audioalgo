#include "plugins/builtin/registration.h"

#include "plugins/builtin/GainNodeAdapter.h"
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

    if (!registry.register_descriptor(NodeDescriptor{
        "builtin.output", "1.0.0", "Output",
        {{"audio", pcm, PortDirection::input, true}}, {}, NodeExecutionKind::sink}))
        return false;
    return register_channel_swap_node(registry);
}

} // namespace audioalgo
