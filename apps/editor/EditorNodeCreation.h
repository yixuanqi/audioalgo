#pragma once

#include "audio_editor_application/EditorNodeCommands.h"
#include "audio_editor_application/EditorGraphCommands.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace audioalgo::editor {

struct NodeLibraryEntry {
    std::string type_id;
    std::string display_name;
    NodeExecutionKind execution_kind = NodeExecutionKind::block;
    NodeBrowserClassification browser_classification{"uncategorized", std::nullopt};
    NodeParameters default_parameters;
    std::vector<std::string> browser_tags;
    std::string browser_description;
    std::string source_id = "builtin";
    std::string language_id = "cpp";
};

namespace node_library_search_detail {

struct CodePoint {
    std::uint32_t value = 0;
    std::size_t byte_count = 1;
};

[[nodiscard]] inline CodePoint decode_code_point(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80)
        return {first, 1};
    const auto continuation = [&](std::size_t index) {
        return index < text.size() &&
               (static_cast<unsigned char>(text[index]) & 0xc0u) == 0x80u;
    };
    if ((first & 0xe0u) == 0xc0u && continuation(offset + 1))
        return {(static_cast<std::uint32_t>(first & 0x1fu) << 6) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3fu), 2};
    if ((first & 0xf0u) == 0xe0u && continuation(offset + 1) && continuation(offset + 2))
        return {(static_cast<std::uint32_t>(first & 0x0fu) << 12) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3fu) << 6) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 2]) & 0x3fu), 3};
    if ((first & 0xf8u) == 0xf0u && continuation(offset + 1) &&
        continuation(offset + 2) && continuation(offset + 3))
        return {(static_cast<std::uint32_t>(first & 0x07u) << 18) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3fu) << 12) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 2]) & 0x3fu) << 6) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 3]) & 0x3fu), 4};
    return {first, 1};
}

[[nodiscard]] inline bool is_whitespace(std::uint32_t code_point) noexcept {
    return (code_point >= 0x09u && code_point <= 0x0du) || code_point == 0x20u ||
           code_point == 0x85u || code_point == 0xa0u || code_point == 0x1680u ||
           (code_point >= 0x2000u && code_point <= 0x200au) ||
           code_point == 0x2028u || code_point == 0x2029u || code_point == 0x202fu ||
           code_point == 0x205fu || code_point == 0x3000u;
}

} // namespace node_library_search_detail

[[nodiscard]] inline std::string node_library_search_fold_ascii(std::string_view value) {
    std::string folded(value);
    for (auto& character : folded)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return folded;
}

[[nodiscard]] inline std::vector<std::string> node_library_search_tokens(std::string_view query) {
    std::vector<std::string> tokens;
    std::size_t cursor = 0;
    while (cursor < query.size()) {
        while (cursor < query.size()) {
            const auto character = node_library_search_detail::decode_code_point(query, cursor);
            if (!node_library_search_detail::is_whitespace(character.value))
                break;
            cursor += character.byte_count;
        }
        if (cursor == query.size())
            break;
        const auto token_begin = cursor;
        while (cursor < query.size()) {
            const auto character = node_library_search_detail::decode_code_point(query, cursor);
            if (node_library_search_detail::is_whitespace(character.value))
                break;
            cursor += character.byte_count;
        }
        tokens.push_back(node_library_search_fold_ascii(query.substr(token_begin, cursor - token_begin)));
    }
    return tokens;
}

[[nodiscard]] inline bool node_library_search_matches(const std::vector<std::string>& folded_query_tokens,
                                                       const std::vector<std::string>& folded_fields) {
    return std::all_of(folded_query_tokens.begin(), folded_query_tokens.end(), [&](const auto& token) {
        return std::any_of(folded_fields.begin(), folded_fields.end(), [&](const auto& field) {
            return field.find(token) != std::string::npos;
        });
    });
}

struct NodeLibraryIdentity {
    std::string source_id;
    std::string language_id;
};

// PluginOrigin is stamped by trusted host registration/package resolution;
// browser metadata deliberately has no fields that can override these filters.
[[nodiscard]] inline NodeLibraryIdentity resolve_node_library_identity(
    const std::optional<NodeDescriptor::PluginOrigin>& origin) {
    if (!origin)
        return {"builtin", "cpp"};
    const auto language = origin->runtime == "python" ? "python" :
                          (origin->runtime == "native" || origin->runtime == "cpp") ? "cpp" : "";
    return {
        origin->managed_content_sha256.empty() ? "third_party" : "custom",
        language,
    };
}

