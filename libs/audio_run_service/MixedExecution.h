#pragma once
#include "audio_juce_backend/JuceGraph.h"

namespace audioalgo {
// Worker-only orchestration. Virtual ingress/capture nodes belong to a disposable
// JUCE projection, never to the caller's authoritative project.
// Optional borrowed partitions replay independently from frame zero in every
// native region. They remain valid through this synchronous worker call; owned
// offline tasks retain their declared maximum block size.
[[nodiscard]] AudioGraphOutcome render_prepared_audio_graph(const ProjectModel&,const NodeRegistry&,
    std::vector<AudioSourceBinding>,int block_size,NodeExecutionContext,
    NativeRenderStatistics*,std::string* task_manifest_json,std::span<const int> host_partitions={});
}
