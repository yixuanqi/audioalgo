#include "audio_node_sdk/NodeBrowserTaxonomy.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace {

juce::var load_taxonomy_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/taxonomy_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.c_str(), static_cast<int>(bytes.size())));
}

} // namespace

TEST_CASE("NLC-01.a fixed browser taxonomy matches independent contract and missing metadata stays uncategorized",
          "[NLC-01.a][node_library][taxonomy]") {
    const auto expected = load_taxonomy_expectation();
    REQUIRE(expected.isObject());
    CHECK(static_cast<int>(expected.getProperty("schema_version", {})) == 1);
    CHECK(static_cast<int>(expected.getProperty("max_depth", {})) == 2);

    const auto* expected_categories = expected.getProperty("categories", {}).getArray();
    REQUIRE(expected_categories != nullptr);
    const auto actual_categories = audioalgo::node_browser_taxonomy();
    CHECK(actual_categories.size() == expected_categories->size());

    std::set<std::string> category_ids;
    std::set<std::string> subcategory_paths;
    const auto category_count = std::min(actual_categories.size(),
                                         static_cast<std::size_t>(expected_categories->size()));
    for (std::size_t index = 0; index < category_count; ++index) {
        const auto& actual = actual_categories[index];
        const auto& expected_category = expected_categories->getReference(static_cast<int>(index));
        const auto expected_id = expected_category.getProperty("id", {}).toString().toStdString();
        const auto expected_name = expected_category.getProperty("display_name", {}).toString().toStdString();
        CHECK(actual.id == expected_id);
        CHECK(actual.display_name == expected_name);
        CHECK(category_ids.insert(std::string(actual.id)).second);

        const auto* expected_subcategories = expected_category.getProperty("subcategory_ids", {}).getArray();
        REQUIRE(expected_subcategories != nullptr);
        CHECK(actual.subcategory_ids.size() == expected_subcategories->size());
        const auto subcategory_count = std::min(actual.subcategory_ids.size(),
                                                static_cast<std::size_t>(expected_subcategories->size()));
        for (std::size_t child = 0; child < subcategory_count; ++child) {
            const auto expected_subcategory = expected_subcategories->getReference(static_cast<int>(child))
                                                  .toString().toStdString();
            CHECK(actual.subcategory_ids[child] == expected_subcategory);
            CHECK(subcategory_paths.insert(std::string(actual.id) + "/" +
                                           std::string(actual.subcategory_ids[child])).second);
        }
    }

    // Only an explicit browser classification is accepted here; display names,
    // type IDs, execution kinds, and implementation languages are not inputs.
    for (const auto& descriptor_like_name : {"File", "Gain", "Mixer", "ChannelSwap", "PrintA"}) {
        (void) descriptor_like_name;
        const auto missing_browser = audioalgo::resolve_node_browser_classification(std::nullopt);
        CHECK(missing_browser.category_id == "uncategorized");
        CHECK_FALSE(missing_browser.subcategory_id.has_value());
    }
}
