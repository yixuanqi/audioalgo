#pragma once
#include <juce_core/juce_core.h>
#include <optional>
#include <string>

namespace audioalgo::editor {
std::optional<std::string> launch_external_editor(const juce::StringArray& arguments);
}
