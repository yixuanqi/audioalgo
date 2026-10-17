#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_core/AudioBuffer.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>
#include <memory>
#include <variant>

namespace audioalgo {

using JuceProcessorFactory = std::function<std::unique_ptr<juce::AudioProcessor>(const NodeInstance&, int channel_count)>;

class JuceProcessorRegistry {
public:
    [[nodiscard]] bool register_factory(std::string type_id, JuceProcessorFactory factory);
    [[nodiscard]] bool contains(const std::string& type_id) const noexcept;
    [[nodiscard]] std::unique_ptr<juce::AudioProcessor> create(const NodeInstance& node, int channel_count) const;

private:
    std::map<std::string, JuceProcessorFactory, std::less<>> factories_;
};

void register_builtin_juce_processors(JuceProcessorRegistry& registry);

using AudioGraphOutcome = std::variant<AudioBuffer, Diagnostic>;

[[nodiscard]] AudioGraphOutcome render_audio_graph(const ProjectModel& project,
                                                   const NodeRegistry& descriptor_registry,
                                                   const JuceProcessorRegistry& processor_registry,
                                                   const AudioBuffer& source_audio,
                                                   int block_size = 512);

} // namespace audioalgo
