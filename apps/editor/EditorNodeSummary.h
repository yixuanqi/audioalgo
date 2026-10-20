#pragma once

#include "EditorText.h"
#include "audio_project/ProjectModel.h"

#include <array>
#include <charconv>
#include <string>
#include <type_traits>

namespace audioalgo::editor {

inline std::string summary_value(const ParameterDescriptor& descriptor, const ParameterValue& value) {
    if (const auto invalid = validate_parameter_value(descriptor, value))
        return invalid->code;
    if (descriptor.kind == ParameterKind::enumeration)
        for (const auto& choice : descriptor.choices)
            if (choice.value == std::get<std::string>(value))
                return choice.label;
    return std::visit([](const auto& scalar) -> std::string {
        using T = std::decay_t<decltype(scalar)>;
        if constexpr (std::is_same_v<T, bool>) {
            return scalar ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return scalar.empty() ? "(not set)" : scalar;
        } else {
            std::array<char, 64> text{};
            const auto result = std::to_chars(text.data(), text.data() + text.size(), scalar);
            return result.ec == std::errc{} ? std::string(text.data(), result.ptr) : "Invalid number";
        }
    }, value);
}

// Painting may fit this text into one line; tooltip projection keeps this complete value.
inline juce::String node_summary(const NodeInstance& node, const NodeDescriptor* descriptor) {
    if (descriptor == nullptr)
        return utf8_string("Missing node: " + node.type_id);
    std::string text;
    for (const auto& parameter : descriptor->parameters) {
        if (!text.empty())
            text += "; ";
        const auto existing = node.parameters.find(parameter.id);
        const auto& value = existing == node.parameters.end() ? parameter.default_value : existing->second;
        text += (parameter.display_name.empty() ? parameter.id : parameter.display_name) + ": " +
                summary_value(parameter, value);
        if (!parameter.unit.empty())
            text += " " + parameter.unit;
    }
    return utf8_string(text.empty() ? descriptor->display_name : text);
}

} // namespace audioalgo::editor
