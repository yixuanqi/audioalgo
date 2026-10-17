#include "audio_juce_backend/JuceGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace audioalgo {
namespace {

class GainAudioProcessor final : public juce::AudioProcessor {
public:
    GainAudioProcessor(double gain_db, int channels)
        : AudioProcessor(BusesProperties()
              .withInput("Input", juce::AudioChannelSet::discreteChannels(channels), true)
              .withOutput("Output", juce::AudioChannelSet::discreteChannels(channels), true)),
          gain_(static_cast<float>(std::pow(10.0, gain_db / 20.0))) {}

    const juce::String getName() const override { return "Gain"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        midi.clear();
        for (int channel = 0; channel < getTotalNumOutputChannels(); ++channel)
            buffer.applyGain(channel, 0, buffer.getNumSamples(), gain_);
    }
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

private:
    const float gain_;
};

const NodeInstance* find_node(const ProjectModel& project, const std::string& node_id) {
    const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const NodeInstance& node) {
        return node.id == node_id;
    });
    return found == project.nodes.end() ? nullptr : &*found;
}

const ParameterValue* find_parameter(const NodeInstance& node, const std::string& id) {
    const auto found = node.parameters.find(id);
    return found == node.parameters.end() ? nullptr : &found->second;
}

std::string first_port_id(const NodeDescriptor& descriptor, PortDirection direction) {
    const auto found = std::find_if(descriptor.ports.begin(), descriptor.ports.end(), [&](const PortDescriptor& port) {
        return port.direction == direction;
    });
    return found == descriptor.ports.end() ? std::string{} : found->id;
}

Diagnostic graph_error(std::string code, std::string message) {
    return {std::move(code), std::move(message)};
}

} // namespace

bool JuceProcessorRegistry::register_factory(std::string type_id, JuceProcessorFactory factory) {
    if (type_id.empty() || !factory || factories_.contains(type_id))
        return false;
    factories_.emplace(std::move(type_id), std::move(factory));
    return true;
}

bool JuceProcessorRegistry::contains(const std::string& type_id) const noexcept {
    return factories_.contains(type_id);
}

std::unique_ptr<juce::AudioProcessor> JuceProcessorRegistry::create(const NodeInstance& node, int channel_count) const {
    const auto factory = factories_.find(node.type_id);
    if (factory == factories_.end())
        return {};
    return factory->second(node, channel_count);
}

void register_builtin_juce_processors(JuceProcessorRegistry& registry) {
    (void)registry.register_factory("builtin.gain", [](const NodeInstance& node, int channels) -> std::unique_ptr<juce::AudioProcessor> {
        const auto* value = find_parameter(node, "gain_db");
        if (value == nullptr)
            return std::make_unique<GainAudioProcessor>(0.0, channels);
        const auto* gain_db = std::get_if<double>(value);
        if (gain_db == nullptr || !std::isfinite(*gain_db) || *gain_db < -60.0 || *gain_db > 12.0)
            return std::unique_ptr<juce::AudioProcessor>{};
        return std::make_unique<GainAudioProcessor>(*gain_db, channels);
    });
}

AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                    const NodeRegistry& descriptor_registry,
                                    const JuceProcessorRegistry& processor_registry,
                                    const AudioBuffer& source_audio,
                                    int block_size) {
    if (const auto error = source_audio.format_error(); !error.empty())
        return graph_error("invalid_audio_format", error);
    if (block_size <= 0)
        return graph_error("invalid_block_size", "JUCE graph block size must be positive");

    const auto validation = validate_project(project, descriptor_registry);
    if (!validation.empty())
        return graph_error(validation.front().code, validation.front().message);

    std::vector<const NodeInstance*> sources;
    std::vector<const NodeInstance*> sinks;
    for (const auto& node : project.nodes) {
        const auto* descriptor = descriptor_registry.find(node.type_id);
        if (descriptor == nullptr)
            return graph_error("node_type_missing", "Node descriptor disappeared during graph compilation: " + node.type_id);
        if (descriptor->execution_kind == NodeExecutionKind::source)
            sources.push_back(&node);
        else if (descriptor->execution_kind == NodeExecutionKind::sink)
            sinks.push_back(&node);
        else if (!processor_registry.contains(node.type_id))
            return graph_error("processor_factory_missing", "No JUCE processor factory is registered for " + node.type_id);
    }
    if (sources.size() != 1 || sinks.size() != 1)
        return graph_error("graph_shape_unsupported", "This render path requires exactly one audio source and one output sink");
    if (project.selected_output_node_id != sinks.front()->id)
        return graph_error("selected_output_mismatch", "The selected output must be the graph's single output sink");

    const int channel_count = static_cast<int>(source_audio.channel_count());
    const auto sample_rate = static_cast<double>(source_audio.sample_rate_hz());
    juce::AudioProcessorGraph graph;
    graph.setPlayConfigDetails(channel_count, channel_count, sample_rate, block_size);
    graph.setNonRealtime(true);

    std::map<std::string, juce::AudioProcessorGraph::NodeID, std::less<>> graph_nodes;
    for (const auto& node : project.nodes) {
        std::unique_ptr<juce::AudioProcessor> processor;
        const auto* descriptor = descriptor_registry.find(node.type_id);
        if (descriptor->execution_kind == NodeExecutionKind::source) {
            processor = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode);
        } else if (descriptor->execution_kind == NodeExecutionKind::sink) {
            processor = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode);
        } else {
            processor = processor_registry.create(node, channel_count);
            if (processor == nullptr)
                return graph_error("processor_configuration_invalid", "Could not create JUCE processor for node " + node.id);
        }

        const auto added = graph.addNode(std::move(processor), std::nullopt, juce::AudioProcessorGraph::UpdateKind::none);
        if (added == nullptr)
            return graph_error("juce_node_rejected", "JUCE rejected node " + node.id);
        graph_nodes.emplace(node.id, added->nodeID);
    }

    for (const auto& connection : project.connections) {
        const auto* from = find_node(project, connection.from_node);
        const auto* to = find_node(project, connection.to_node);
        const auto* from_descriptor = from == nullptr ? nullptr : descriptor_registry.find(from->type_id);
        const auto* to_descriptor = to == nullptr ? nullptr : descriptor_registry.find(to->type_id);
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
        for (int channel = 0; channel < channel_count; ++channel)
            std::copy_n(block.getReadPointer(channel), count, channels[static_cast<std::size_t>(channel)]);
        offset += count;
    }
    graph.releaseResources();
    return output;
}

} // namespace audioalgo