[[nodiscard]] inline std::vector<NodeLibraryEntry> build_node_library_entries(const NodeCatalog& registry) {
    std::vector<NodeLibraryEntry> entries;
    entries.reserve(registry.descriptors().size());
    for (const auto& [type_id, descriptor] : registry.descriptors()) {
        const auto identity = resolve_node_library_identity(descriptor.plugin_origin);
        entries.push_back({type_id, descriptor.display_name, descriptor.execution_kind,
                           resolve_node_browser_classification(descriptor.browser_classification),
                           default_node_parameters(descriptor), descriptor.browser_tags,
                           descriptor.browser_description, identity.source_id, identity.language_id});
    }
    return entries;
}

[[nodiscard]] inline bool try_add_node_from_descriptor(ProjectModel& project, const NodeCatalog& registry,
                                                       const std::string& type_id, const std::string& node_id,
                                                       double x, double y) {
    const auto* descriptor = registry.find(type_id);
    if (descriptor == nullptr || descriptor->execution_kind == NodeExecutionKind::source)
        return false;
    if (!can_add_node_of_execution_kind(project, registry, descriptor->execution_kind))
        return false;

    auto node = make_node_instance(registry, project.nodes, type_id, node_id, x, y);
    if (!node)
        return false;
    project.nodes.push_back(std::move(*node));
    if (descriptor->execution_kind == NodeExecutionKind::sink)
        project.selected_output_node_id = node_id;
    ++project.revision;
    return true;
}

class ProjectDirtyState {
public:
    void mark_modified() noexcept { modified_ = true; }
    void mark_saved() noexcept { modified_ = false; }
    [[nodiscard]] bool is_modified() const noexcept { return modified_; }

private:
    bool modified_ = false;
};

[[nodiscard]] inline bool move_node_layout(ProjectModel& project, ProjectDirtyState& dirty_state,
                                           const std::string& node_id, double x, double y) {
    if (!try_move_node_layout(project, node_id, x, y))
        return false;
    dirty_state.mark_modified();
    return true;
}

[[nodiscard]] inline bool delete_node(ProjectModel& project, const NodeCatalog& registry,
                                      const std::string& node_id) {
    return try_delete_node(project, registry, node_id);
}

template <typename IdGenerator>
[[nodiscard]] inline std::optional<std::string> make_unique_node_id(
    const std::vector<NodeInstance>& existing_nodes, IdGenerator&& generate) {
    constexpr std::size_t max_attempts = 64;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        auto id = generate();
        if (id.empty())
            continue;
        const bool already_used = std::any_of(existing_nodes.begin(), existing_nodes.end(), [&](const auto& node) {
            return node.id == id;
        });
        if (!already_used)
            return id;
    }
    return std::nullopt;
}

class NodeSelectionState {
public:
    [[nodiscard]] bool select(const ProjectModel& project, const std::string& node_id) {
        if (node_id.empty()) {
            clear();
            return true;
        }
        if (!exists(project, node_id))
            return false;
        selected_node_id_ = node_id;
        selected_node_ids_ = {node_id};
        return true;
    }

    // Selects several validated nodes at once (rubber-band); the first id in the
    // given order becomes the primary selection driving the inspector.
    [[nodiscard]] bool select_many(const ProjectModel& project, const std::vector<std::string>& node_ids) {
        std::vector<std::string> chosen;
        for (const auto& node_id : node_ids) {
            if (node_id.empty() || std::find(chosen.begin(), chosen.end(), node_id) != chosen.end())
                continue;
            if (!exists(project, node_id))
                return false;
            chosen.push_back(node_id);
        }
        if (chosen.empty()) {
            clear();
            return true;
        }
        selected_node_ids_ = std::move(chosen);
        selected_node_id_ = selected_node_ids_.front();
        return true;
    }

    [[nodiscard]] const std::string& selected_node_id() const noexcept { return selected_node_id_; }
    [[nodiscard]] const std::vector<std::string>& selected_node_ids() const noexcept { return selected_node_ids_; }
    [[nodiscard]] bool is_selected(const std::string& node_id) const noexcept {
        return node_id == selected_node_id_ ||
               std::find(selected_node_ids_.begin(), selected_node_ids_.end(), node_id) != selected_node_ids_.end();
    }

private:
    static bool exists(const ProjectModel& project, const std::string& node_id) {
        return std::any_of(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
            return node.id == node_id;
        });
    }
    void clear() noexcept {
        selected_node_id_.clear();
        selected_node_ids_.clear();
    }

    std::string selected_node_id_;
    std::vector<std::string> selected_node_ids_;
};

} // namespace audioalgo::editor
