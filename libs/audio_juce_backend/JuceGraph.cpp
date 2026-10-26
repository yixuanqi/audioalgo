#include "audio_juce_backend/JuceGraph.h"
#include "audio_juce_backend/NativeNodeAudioProcessor.h"
#include "audio_runtime/NativePlan.h"
#include "audio_core/AudioTimeline.h"

#include <algorithm>
#include <map>
#include <limits>
#include <utility>
#include <vector>

namespace audioalgo {
namespace {

Diagnostic graph_error(std::string code, std::string message, std::string node_id = {}, std::string port_id = {}) {
    return {std::move(code), std::move(message), std::move(node_id), std::move(port_id)};
}

// Feeds one already decoded, immutable source version into the graph. The adapter performs no
// file IO and allocates nothing while processing: it only replays staged sample pointers.
class SourceBufferAdapter final : public juce::AudioProcessor {
public:
    SourceBufferAdapter(const AudioBuffer& source, int channel_count)
        : AudioProcessor(juce::AudioProcessor::BusesProperties()
                             .withOutput("audio", juce::AudioChannelSet::discreteChannels(std::max(1, channel_count)), true)),
          frames_(source.frame_count()) {
        channel_data_.reserve(source.channel_count());
        for (std::size_t channel = 0; channel < source.channel_count(); ++channel)
            channel_data_.push_back(source.channel(channel).data());
    }

    const juce::String getName() const override { return "AudioAlgo Source Adapter"; }
    void prepareToPlay(double, int) override { cursor_ = 0; }
    void releaseResources() override {}
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
    using AudioProcessor::processBlock;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        auto output = getBusBuffer(buffer, false, 0);
        const auto frames = static_cast<std::size_t>(output.getNumSamples());
        const auto count = std::min(frames, frames_ - cursor_);
        const auto channels = std::min(channel_data_.size(), static_cast<std::size_t>(output.getNumChannels()));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            if (count > 0)
                output.copyFrom(static_cast<int>(channel), 0, channel_data_[channel] + cursor_, static_cast<int>(count));
            if (count < frames)
                output.clear(static_cast<int>(channel), static_cast<int>(count), static_cast<int>(frames - count));
        }
        cursor_ += count;
    }

private:
    std::vector<const float*> channel_data_;
    std::size_t frames_ = 0;
    std::size_t cursor_ = 0;
};

std::vector<std::string> source_node_ids(const ProjectModel& project, const NodeCatalog& catalog) {
    std::vector<std::string> ids;
    for (const auto& node : project.nodes) {
        const auto* descriptor = catalog.find(node.type_id);
        if (descriptor != nullptr && descriptor->execution_kind == NodeExecutionKind::source)
            ids.push_back(node.id);
    }
    return ids;
}

} // namespace

AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                    const NodeRegistry& node_registry,
                                    std::vector<AudioSourceBinding> sources,
                                    int block_size, NativeRenderOptions options) {
    if (options.statistics) *options.statistics = {};
    const auto cancelled = [&] { return options.is_cancelled && options.is_cancelled(); };
    if (cancelled()) return graph_error("run_cancelled", "Native rendering was cancelled before preparation");
    for (auto partition : options.host_partitions)
        if (partition <= 0 || partition > block_size)
            return graph_error("invalid_host_partition", "Host partitions must be positive and fit the declared maximum block");
    if (sources.empty())
        return graph_error("source_binding_missing", "The native render path needs at least one bound audio source");
    const auto& reference = sources.front();
    if (const auto error = reference.audio.format_error(); !error.empty())
        return graph_error("invalid_audio_format", error);
    // Every source must be an independently decoded version of the same stream shape; the
    // backend has no resampling, padding or alignment, so any disagreement is rejected here,
    // before the plan compiles and before any block factory or prepare can run.
    for (std::size_t index = 1; index < sources.size(); ++index) {
        const auto& candidate = sources[index];
        if (const auto error = candidate.audio.format_error(); !error.empty())
            return graph_error("invalid_audio_format", error, candidate.node_id, "audio");
        if (candidate.audio.sample_rate_hz() != reference.audio.sample_rate_hz())
            return graph_error("source_format_mismatch",
                               "Source " + candidate.node_id + " runs at " +
                                   std::to_string(candidate.audio.sample_rate_hz()) + " Hz while source " +
                                   reference.node_id + " runs at " + std::to_string(reference.audio.sample_rate_hz()) + " Hz",
                               candidate.node_id, "audio");
        if (candidate.audio.channel_ids() != reference.audio.channel_ids())
            return graph_error("source_channel_mismatch",
                               "Source " + candidate.node_id + " disagrees with source " + reference.node_id +
                                   " on channel identity or layout",
                               candidate.node_id, "audio");
        if (candidate.audio.frame_count() != reference.audio.frame_count())
            return graph_error("source_frame_count_mismatch",
                               "Source " + candidate.node_id + " has " + std::to_string(candidate.audio.frame_count()) +
                                   " frames while source " + reference.node_id + " has " +
                                   std::to_string(reference.audio.frame_count()),
                               candidate.node_id, "audio");
        if (candidate.audio.valid_interval() != reference.audio.valid_interval())
            return graph_error("source_interval_mismatch",
                               "Source " + candidate.node_id + " disagrees with source " + reference.node_id +
                                   " on the valid interval",
                               candidate.node_id, "audio");
    }
    auto compiled = compile_native_plan(project, node_registry, reference.audio.format(),
                                        reference.audio.frame_count(), block_size);
    if (const auto* error = std::get_if<Diagnostic>(&compiled))
        return *error;
    auto execution_plan = std::move(std::get<NativePlan>(compiled));
    block_size = execution_plan.maximum_block_frames;
    const int channel_count = static_cast<int>(execution_plan.format.channel_count);
    const auto sample_rate = static_cast<double>(execution_plan.format.sample_rate_hz);
    std::map<std::string, const NativePlanNode*, std::less<>> plan_nodes;
    for (const auto& node : execution_plan.nodes)
        plan_nodes.emplace(node.node_id, &node);
    std::map<std::string, const AudioSourceBinding*, std::less<>> bindings;
    for (const auto& binding : sources)
        bindings.emplace(binding.node_id, &binding);
    std::vector<NodeProcessorRequest> requests;
    for (const auto& task : execution_plan.tasks)
        if (task.kind == PlanTaskKind::native_region)
            for (const auto& node_id : task.node_ids) {
                const auto& node = *plan_nodes.at(node_id);
                requests.push_back({node_id, node.descriptor.type_id, node.parameters});
            }
    auto instances = node_registry.create_plan(requests);
    if (const auto* error = std::get_if<NodeFactoryDiagnostic>(&instances))
        return graph_error(error->code, error->message, error->node_id);
    std::map<std::string, PlannedNodeProcessor, std::less<>> planned_processors;
    for (auto& processor : std::get<NodeProcessorPlan>(instances).processors)
        planned_processors.emplace(processor.node_id, std::move(processor));

    // A mixed Run calls this renderer separately for each native-region task.
    // Keep its budget local to that task, including final reset/drain, rather
    // than granting each node an independent complete-log allowance.
    const auto native_task_log_bytes=std::make_shared<std::size_t>(0);
    juce::AudioProcessorGraph graph;
    graph.setPlayConfigDetails(channel_count, channel_count, sample_rate, block_size);
    graph.setNonRealtime(true);

    std::map<std::string, juce::AudioProcessorGraph::NodeID, std::less<>> graph_nodes;
    std::vector<std::pair<std::string, NativeNodeAudioProcessor*>> diagnostic_sources;
    struct RenderCleanup {
        juce::AudioProcessorGraph& graph;
        std::vector<std::pair<std::string,NativeNodeAudioProcessor*>>& processors;
        bool finished=false;
        std::optional<Diagnostic> finish() noexcept {
            if(finished)return {};
            finished=true;
            std::optional<Diagnostic> failure;
            try {
                graph.releaseResources();
                // As with preparation, JUCE schedules worker-triggered release
                // on its message thread. Fence it before draining reset logs
                // and detaching their borrowed sinks.
                if(auto* messages=juce::MessageManager::getInstanceWithoutCreating();messages&&!messages->isThisTheMessageThread()) {
                    const auto released=messages->callFunctionOnMessageThread([](void* pointer)->void*{static_cast<juce::AudioProcessorGraph*>(pointer)->rebuild();return pointer;},&graph);
                    if(released!=&graph)failure=graph_error("graph_release_dispatch_failed","Could not finish native graph release on its message thread");
                }
            }catch(const std::exception& e){failure=graph_error("node_release_exception",e.what());}catch(...){failure=graph_error("node_release_exception","Unknown native release exception");}
            for(const auto& [id,processor]:processors){(void)id;auto error=processor->drain_logs(true);if(!failure&&error)failure=std::move(error);}
            return failure;
        }
        ~RenderCleanup(){(void)finish();}
    } cleanup{graph,diagnostic_sources};
    for (const auto& node : execution_plan.nodes) {
        std::unique_ptr<juce::AudioProcessor> processor;
        NativeNodeAudioProcessor* wrapper_view=nullptr;
        const auto* descriptor = &node.descriptor;
        if (descriptor->execution_kind == NodeExecutionKind::source) {
            const auto bound = bindings.find(node.node_id);
            if (bound == bindings.end() || bound->second == nullptr)
                return graph_error("source_binding_missing", "No decoded audio version is bound to source " + node.node_id,
                                   node.node_id, "audio");
            processor = std::make_unique<SourceBufferAdapter>(bound->second->audio, channel_count);
        } else if (descriptor->execution_kind == NodeExecutionKind::sink) {
            processor = std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode);
        } else {
            auto planned = planned_processors.find(node.node_id);
            if (planned == planned_processors.end() || !planned->second.processor)
                return graph_error("node_plan_incomplete", "No prepared SDK processor exists for node " + node.node_id);
            auto node_context=options.execution_context;node_context.node_id=node.node_id;node_context.maximum_block_frames=block_size;
            node_context.native_task_log_bytes=native_task_log_bytes;
            const auto* message_thread=juce::MessageManager::getInstanceWithoutCreating();
            node_context.worker_log_io_allowed=!message_thread || !message_thread->isThisTheMessageThread();
            if(auto error=planned->second.processor->freeze_task(node_context)) {
                (void)planned->second.processor->drain_logs(node_context,true);
                if(error->node_id.empty())error->node_id=node.node_id;
                return *error;
            }
            auto wrapper = std::make_unique<NativeNodeAudioProcessor>(
                std::move(planned->second.processor), node.node_id, *descriptor,
                std::move(planned->second.parameters), std::move(node_context), channel_count);
            wrapper_view = wrapper.get();
            wrapper_view->set_finite_output_frames(execution_plan.source_frames + node.output_tail_frames + node.output_latency_frames);
            std::vector<std::size_t> input_ends;
            for (const auto& end : node.input_ends) input_ends.push_back(end.frame);
            wrapper_view->set_finite_input_ends(std::move(input_ends));
            std::vector<std::size_t> output_ends;
            for (const auto& port : descriptor->ports) if (port.direction == PortDirection::output)
                output_ends.push_back(execution_plan.source_frames + node.output_latency_frames + node.output_tails.at(port.id));
            wrapper_view->set_finite_output_ends(std::move(output_ends));
            processor = std::move(wrapper);
        }

        const auto added = graph.addNode(std::move(processor), std::nullopt, juce::AudioProcessorGraph::UpdateKind::none);
        if (added == nullptr)
            return graph_error("juce_node_rejected", "JUCE rejected node " + node.node_id);
        if(wrapper_view)diagnostic_sources.emplace_back(node.node_id,wrapper_view);
        graph_nodes.emplace(node.node_id, added->nodeID);
    }

    for (const auto& route : execution_plan.routes) {
        for (std::size_t channel = 0; channel < route.channel_count; ++channel) {
            const juce::AudioProcessorGraph::Connection edge{
                {graph_nodes.at(route.from.node_id), static_cast<int>(route.from_channel_base + channel)},
                {graph_nodes.at(route.to.node_id), static_cast<int>(route.to_channel_base + channel)}};
            if (!graph.addConnection(edge, juce::AudioProcessorGraph::UpdateKind::none))
                return graph_error("juce_connection_rejected", "JUCE rejected a typed connection at channel " + std::to_string(channel),
                                   route.to.node_id, route.to.port_id);
        }
    }

    // Port-level selection has no sink node in the closure: route the selected output port of
    // the selected node through an implicit IO output node so the signal reaches the render.
    if (execution_plan.selected_output.has_value()) {
        const auto& selection = *execution_plan.selected_output;
        const auto selected = graph_nodes.find(selection.node_id);
        const NativePlanNode* selected_node = nullptr;
        for (const auto& node : execution_plan.nodes)
            if (node.node_id == selection.node_id)
                selected_node = &node;
        if (selected == graph_nodes.end() || selected_node == nullptr)
            return graph_error("selected_output_missing", "The selected output node is not part of the compiled plan",
                               selection.node_id, selection.port_id);
        std::size_t base = 0;
        bool port_found = false;
        for (const auto& port : selected_node->descriptor.ports) {
            if (port.direction != PortDirection::output)
                continue;
            if (port.id == selection.port_id) {
                port_found = true;
                break;
            }
            base += channel_count;
        }
        if (!port_found)
            return graph_error("selected_output_port_missing", "The selected output port is not part of the compiled plan",
                               selection.node_id, selection.port_id);
        const auto io = graph.addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                                          juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode),
                                      std::nullopt, juce::AudioProcessorGraph::UpdateKind::none);
        if (io == nullptr)
            return graph_error("juce_node_rejected", "JUCE rejected the implicit output node for the port selection",
                               selection.node_id, selection.port_id);
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const juce::AudioProcessorGraph::Connection edge{
                {selected->second, static_cast<int>(base + channel)},
                {io->nodeID, static_cast<int>(channel)}};
            if (!graph.addConnection(edge, juce::AudioProcessorGraph::UpdateKind::none))
                return graph_error("juce_connection_rejected",
                                   "JUCE rejected the selected output port connection at channel " + std::to_string(channel),
                                   selection.node_id, selection.port_id);
        }
    }

    graph.prepareToPlay(sample_rate, block_size);
    // JUCE's sync rebuild is asynchronous when requested from a worker. Fence the
    // real message-thread rebuild before inspecting prepared diagnostics/latency;
    // otherwise a delayed graph can be mistaken for a zero-latency graph.
    auto* messages = juce::MessageManager::getInstance();
    if (!messages->isThisTheMessageThread()) {
        const auto prepared = messages->callFunctionOnMessageThread([](void* pointer) -> void* {
            static_cast<juce::AudioProcessorGraph*>(pointer)->rebuild();
            return pointer;
        }, &graph);
        if (prepared != &graph)
            return graph_error("graph_prepare_dispatch_failed", "Could not finish native graph preparation on its message thread");
    }
    const auto find_processing_error = [&]() -> std::optional<Diagnostic> {
        for (const auto& [node_id, processor] : diagnostic_sources) {
            (void)node_id;
            if (processor->processing_diagnostic())
                return *processor->processing_diagnostic();
        }
        return std::nullopt;
    };
    const auto drain_pending_logs=[&]() -> std::optional<Diagnostic> {
        for(const auto& [id,processor]:diagnostic_sources){(void)id;if(auto error=processor->drain_logs(false))return error;}
        return {};
    };
    if(const auto error=drain_pending_logs())return *error;
    if (const auto error = find_processing_error()) {
        return *error;
    }

    const auto aligned_frames = execution_plan.source_frames + execution_plan.retained_tail_frames;
    auto valid = reference.audio.valid_interval();
    if (valid.frame_count != 0 && valid.first_frame + valid.frame_count == execution_plan.source_frames)
        valid.frame_count += execution_plan.retained_tail_frames;
    AudioBuffer output(reference.audio.sample_rate_hz(), reference.audio.channel_ids(), aligned_frames, valid);
    juce::MidiBuffer midi;
    const auto latency = static_cast<std::size_t>(std::max(0, graph.getLatencySamples()));
    if (latency != execution_plan.latency_frames)
        return graph_error("latency_projection_mismatch", "JUCE graph latency disagrees with the compiled native timeline");
    if (aligned_frames > std::numeric_limits<std::size_t>::max() - latency)
        return graph_error("render_length_overflow", "Source plus buffering latency exceeds the sample range");
    const auto render_frames = aligned_frames + latency;
    auto processing_frames = render_frames;
    for (const auto& node : execution_plan.nodes) if (node.descriptor.capabilities.uses_streaming_frame_api)
        processing_frames = std::max(processing_frames, execution_plan.source_frames + node.output_latency_frames + node.output_tail_frames);
    if (options.capture_raw_audio && options.statistics && latency != 0)
        options.statistics->raw_audio.emplace(reference.audio.sample_rate_hz(), reference.audio.channel_ids(), render_frames,
            AudioInterval{latency + valid.first_frame, valid.frame_count});
    std::size_t offset = 0;
    std::size_t partition_index = 0;
    SampleFrameClock raw_clock(reference.audio.sample_rate_hz(), 1000000000);
    while (offset < processing_frames) {
        if (cancelled()) return graph_error("run_cancelled", "Native rendering was cancelled at a block boundary");
        const auto host_count = options.host_partitions.empty() ? block_size :
            options.host_partitions[partition_index++ % options.host_partitions.size()];
        const auto count = std::min<std::size_t>(static_cast<std::size_t>(host_count), processing_frames - offset);
        juce::AudioBuffer<float> block(channel_count, static_cast<int>(count));
        block.clear();
        graph.processBlock(block, midi);
        if(const auto error=drain_pending_logs())return *error;
        if (cancelled()) return graph_error("run_cancelled", "Native rendering was cancelled after its processing block");
        if (const auto error = find_processing_error()) {
            return *error;
        }
        if (options.statistics && options.statistics->raw_audio && offset < render_frames)
            for (int channel = 0; channel < channel_count; ++channel)
                std::copy_n(block.getReadPointer(channel), std::min(count, render_frames - offset),
                    options.statistics->raw_audio->mutable_channel(static_cast<std::size_t>(channel)).data() + offset);
        // Compensated graph output is cropped exactly once. Drain with zero source
        // samples to include the last valid input even for a partial algorithm frame.
        const auto first = std::max(offset, latency);
        const auto end = std::min(offset + count, latency + aligned_frames);
        if (first < end) for (int channel = 0; channel < channel_count; ++channel)
            std::copy_n(block.getReadPointer(channel) + (first - offset), end - first,
                        output.mutable_channel(static_cast<std::size_t>(channel)).data() + (first - latency));
        offset += count;
        if (!raw_clock.advance(count))
            return graph_error("time_mapping_overflow", "Raw sample timeline cannot be represented in nanoseconds");
    }
    if (cancelled()) return graph_error("run_cancelled", "Native rendering was cancelled before terminal stream completion");
    for (const auto& [node_id, processor] : diagnostic_sources) {
        (void)node_id;processor->complete_finite_stream();
    }
    if (const auto error = find_processing_error()) return *error;
    if (cancelled()) return graph_error("run_cancelled", "Native rendering was cancelled after terminal stream completion");
    if(const auto error=cleanup.finish())return *error;
    if (options.statistics) {
        const auto aligned_time = map_sample_frame(aligned_frames, reference.audio.sample_rate_hz(), 1000000000);
        const auto offset_time = map_sample_frame(latency, reference.audio.sample_rate_hz(), 1000000000);
        if (!aligned_time || !offset_time)
            return graph_error("time_mapping_overflow", "Aligned sample timeline cannot be represented in nanoseconds");
        options.statistics->raw_frames = render_frames;
        options.statistics->aligned_frames = aligned_frames;
        options.statistics->alignment_offset_frames = latency;
        options.statistics->source_frames = execution_plan.source_frames;
        options.statistics->retained_tail_frames = execution_plan.retained_tail_frames;
        options.statistics->zero_padding_frames = render_frames - execution_plan.source_frames;
        options.statistics->raw_duration_ns = *map_sample_frame(render_frames, reference.audio.sample_rate_hz(), 1000000000);
        options.statistics->drain_padding_frames = processing_frames - render_frames;
        options.statistics->aligned_duration_ns = *aligned_time;
        options.statistics->alignment_offset_ns = *offset_time;
    }
    return output;
}

AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                    const NodeRegistry& node_registry,
                                    const AudioBuffer& source_audio,
                                    int block_size, NativeRenderOptions options) {
    if (const auto error = source_audio.format_error(); !error.empty())
        return graph_error("invalid_audio_format", error);
    const auto sources = source_node_ids(project, node_registry);
    if (sources.size() > 1)
        return graph_error("source_binding_ambiguous",
                           "The single-buffer render entry cannot bind a project with " + std::to_string(sources.size()) +
                               " audio source nodes; pass one AudioSourceBinding per source instead of duplicating a buffer");
    if (sources.empty())
        return graph_error("graph_shape_unsupported", "This render path requires at least one audio source and one output sink");
    return render_audio_graph(project, node_registry,
                              std::vector<AudioSourceBinding>{{sources.front(), source_audio}}, block_size, options);
}

} // namespace audioalgo
