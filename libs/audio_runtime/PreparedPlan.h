#pragma once
#include "audio_runtime/NativePlan.h"

namespace audioalgo {
enum class PreparedTaskKind { source, native_region, offline, sink };
struct PreparedTask {
    PreparedTaskKind kind;
    std::vector<NativePlanNode> nodes;
    // Indexes into PreparedPlan::tasks, always preceding this task.
    std::vector<std::size_t> dependencies;
};
// Pure value projection. No filesystem, factories, process handles or sample views.
struct PreparedPlan {
    AudioFormat format;
    std::size_t source_frames=0;
    int maximum_block_frames=512;
    std::vector<PreparedTask> tasks;
    std::vector<NativeAudioRoute> routes;
    std::string terminal_node;
    std::optional<PlanPortAddress> selected_output;
};
using PreparedPlanOutcome=std::variant<PreparedPlan,Diagnostic>;
[[nodiscard]] PreparedPlanOutcome compile_prepared_plan(const ProjectModel&,const NodeCatalog&,
    AudioFormat,std::size_t source_frames,int maximum_block_frames=512);
}
