#pragma once

#include <juce_core/juce_core.h>

#include <string>

namespace audioalgo::editor {

// JUCE treats const char* literals as ASCII; text crossing the editor boundary is UTF-8.
[[nodiscard]] inline juce::String utf8_literal(const char* text) {
    return juce::String::fromUTF8(text);
}

[[nodiscard]] inline juce::String utf8_string(const std::string& text) {
    return juce::String::fromUTF8(text.c_str());
}

[[nodiscard]] inline std::string to_utf8(const juce::String& value) {
    const auto bytes = value.toUTF8();
    return std::string(bytes.getAddress(), static_cast<std::size_t>(bytes.sizeInBytes() - 1));
}

[[nodiscard]] inline std::string build_node_tooltip_text(
    const std::string& display_name, const std::string& summary, const std::string& hovered_port = {}) {
    std::string text = display_name;
    if (!summary.empty())
        text += "\n" + summary;
    if (!hovered_port.empty())
        text += "\n" + hovered_port;
    return text;
}

} // namespace audioalgo::editor
