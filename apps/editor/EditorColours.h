#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace audioalgo::editor {
// Neutral dark surfaces and blue focus accents, following the supplied VS Code reference.
inline const auto background = juce::Colour(0xff1f1f1f);
inline const auto panel = juce::Colour(0xff181818);
inline const auto raised = juce::Colour(0xff2b2b2b);
inline const auto border = juce::Colour(0xff333333);
inline const auto grid = juce::Colour(0xff292929);
inline const auto selection = juce::Colour(0xff264f78);
inline const auto ink = juce::Colour(0xffcccccc);
inline const auto muted = juce::Colour(0xff9d9d9d);
inline const auto signal = juce::Colour(0xff4daafc);
inline const auto primary = juce::Colour(0xff0078d4);
inline const auto compatible_green = juce::Colour(0xff89d185);
inline const auto amber = juce::Colour(0xffcca700);
inline const auto danger = juce::Colour(0xfff48771);
} // namespace audioalgo::editor
