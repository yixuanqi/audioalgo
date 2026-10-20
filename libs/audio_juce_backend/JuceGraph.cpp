#include "audio_juce_backend/JuceGraph.h"
#include "audio_juce_backend/NativeNodeAudioProcessor.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace audioalgo {
namespace {

const NodeInstance* find_node(const ProjectModel& project, const std::string& node_id) {
    const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const NodeInstance& node) {
        return node.id == node_id;
    });
    return found == project.nodes.end() ? nullptr : &*found;
}

std::string first_port_id(const NodeDescriptor& descriptor, PortDirection direction) {
    const auto found = std::find_if(descriptor.ports.begin(), descriptor.ports.end(), [&](const PortDescriptor& port) {
        return port.direction == direction;
    });
    return found == descriptor.ports.end() ? std::string{} : found->id;
}

Diagnostic graph_error(std::string code, std::string message, std::string node_id = {}, std::string port_id = {}) {
    return {std::move(code), std::move(message), std::move(node_id), std::move(port_id)};
}

} // namespace

AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                    const NodeRegistry& node_registry,
                                    const AudioBuffer& source_audio,
                                    int block_size) {
    if (const auto error = source_audio.format_error(); !error.empty())
        return graph_error("invalid_audio_format", error);
    if (block_size <= 0)
        return graph_error("invalid_block_size", "JUCE graph block size must be positive");

    const auto validation = validate_project(project, node_registry);
    if (!validation.empty())
        return graph_error(validation.front().code, validation.front().message,
                           validation.front().node_id, validation.front().port_id);

    std::vector<const NodeInstance*> sources;
    std::vector<const NodeInstance*> sinks;
    std::vector<NodeProcessorRequest> requests;
    for (const auto& node : project.nodes) {
        const auto* descriptor = node_registry.find(node.type_id);
        if (descriptor == nullptr)
            return graph_error("node_type_missing", "Node descriptor disappeared during graph compilation: " + node.type_id);
        if (descriptor->execution_kind == NodeExecutionKind::source)
            sources.push_back(&node);
        else if (descriptor->execution_kind == NodeExecutionKind::sink)
            sinks.push_back(&node);
        else
            requests.push_back({node.id, node.type_id, node.parameters});
    }
    if (sources.size() != 1 || sinks.size() != 1)
        return graph_error("graph_shape_unsupported", "This render path requires exactly one audio source and one output sink");
    if (project.selected_output_node_id != sinks.front()->id)
        return graph_error("selected_output_mismatch", "The selected output must be the graph's single output sink");

    auto plan_outcome = node_registry.create_plan(requests);
    if (const auto* error = std::get_if<NodeFactoryDiagnostic>(&plan_outcome))
        return graph_error(error->code, error->message, error->node_id);
    auto plan = std::move(std::get<NodeProcessorPlan>(plan_outcome));
    std::map<std::string, PlannedNodeProcessor, std::less<>> planned_processors;
    for (auto& processor : plan.processors)
        planned_processors.emplace(processor.node_id, std::move(processor));

    const int channel_count = static_cast<int>(source_audio.channel_count());
    const auto sample_rate = static_cast<double>(source_audio.sample_rate_hz());
    // Capability agreement is decided before execution: an unavailable format or frame contract must
    // fail loudly instead of being silently adapted.
    for (const auto& node : project.nodes) {
        const auto* descriptor = node_registry.find(node.type_id);
        if (descriptor == nullptr)
            continue;
        const auto& audio_format = descriptor->audio_format;
        if (!audio_format.sample_rates_hz.empty() &&
            std::find(audio_format.sample_rates_hz.begin(), audio_format.sample_rates_hz.end(),
                      source_audio.sample_rate_hz()) == audio_format.sample_rates_hz.end())
            return graph_error("capability_unsupported",
                               "Node " + node.id + " does not support sample rate " +
                                   std::to_string(source_audio.sample_rate_hz()) + " Hz", node.id);
        if (!audio_format.channel_counts.empty() &&
            std::find(audio_format.channel_counts.begin(), audio_format.channel_counts.end(),
                      source_audio.channel_count()) == audio_format.channel_counts.end())
            return graph_error("capability_unsupported",
                               "Node " + node.id + " does not support " +
                                   std::to_string(source_audio.channel_count()) + " channels", node.id);
        // The native region only carries PCM audio. A geometry/metric port must not be flattened into
        // JUCE channels; it needs a typed task, which R1 does not provide.
        for (const auto& port : descriptor->ports) {
            if (port.type_id != "audio.pcm@1")
                return graph_error("port_type_unsupported",
                                   "Node " + node.id + " port " + port.id + " is typed " + port.type_id +
                                       "; the native audio region only accepts audio.pcm@1", node.id, port.id);
        }
        if (!descriptor->capabilities.supports_variable_block_size) {
            const auto fixed = descriptor->capabilities.fixed_block_frames;
            if (fixed == 0 || static_cast<std::size_t>(block_size) != fixed)
                return graph_error("capability_unsupported",
                                   "Node " + node.id + " requires " + std::to_string(fixed) +
                                       " frame blocks but the run requested " + std::to_string(block_size), node.id);
            // There is no fixed-frame adapter or FIFO yet, so a short tail cannot be honoured.
            if (source_audio.frame_count() % fixed != 0)
                return graph_error("capability_unsupported",
                                   "Node " + node.id + " requires whole " + std::to_string(fixed) +
                                       " frame blocks but the source has " +
                                       std::to_string(source_audio.frame_count()) +
                                       " frames; fixed-frame tails are unsupported", node.id);
        }
    }

    juce::AudioProcessorGraph graph;
    graph.setPlayConfigDetails(channel_count, channel_count, sample_rate, block_size);
    graph.setNonRealtime(true);

    std::map<std::string, juce::AudioProcessorGraph::NodeID, std::less<>> graph_nodes;
    std::vector<std::pair<std::string, NativeNodeAudioProcessor*>> diagnostic_sources;
    for (const auto& node : project.nodes) {
        std::unique_ptr<juce::AudioProcessor> processor;
        const auto* descriptor = node_registry.find(node.type_id);
        if (descriptor->execution_kind == NodeExecutionKind::source) {
            processor = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode);
        } else if (descriptor->execution_kind == NodeExecutionKind::sink) {
            processor = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode);
        } else {
            auto planned = planned_processors.find(node.id);
            if (planned == planned_processors.end() || !planned->second.processor)
                return graph_error("node_plan_incomplete", "No prepared SDK processor exists for node " + node.id);
            auto wrapper = std::make_unique<NativeNodeAudioProcessor>(
                std::move(planned->second.processor), node.id, *descriptor,
                std::move(planned->second.parameters), NodeExecutionContext{}, channel_count);
            auto* wrapper_view = wrapper.get();
            processor = std::move(wrapper);
            diagnostic_sources.emplace_back(node.id, wrapper_view);
        }

        const auto added = graph.addNode(std::move(processor), std::nullopt, juce::AudioProcessorGraph::UpdateKind::none);
        if (added == nullptr)
            return graph_error("juce_node_rejected", "JUCE rejected node " + node.id);
        graph_nodes.emplace(node.id, added->nodeID);
    }

    for (const auto& connection : project.connections) {
        const auto* from = find_node(project, connection.from_node);
        const auto* to = find_node(project, connection.to_node);
        const auto* from_descriptor = from == nullptr ? nullptr : node_registry.find(from->type_id);
        const auto* to_descriptor = to == nullptr ? nullptr : node_registry.find(to->type_id);
        if (from == nullptr || to == nullptr || from_descriptor == nullptr || to_descriptor == nullptr)
            return graph_error("graph_endpoint_invalid", "Validated graph endpoint disappeared during JUCE compilation");
        const auto output_port = first_port_id(*from_descriptor, PortDirection::output);
        const auto input_port = first_port_id(*to_descriptor, PortDirection::input);
        if (output_port != connection.from_port || input_port != connection.to_port)
            return graph_error("port_mapping_unsupported", "The G1 JUCE adapter currently supports one audio input and output port per node");

        for (int channel = 0; channel < channel_count; ++channel) {
            const juce::AudioProcessorGraph::Connection edge{
                {graph_nodes.at(connection.from_node), channel},
                {graph_nodes.at(connection.to_node), channel}};
            if (!graph.addConnection(edge, juce::AudioProcessorGraph::UpdateKind::none))
                return graph_error("juce_connection_rejected", "JUCE rejected a typed connection at channel " + std::to_string(channel));
        }
    }

    graph.prepareToPlay(sample_rate, block_size);
    const auto find_processing_error = [&]() -> std::optional<Diagnostic> {
        for (const auto& [node_id, processor] : diagnostic_sources) {
            (void)node_id;
            if (processor->processing_diagnostic())
                return *processor->processing_diagnostic();
        }
        return std::nullopt;
    };
    if (const auto error = find_processing_error()) {
        return *error;
    }

    AudioBuffer output(source_audio.sample_rate_hz(), source_audio.channel_ids(), source_audio.frame_count(),
                       source_audio.valid_interval());
    juce::MidiBuffer midi;
    std::vector<float*> channels(static_cast<std::size_t>(channel_count));
    std::size_t offset = 0;
    while (offset < source_audio.frame_count()) {
        const auto count = std::min<std::size_t>(static_cast<std::size_t>(block_size), source_audio.frame_count() - offset);
        juce::AudioBuffer<float> block(channel_count, static_cast<int>(count));
        for (int channel = 0; channel < channel_count; ++channel) {
            block.copyFrom(channel, 0, source_audio.channel(static_cast<std::size_t>(channel)).data() + offset,
                           static_cast<int>(count));
            channels[static_cast<std::size_t>(channel)] = output.mutable_channel(static_cast<std::size_t>(channel)).data() + offset;
        }
        graph.processBlock(block, midi);
        if (const auto error = find_processing_error()) {
            return *error;
        }
        for (int channel = 0; channel < channel_count; ++channel)
            std::copy_n(block.getReadPointer(channel), count, channels[static_cast<std::size_t>(channel)]);
        offset += count;
    }
    graph.releaseResources();
    return output;
}

} // namespace audioalgo



