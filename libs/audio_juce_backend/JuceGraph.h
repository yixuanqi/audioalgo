#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_core/AudioBuffer.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <variant>

namespace audioalgo {

using AudioGraphOutcome = std::variant<AudioBuffer, Diagnostic>;

[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& node_registry,
                                                   const AudioBuffer& source_audio,
                                                   int block_size = 512);

} // namespace audioalgo
