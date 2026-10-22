#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"
#include "audio_project/ProjectModel.h"

#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace audioalgo {

enum class PlanTaskKind { source_boundary, native_region, sink_boundary };

struct NativePlanNode {
    std::string node_id;
    NodeDescriptor descriptor;
    NodeParameters parameters;
};

struct NativePlanTask {
    PlanTaskKind kind;
    std::vector<std::string> node_ids;
};

struct PlanPortAddress {
    std::string node_id;
    std::string port_id;
};

struct NativeAudioRoute {
    PlanPortAddress from, to;
    std::size_t from_channel_base = 0;
    std::size_t to_channel_base = 0;
    std::size_t channel_count = 0;
};

// Value-only execution projection. No factories, processors, GUI or filesystem objects.
struct NativePlan {
    AudioFormat format;
    std::size_t source_frames = 0;
    int maximum_block_frames = 0;
    std::vector<NativePlanNode> nodes;
    std::vector<NativePlanTask> tasks;
    std::vector<NativeAudioRoute> routes;
    // Port-level selection: the plan renders this output port of this node (which may be any
    // node kind, not just a sink). Empty for legacy node-level selection (the single sink).
    std::optional<PlanPortAddress> selected_output;
};

// The render terminal and its reverse dependency closure, resolved from the project's output
// selection without touching audio metadata. Nodes outside the closure are not rendered and
// must not be admitted, executed or materialized by any consumer of this selection.
struct PlanSelection {
    std::string terminal_node;
    std::optional<PlanPortAddress> selected_output;
    std::set<std::string> closure;
};

using PlanSelectionOutcome = std::variant<PlanSelection, Diagnostic>;

[[nodiscard]] PlanSelectionOutcome evaluate_plan_selection(const ProjectModel& project,
                                                           const NodeCatalog& catalog);

using NativePlanOutcome = std::variant<NativePlan, Diagnostic>;

[[nodiscard]] NativePlanOutcome compile_native_plan(const ProjectModel& project,
    const NodeCatalog& catalog, AudioFormat format, std::size_t source_frames,
    int maximum_block_frames = 512);

} // namespace audioalgo
