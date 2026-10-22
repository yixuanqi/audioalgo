#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_core/AudioBuffer.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <variant>
#include <vector>

namespace audioalgo {

using AudioGraphOutcome = std::variant<AudioBuffer, Diagnostic>;

// One independently decoded, immutable audio version bound to one source node. Bindings are
// prepared before any processor runs; the backend never reads audio files itself.
struct AudioSourceBinding {
    std::string node_id;
    AudioBuffer audio;
};

// Explicit multi-source entry: every source node of the project must have exactly one binding.
[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& node_registry,
                                                   std::vector<AudioSourceBinding> sources,
                                                   int block_size = 512);

// Legacy single-buffer entry. Compatible with single-source projects only; a project with
// several audio sources is rejected with an explicit ambiguity diagnostic instead of guessing
// or duplicating the buffer.
[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& node_registry,
                                                   const AudioBuffer& source_audio,
                                                   int block_size = 512);

} // namespace audioalgo
