#include "audio_node_sdk/NodeSDK.h"

#include <algorithm>
#include <set>

namespace audioalgo {

bool NodeRegistry::register_descriptor(NodeDescriptor descriptor) {
    if (descriptor.type_id.empty() || descriptor.version.empty() || descriptor.display_name.empty())
        return false;
    if (descriptors_.contains(descriptor.type_id))
        return false;

    std::set<std::string> port_ids;
    for (const auto& port : descriptor.ports) {
        if (port.id.empty() || port.type_id.empty() || !port_ids.insert(port.id).second)
            return false;
    }
    std::set<std::string> parameter_ids;
    for (const auto& parameter : descriptor.parameters) {
        if (parameter.id.empty() || !parameter_ids.insert(parameter.id).second)
            return false;
        if (parameter.minimum && parameter.maximum && *parameter.minimum > *parameter.maximum)
            return false;
    }
    descriptors_.emplace(descriptor.type_id, std::move(descriptor));
    return true;
}

bool NodeRegistry::register_factory(std::string type_id, NodeFactory factory) {
    if (type_id.empty() || !factory || !descriptors_.contains(type_id) || factories_.contains(type_id))
        return false;
    factories_.emplace(std::move(type_id), std::move(factory));
    return true;
}

const NodeDescriptor* NodeRegistry::find(const std::string& type_id) const noexcept {
    const auto it = descriptors_.find(type_id);
    return it == descriptors_.end() ? nullptr : &it->second;
}

std::unique_ptr<AudioNodeProcessor> NodeRegistry::create_processor(const std::string& type_id) const {
    const auto it = factories_.find(type_id);
    return it == factories_.end() ? nullptr : it->second();
}

void register_builtin_node_descriptors(NodeRegistry& registry) {
    const auto pcm = std::string("audio.pcm@1");
    (void)registry.register_descriptor(NodeDescriptor{
        "builtin.file_source", "1.0.0", "File",
        {{"audio", pcm, PortDirection::output, false}},
        {{"path", ParameterKind::path, std::string{}, {}, {}, {}}}, NodeExecutionKind::source});
    (void)registry.register_descriptor(NodeDescriptor{
        "builtin.gain", "1.0.0", "Gain",
        {{"audio_in", pcm, PortDirection::input, true}, {"audio_out", pcm, PortDirection::output, false}},
        {{"gain_db", ParameterKind::floating, 0.0, -60.0, 12.0, "dB"}}, NodeExecutionKind::block});
    (void)registry.register_descriptor(NodeDescriptor{
        "builtin.output", "1.0.0", "Output",
        {{"audio", pcm, PortDirection::input, true}}, {}, NodeExecutionKind::sink});
}

} // namespace audioalgo
