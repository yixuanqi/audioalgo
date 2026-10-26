#pragma once
#include "EditorText.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <filesystem>
#include <optional>
#include <algorithm>
namespace audioalgo::editor {
inline const auto background = juce::Colour(0xff101b23);
inline const auto panel = juce::Colour(0xff1f2d36);
inline const auto raised = juce::Colour(0xff263842);
inline const auto ink = juce::Colour(0xffe6ece7);
inline const auto muted = juce::Colour(0xff9aabb0);
inline const auto signal = juce::Colour(0xff9ad3c8);
inline const auto compatible_green = juce::Colour(0xff6ed39a);
inline const auto amber = juce::Colour(0xffe5b56f);
inline const auto danger = juce::Colour(0xffe28778);

inline juce::String native_user_log_notice(const NodeDescriptor& descriptor) {
    if(!descriptor.plugin_origin || descriptor.plugin_origin->runtime!="native")return {};
    const auto metadata=juce::JSON::parse(utf8_string(descriptor.backend_metadata_json));
    const auto capability=metadata["user_logs"].toString();
    if(capability=="sdk_v1")return "User logs: available.";
    if(capability=="unavailable")return "User logs: unavailable in this version.";
    return {};
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline juce::File to_juce_file(const std::filesystem::path& path) {
    const auto value = path_to_utf8(path);
    return juce::File(utf8_string(value));
}

inline std::filesystem::path path_from_juce_file(const juce::File& file) {
    return std::filesystem::u8path(to_utf8(file.getFullPathName()));
}

inline std::string new_id() {
    return juce::Uuid().toString().toStdString();
}

inline juce::var make_library_drag_description(const std::string& type_id) {
    auto* payload = new juce::DynamicObject();
    payload->setProperty("operation", "node-library-add");
    payload->setProperty("version", 1);
    payload->setProperty("type_id", utf8_string(type_id));
    return juce::var(payload);
}

inline std::optional<std::string> library_drag_type(const juce::var& description) {
    const auto* payload = description.getDynamicObject();
    if (payload == nullptr || payload->getProperty("operation").toString() != "node-library-add" ||
        static_cast<int>(payload->getProperty("version")) != 1)
        return std::nullopt;
    const auto type_id = to_utf8(payload->getProperty("type_id").toString());
    if (type_id.empty())
        return std::nullopt;
    return type_id;
}

inline const audioalgo::NodeInstance* find_node(const audioalgo::ProjectModel& project, const std::string& id) {
    const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) { return node.id == id; });
    return found == project.nodes.end() ? nullptr : &*found;
}

inline void style_button(juce::TextButton& button, juce::Colour accent = signal) {
        button.setColour(juce::TextButton::buttonColourId, raised);
        button.setColour(juce::TextButton::buttonOnColourId, accent.withAlpha(0.2f));
        button.setColour(juce::TextButton::textColourOffId, ink);
        button.setColour(juce::TextButton::textColourOnId, accent);
    }


} // namespace audioalgo::editor
