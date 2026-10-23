#include "audio_runtime/NativePlan.h"

#include <algorithm>
#include <map>
#include <limits>
#include <set>
#include <utility>

namespace audioalgo {
namespace {
Diagnostic plan_error(std::string code, std::string message, std::string node_id = {}, std::string port_id = {}) {
    return {std::move(code), std::move(message), std::move(node_id), std::move(port_id)};
}
std::optional<std::size_t> channel_base(const NodeDescriptor& descriptor, PortDirection direction,
    const std::string& id, std::size_t channels) {
    std::size_t index = 0;
    for (const auto& port : descriptor.ports) {
        if (port.direction != direction)
            continue;
        if (port.id == id) {
            if (index != 0 && !(descriptor.execution_kind == NodeExecutionKind::block &&
                               descriptor.capabilities.supports_named_audio_ports))
                return std::nullopt;
            return index * channels;
        }
        ++index;
    }
    return std::nullopt;
}
}

PlanSelectionOutcome evaluate_plan_selection(const ProjectModel& project, const NodeCatalog& catalog) {
    // The render terminal is either the port-level selection (any node's output port) or the
    // legacy single sink. Everything outside the terminal's reverse dependency closure is pruned
    // before admission, so unselected branches create no processors and no resources.
    PlanSelection selection;
    if (!project.selected_output_port_id.empty()) {
        selection.terminal_node = project.selected_output_node_id;
        selection.selected_output = PlanPortAddress{selection.terminal_node, project.selected_output_port_id};
    } else {
        std::vector<std::string> sinks;
        for (const auto& node : project.nodes) {
            const auto* descriptor = catalog.find(node.type_id);
            if (descriptor == nullptr)
                return plan_error("node_type_missing", "Node descriptor disappeared during graph compilation: " + node.type_id, node.id);
            if (descriptor->execution_kind == NodeExecutionKind::sink)
                sinks.push_back(node.id);
        }
        if (sinks.size() != 1)
            return plan_error("graph_shape_unsupported", "This render path requires at least one audio source and exactly one output sink");
        if (project.selected_output_node_id != sinks.front())
            return plan_error("selected_output_mismatch", "The selected output must be the graph's single output sink");
        selection.terminal_node = sinks.front();
    }

    std::map<std::string, std::vector<std::string>, std::less<>> upstream;
    for (const auto& edge : project.connections)
        upstream[edge.to_node].push_back(edge.from_node);
    std::vector<std::string> pending{selection.terminal_node};
    selection.closure.insert(selection.terminal_node);
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto found = upstream.find(current);
        if (found == upstream.end())
            continue;
        for (const auto& from : found->second)
            if (selection.closure.insert(from).second)
                pending.push_back(from);
    }
    return selection;
}

