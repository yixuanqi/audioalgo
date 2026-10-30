#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace audioalgo {

// The browser tree intentionally stops at a single subcategory level. It is
// descriptive metadata and is not consulted by node factories or execution.
struct NodeBrowserCategoryDefinition {
    std::string_view id;
    std::string_view display_name;
    std::span<const std::string_view> subcategory_ids;
};

struct NodeBrowserClassification {
    std::string category_id;
    std::optional<std::string> subcategory_id;
};

namespace node_browser_taxonomy_detail {
    inline constexpr std::array<std::string_view, 4> io_subcategories{
        "file", "generator", "output", "export"};
    inline constexpr std::array<std::string_view, 3> routing_subcategories{
        "mixer", "channel-mapping", "resampling"};
    inline constexpr std::array<std::string_view, 4> dynamics_subcategories{
        "gain", "normalization", "compressor", "limiter"};
    inline constexpr std::array<std::string_view, 3> filter_subcategories{"fir", "iir", "eq"};
    inline constexpr std::array<std::string_view, 5> effect_subcategories{
        "delay", "reverb", "modulation", "distortion", "pitch"};
    inline constexpr std::array<std::string_view, 3> restoration_subcategories{
        "denoise", "aec", "dereverb"};
    inline constexpr std::array<std::string_view, 3> spatial_subcategories{
        "beamforming", "localization", "spatialization"};
    inline constexpr std::array<std::string_view, 4> analysis_subcategories{
        "spectrum", "loudness", "features", "metrics"};
    inline constexpr std::array<std::string_view, 3> math_subcategories{"scalar", "matrix", "geometry"};
    inline constexpr std::array<std::string_view, 2> utility_subcategories{"logging", "inspection"};
    inline constexpr std::array<NodeBrowserCategoryDefinition, 11> categories{{
        {"audio.io", "Input & Output", io_subcategories},
        {"audio.routing", "Routing & Conversion", routing_subcategories},
        {"audio.dynamics", "Level & Dynamics", dynamics_subcategories},
        {"audio.filters", "Filters & EQ", filter_subcategories},
        {"audio.effects", "Time & Effects", effect_subcategories},
        {"audio.restoration", "Enhancement & Restoration", restoration_subcategories},
        {"audio.spatial", "Spatial Audio", spatial_subcategories},
        {"audio.analysis", "Analysis & Metrics", analysis_subcategories},
        {"math", "Math", math_subcategories},
        {"utility", "Utility & Debug", utility_subcategories},
        {"uncategorized", "Uncategorized", {}}
    }};
    static_assert(categories.back().id == "uncategorized");
    static_assert(categories.back().subcategory_ids.empty());
} // namespace node_browser_taxonomy_detail

[[nodiscard]] inline std::span<const NodeBrowserCategoryDefinition> node_browser_taxonomy() noexcept {
    return node_browser_taxonomy_detail::categories;
}

[[nodiscard]] inline NodeBrowserClassification resolve_node_browser_classification(
    const std::optional<NodeBrowserClassification>& declared) {
    return declared.value_or(NodeBrowserClassification{"uncategorized", std::nullopt});
}

} // namespace audioalgo
