#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_core/AudioBuffer.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <functional>
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

struct NativeRenderStatistics {
    std::size_t raw_frames = 0, aligned_frames = 0, alignment_offset_frames = 0;
    std::optional<AudioBuffer> raw_audio;
    std::size_t source_frames = 0, retained_tail_frames = 0, zero_padding_frames = 0;
    // Extra bounded graph processing after the selected raw prefix, for EOS on
    // longer unselected output ports. Never added to the selected valid interval.
    std::size_t drain_padding_frames = 0;
    std::uint64_t raw_duration_ns = 0, aligned_duration_ns = 0, alignment_offset_ns = 0;
};
struct NativeRenderOptions {
    // Optional repeating host partitions; every value must be in [1, block_size].
    // Borrowed only for this synchronous render; useful for deterministic host replay.
    std::span<const int> host_partitions;
    NativeRenderStatistics* statistics = nullptr;
    bool capture_raw_audio = false;
    std::function<bool()> is_cancelled;
    NodeExecutionContext execution_context;
};

// Explicit multi-source entry: every source node of the project must have exactly one binding.
[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& node_registry,
                                                   std::vector<AudioSourceBinding> sources,
                                                   int block_size = 512,
                                                   NativeRenderOptions options = {});

// Legacy single-buffer entry. Compatible with single-source projects only; a project with
// several audio sources is rejected with an explicit ambiguity diagnostic instead of guessing
// or duplicating the buffer.
[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& node_registry,
                                                   const AudioBuffer& source_audio,
                                                   int block_size = 512,
                                                   NativeRenderOptions options = {});

} // namespace audioalgo