std::optional<Diagnostic> validate_native_plan_node(const NodeDescriptor& descriptor,const std::string& node_id,
    AudioFormat format,std::size_t source_frames,int maximum_block_frames) {
    if (descriptor.capabilities.requires_offline_task)
        return plan_error("task_boundary_required", "Node requires an owned offline task and cannot enter a native callback region", node_id);
    const auto& constraint = descriptor.audio_format;
    if (!constraint.sample_rates_hz.empty() && std::find(constraint.sample_rates_hz.begin(),
            constraint.sample_rates_hz.end(), format.sample_rate_hz) == constraint.sample_rates_hz.end())
        return plan_error("capability_unsupported", "Node " + node_id + " does not support sample rate " +
                          std::to_string(format.sample_rate_hz) + " Hz", node_id);
    if (!constraint.channel_counts.empty() && std::find(constraint.channel_counts.begin(),
            constraint.channel_counts.end(), format.channel_count) == constraint.channel_counts.end())
        return plan_error("capability_unsupported", "Node " + node_id + " does not support " +
                          std::to_string(format.channel_count) + " channels", node_id);
    for (const auto& port : descriptor.ports)
        if (port.type_id != "audio.pcm@1")
            return plan_error("port_type_unsupported", "Node " + node_id + " port " + port.id + " is typed " +
                port.type_id + "; the native audio region only accepts audio.pcm@1", node_id, port.id);
    if (!descriptor.capabilities.latency_frames)
        return plan_error("latency_unknown", "Unknown algorithm latency cannot enter a synchronous native region", node_id);
    const auto buffering = !descriptor.capabilities.supports_variable_block_size &&
        descriptor.capabilities.fixed_frame_end_policy == NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad &&
        descriptor.capabilities.fixed_block_frames > 0 ? descriptor.capabilities.fixed_block_frames - 1 : 0;
    if (*descriptor.capabilities.latency_frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        buffering > static_cast<std::size_t>(std::numeric_limits<int>::max()) - *descriptor.capabilities.latency_frames)
        return plan_error("latency_unsupported", "Declared latency exceeds the native graph sample range", node_id);
    const auto& caps = descriptor.capabilities;
    if (!caps.tail_frames)
        return plan_error("tail_unknown", "Unknown algorithm tails cannot enter a bounded native region", node_id);
    if (!caps.output_tail_frames.empty()) {
        if (!caps.supports_named_audio_ports || caps.supports_variable_block_size ||
            caps.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad ||
            caps.fixed_block_frames == 0)
            return plan_error("tail_ports_profile_unsupported", "Independent tails require the named padded fixed-frame profile", node_id);
        std::set<std::string> outputs;
        for (const auto& port : descriptor.ports) if (port.direction == PortDirection::output) outputs.insert(port.id);
        std::size_t maximum = 0;
        for (const auto& [port, tail] : caps.output_tail_frames) {
            if (!outputs.erase(port))
                return plan_error("tail_ports_invalid", "Tail override does not name an output port", node_id, port);
            maximum = std::max(maximum, tail);
        }
        if (!outputs.empty())
            return plan_error("tail_ports_invalid", "Independent tail declarations must cover every output", node_id, *outputs.begin());
        if (maximum != *caps.tail_frames)
            return plan_error("tail_ports_invalid", "Node tail must equal the maximum declared output tail", node_id);
    }
    if (caps.uses_streaming_frame_api && caps.maximum_no_progress_calls == 0)
        return plan_error("stream_progress_budget_invalid", "A finite streaming node requires a positive no-progress budget", node_id);
    if (caps.tail_processing == NodeExecutionCapabilities::TailProcessing::requires_flush)
        return plan_error("flush_unsupported", "This native profile has no algorithm flush API; zero padding cannot be substituted", node_id);
    if (*caps.tail_frames != 0 && caps.tail_processing != NodeExecutionCapabilities::TailProcessing::zero_padding)
        return plan_error("tail_policy_unsupported", "A retained tail requires an explicit zero-padding algorithm contract", node_id);
    if (caps.tail_processing == NodeExecutionCapabilities::TailProcessing::zero_padding) {
        const auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
        if (caps.supports_variable_block_size || caps.fixed_block_frames == 0 ||
            caps.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad)
            return plan_error("tail_profile_unsupported", "Explicit zero-padding tails currently require a padded fixed frame", node_id);
        const auto recovery = buffering + *caps.latency_frames;
        if (*caps.tail_frames > limit - recovery || !caps.maximum_zero_padding_frames ||
            *caps.maximum_zero_padding_frames > limit || *caps.maximum_zero_padding_frames < recovery + *caps.tail_frames)
            return plan_error("tail_padding_budget_invalid", "Finite padding budget must cover algorithm delay, retained tail and at most F-1 padding samples", node_id);
    }
    if (!descriptor.capabilities.supports_variable_block_size) {
        const auto fixed = descriptor.capabilities.fixed_block_frames;
        const auto can_pad = descriptor.capabilities.fixed_frame_end_policy ==
            NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
        if (can_pad) {
            if (fixed == 0 || fixed > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return plan_error("capability_unsupported", "Fixed frame length is outside the supported native buffer range", node_id);
            return {};
        }
        if (fixed == 0 || static_cast<std::size_t>(maximum_block_frames) != fixed)
            return plan_error("capability_unsupported", "Node " + node_id + " requires " + std::to_string(fixed) +
                " frame blocks but the run requested " + std::to_string(maximum_block_frames), node_id);
        if (source_frames % fixed != 0)
            return plan_error("capability_unsupported", "Node " + node_id + " requires whole " + std::to_string(fixed) +
                " frame blocks but the source has " + std::to_string(source_frames) +
                " frames; fixed-frame tails are unsupported", node_id);
    }
    return {};
}

NativePlanOutcome compile_native_plan(const ProjectModel& project, const NodeCatalog& catalog,
                                      AudioFormat format, std::size_t source_frames, int maximum_block_frames) {
    if (format.sample_rate_hz != 16000 && format.sample_rate_hz != 48000)
        return plan_error("invalid_audio_format", "Only 16000 Hz and 48000 Hz audio is supported");
    if (format.channel_count != 1 && format.channel_count != 2 && format.channel_count != 4)
        return plan_error("invalid_audio_format", "Audio must have 1, 2, or 4 channels");
    if (maximum_block_frames <= 0)
        return plan_error("invalid_block_size", "JUCE graph block size must be positive");
    std::vector<GraphDiagnostic> diagnostics;
    const auto order = topological_order(project, catalog, diagnostics);
    if (!diagnostics.empty())
        return plan_error(diagnostics.front().code, diagnostics.front().message,
                          diagnostics.front().node_id, diagnostics.front().port_id);

    const auto selection_outcome = evaluate_plan_selection(project, catalog);
    if (const auto* error = std::get_if<Diagnostic>(&selection_outcome))
        return *error;
    const auto& selection = std::get<PlanSelection>(selection_outcome);
    const auto& terminal_node = selection.terminal_node;
    const auto& closure = selection.closure;

    // Admission reads metadata only, and only for nodes the render will actually execute. No
    // plugin factory is reachable through NodeCatalog.
    std::vector<std::string> sources;
    for (const auto& id : order) {
        if (!closure.contains(id))
            continue;
        const auto node = std::find_if(project.nodes.begin(), project.nodes.end(),
                                       [&](const auto& item) { return item.id == id; });
        const auto& descriptor = *catalog.find(node->type_id);
        if (const auto error=validate_native_plan_node(descriptor,id,format,source_frames,maximum_block_frames))
            return *error;
        if (descriptor.execution_kind==NodeExecutionKind::source)
            sources.push_back(id);
    }
    if (sources.empty())
        return plan_error("graph_shape_unsupported", "This render path requires at least one audio source and exactly one output sink");

    NativePlan plan;
    plan.format = format;
    plan.source_frames = source_frames;
    plan.maximum_block_frames = maximum_block_frames;
    plan.selected_output = selection.selected_output;
    std::map<std::string, const NativePlanNode*, std::less<>> by_id;
    std::vector<std::string> blocks;
    plan.nodes.reserve(closure.size());
    for (const auto& id : order) {
        if (!closure.contains(id))
            continue;
        const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& item) { return item.id == id; });
        const auto& descriptor = *catalog.find(node->type_id);
        auto parameters = node->parameters;
        for (const auto& parameter : descriptor.parameters)
            parameters.try_emplace(parameter.id, parameter.default_value);
        plan.nodes.push_back({id, descriptor, std::move(parameters)});
        auto& planned = plan.nodes.back();
        std::size_t incoming_tail = 0;
        for (const auto& edge : project.connections) if (edge.to_node == id && closure.contains(edge.from_node)) {
            const auto* upstream = by_id.at(edge.from_node);
            planned.input_latency_frames = std::max(planned.input_latency_frames, upstream->output_latency_frames);
            incoming_tail = std::max(incoming_tail, upstream->output_tails.at(edge.from_port));
        }
        const auto& caps = descriptor.capabilities;
        const auto own_latency = *caps.latency_frames +
            (!caps.supports_variable_block_size && caps.fixed_frame_end_policy == NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad ? caps.fixed_block_frames - 1 : 0);
        if (own_latency > static_cast<std::size_t>(std::numeric_limits<int>::max()) - planned.input_latency_frames)
            return plan_error("latency_path_unsupported", "Cumulative path latency exceeds the native graph sample range", id);
        planned.output_latency_frames = planned.input_latency_frames + own_latency;
        if (*caps.tail_frames > std::numeric_limits<std::size_t>::max() - incoming_tail)
            return plan_error("tail_length_overflow", "Cumulative tail exceeds the sample range", id);
        planned.output_tail_frames = incoming_tail + *caps.tail_frames;
        for (const auto& port : descriptor.ports) if (port.direction == PortDirection::output) {
            const auto tail = caps.output_tail_frames.empty() ? *caps.tail_frames : caps.output_tail_frames.at(port.id);
            planned.output_tails.emplace(port.id, incoming_tail + tail);
        }
        if (source_frames > std::numeric_limits<std::size_t>::max() - planned.output_tail_frames ||
            source_frames + planned.output_tail_frames > std::numeric_limits<std::size_t>::max() - planned.output_latency_frames)
            return plan_error("render_length_overflow", "Source, tail and latency exceed the sample range", id);
        for (const auto& port : descriptor.ports) if (port.direction == PortDirection::input) {
            std::size_t end = 0; // Unconnected optional input is already an ended silence stream.
            for (const auto& edge : project.connections) if (edge.to_node == id && edge.to_port == port.id && closure.contains(edge.from_node)) {
                end = source_frames + planned.input_latency_frames + by_id.at(edge.from_node)->output_tails.at(edge.from_port);
                break;
            }
            planned.input_ends.push_back({port.id, end});
        }
        if (id == terminal_node) {
            plan.latency_frames = planned.output_latency_frames;
            plan.retained_tail_frames = selection.selected_output ? planned.output_tails.at(selection.selected_output->port_id) : planned.output_tail_frames;
        }
        by_id.emplace(id, &plan.nodes.back());
        if (descriptor.execution_kind == NodeExecutionKind::block)
            blocks.push_back(id);
    }
    for (const auto& edge : project.connections) {
        if (!closure.contains(edge.from_node) || !closure.contains(edge.to_node))
            continue;
        const auto& from = by_id.at(edge.from_node)->descriptor;
        const auto& to = by_id.at(edge.to_node)->descriptor;
        const auto from_base = channel_base(from, PortDirection::output, edge.from_port, format.channel_count);
        const auto to_base = channel_base(to, PortDirection::input, edge.to_port, format.channel_count);
        if (!from_base || !to_base)
            return plan_error("port_mapping_unsupported", "The native plan requires named audio port capability for additional groups",
                              edge.to_node, edge.to_port);
        plan.routes.push_back({{edge.from_node, edge.from_port}, {edge.to_node, edge.to_port},
                               *from_base, *to_base, format.channel_count});
    }
    plan.tasks.push_back({PlanTaskKind::source_boundary, std::move(sources)});
    plan.tasks.push_back({PlanTaskKind::native_region, std::move(blocks)});
    plan.tasks.push_back({PlanTaskKind::sink_boundary, {terminal_node}});
    return plan;
}
} // namespace audioalgo
