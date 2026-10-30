#include "EditorNodeCreation.h"
#include "EditorGraphCanvas.h"
#include "EditorNodeLibrary.h"
#include "audio_node_authoring/Draft.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "audio_native_plugins/NativePlugins.h"
#include "audio_node_sdk/NodeBrowserTaxonomy.h"
#include "audio_editor_application/EditorController.h"
#include "plugins/builtin/registration.h"
#include "tests/fixtures/native_browser_v1/manifest.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

juce::var load_builtin_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/builtin_registration_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.c_str(), static_cast<int>(bytes.size())));
}

juce::var load_provenance_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/trusted_provenance_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.c_str(), static_cast<int>(bytes.size())));
}

void check_browser_path(const std::optional<audioalgo::NodeBrowserClassification>& actual,
                        const std::string& category, const std::string& subcategory) {
    CHECK(actual.has_value());
    if (!actual)
        return;
    CHECK(actual->category_id == category);
    REQUIRE(actual->subcategory_id.has_value());
    CHECK(*actual->subcategory_id == subcategory);
}

template <typename T>
constexpr bool browser_metadata_claims_source = requires(T value) { value.source_id; };

template <typename T>
constexpr bool browser_metadata_claims_language = requires(T value) { value.language_id; };

static_assert(!browser_metadata_claims_source<audioalgo::NodeBrowserClassification>);
static_assert(!browser_metadata_claims_language<audioalgo::NodeBrowserClassification>);

std::vector<std::string> string_array(const juce::var& value) {
    std::vector<std::string> result;
    const auto* array = value.getArray();
    if (array == nullptr)
        return result;
    for (const auto& item : *array)
        result.push_back(item.toString().toStdString());
    return result;
}

juce::var load_library_order_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_order_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

juce::var load_library_collapse_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_collapse_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

juce::var load_library_defaults_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_defaults_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

juce::var load_library_search_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_search_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

juce::var load_library_keyboard_safety_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_keyboard_safety_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

juce::var load_library_filters_expectation() {
    const auto path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
                      "tests/fixtures/node_browser/library_filters_v1.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
}

template <typename T>
void check_browser_descriptor(const T& descriptor,
                              const std::string& category, const std::string& subcategory,
                              const std::vector<std::string>& tags, const std::string& description) {
    CHECK(descriptor.browser_classification.has_value());
    if (descriptor.browser_classification) {
        CHECK(descriptor.browser_classification->category_id == category);
        CHECK(descriptor.browser_classification->subcategory_id.has_value());
        if (descriptor.browser_classification->subcategory_id)
            CHECK(*descriptor.browser_classification->subcategory_id == subcategory);
    }
    CHECK(descriptor.browser_tags == tags);
    if constexpr (requires { descriptor.browser_description; })
        CHECK(descriptor.browser_description == description);
    else
        CHECK(false);
}

template <typename T>
void check_browser_entry(const T& entry,
                         const std::string& category, const std::string& subcategory,
                         const std::vector<std::string>& tags, const std::string& description) {
    CHECK(entry.browser_classification.category_id == category);
    CHECK(entry.browser_classification.subcategory_id.has_value());
    if (entry.browser_classification.subcategory_id)
        CHECK(*entry.browser_classification.subcategory_id == subcategory);
    CHECK(entry.browser_tags == tags);
    if constexpr (requires { entry.browser_description; })
        CHECK(entry.browser_description == description);
    else
        CHECK(false);
}

std::string python_browser_manifest(bool declare_browser) {
    const std::string browser = declare_browser
        ? R"json(,"browser":{"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["gain","volume","增益"],"description":"调整音频电平"})json"
        : std::string{};
    return R"json({"schema_version":1,"runtime":"python","plugin_id":"fixture.browser_python","plugin_version":"1.2.3","nodes":[{"type_id":"fixture.browser_python","version":"1.2.3","display_name":"Browser Python","execution":{"kind":"block","stateful":false},"entrypoint":"browser_node:Node","inputs":[{"id":"audio_in","type":"audio.pcm@1","required":true}],"outputs":[{"id":"audio_out","type":"audio.pcm@1"}],"parameters":[{"id":"gain","type":"float","default":1.0}],"format":{"sample_rates_hz":[48000],"channels":[2]})json" + browser + R"json(}]})json";
}

void write_python_package(const std::filesystem::path& root, bool declare_browser) {
    std::filesystem::create_directories(root);
    std::ofstream manifest(root / "manifest.json", std::ios::binary);
    manifest << python_browser_manifest(declare_browser);
    manifest.close();
    REQUIRE(manifest.good());
}

std::string python_browser_case_manifest(const std::string& plugin_id, const std::string& type_id,
                                         const std::string& browser_field,
                                         const std::string& execution_kind = "block") {
    return R"json({"schema_version":1,"runtime":"python","plugin_id":")json" + plugin_id +
        R"json(","plugin_version":"1.0.0","nodes":[{"type_id":")json" + type_id +
        R"json(","version":"1.0.0","display_name":")json" + type_id +
        R"json(","execution":{"kind":")json" + execution_kind +
        R"json(","stateful":false},"entrypoint":"browser_node:Node","inputs":[{"id":"audio_in","type":"audio.pcm@1","required":true}],"outputs":[{"id":"audio_out","type":"audio.pcm@1"}],"parameters":[],"format":{"sample_rates_hz":[48000],"channels":[2]})json" +
        browser_field + R"json(}]})json";
}

void write_python_browser_case_package(const std::filesystem::path& root,
                                       const std::string& plugin_id, const std::string& type_id,
                                       const std::string& browser_field,
                                       const std::string& execution_kind = "block") {
    std::filesystem::create_directories(root);
    std::ofstream manifest(root / "manifest.json", std::ios::binary);
    manifest << python_browser_case_manifest(plugin_id, type_id, browser_field, execution_kind);
    manifest.close();
    REQUIRE(manifest.good());
}

juce::Component* find_component(juce::Component& root, const juce::String& component_id) {
    if (root.getComponentID() == component_id)
        return &root;
    for (int index = 0; index < root.getNumChildComponents(); ++index) {
        if (auto* found = find_component(*root.getChildComponent(index), component_id))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("NLC-01.b builtin registrations and equivalent descriptors carry generic browser classifications",
          "[NLC-01.b][node_library][registration]") {
    const auto expected = load_builtin_expectation();
    REQUIRE(expected.isObject());
    CHECK(static_cast<int>(expected.getProperty("schema_version", {})) == 1);
    const auto* builtins = expected.getProperty("builtins", {}).getArray();
    REQUIRE(builtins != nullptr);

    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    for (const auto& expected_builtin : *builtins) {
        const auto type_id = expected_builtin.getProperty("type_id", {}).toString().toStdString();
        const auto category = expected_builtin.getProperty("category_id", {}).toString().toStdString();
        const auto subcategory = expected_builtin.getProperty("subcategory_id", {}).toString().toStdString();
        const auto* descriptor = registry.find(type_id);
        REQUIRE(descriptor != nullptr);
        check_browser_path(descriptor->browser_classification, category, subcategory);

        const auto entries = audioalgo::editor::build_node_library_entries(registry);
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
            return candidate.type_id == type_id;
        });
        REQUIRE(entry != entries.end());
        check_browser_path(std::optional{entry->browser_classification}, category, subcategory);
    }

    const auto equivalent = expected.getProperty("equivalent_descriptor", {});
    REQUIRE(equivalent.isObject());
    audioalgo::NodeDescriptor descriptor{
        equivalent.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
        equivalent.getProperty("display_name", {}).toString().toStdString(),
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    const auto custom_category = equivalent.getProperty("category_id", {}).toString().toStdString();
    const auto custom_subcategory = equivalent.getProperty("subcategory_id", {}).toString().toStdString();
    descriptor.browser_classification = audioalgo::NodeBrowserClassification{
        custom_category, custom_subcategory};
    REQUIRE(registry.register_descriptor(descriptor));

    check_browser_path(registry.find(descriptor.type_id)->browser_classification,
                       custom_category, custom_subcategory);
    const auto entries = audioalgo::editor::build_node_library_entries(registry);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
        return candidate.type_id == descriptor.type_id;
    });
    REQUIRE(entry != entries.end());
    CHECK(entry->display_name == descriptor.display_name);
    check_browser_path(std::optional{entry->browser_classification}, custom_category, custom_subcategory);
}

TEST_CASE("NLC-01.c one primary class plus tags and trusted library filter identities",
          "[NLC-01.c][node_library][provenance]") {
    const auto expected = load_provenance_expectation();
    REQUIRE(expected.isObject());
    CHECK(static_cast<int>(expected.getProperty("schema_version", {})) == 1);
    const auto* nodes = expected.getProperty("nodes", {}).getArray();
    REQUIRE(nodes != nullptr);

    audioalgo::NodeRegistry registry;
    for (const auto& item : *nodes) {
        audioalgo::NodeDescriptor descriptor{
            item.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
            item.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};

        const auto browser = item.getProperty("browser", {});
        if (browser.isObject()) {
            descriptor.browser_classification = audioalgo::NodeBrowserClassification{
                browser.getProperty("category_id", {}).toString().toStdString(),
                browser.getProperty("subcategory_id", {}).toString().toStdString()};
            descriptor.browser_tags = string_array(browser.getProperty("tags", {}));
        }

        const auto trusted = item.getProperty("trusted_registration", {});
        const auto expected_source = trusted.getProperty("source_id", {}).toString().toStdString();
        if (expected_source != "builtin") {
            descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
                trusted.getProperty("plugin_id", {}).toString().toStdString(), "1.0.0", "",
                "fixture-manifest-sha256", trusted.getProperty("runtime", {}).toString().toStdString()};
            descriptor.plugin_origin->managed_content_sha256 =
                trusted.getProperty("managed_content_sha256", {}).toString().toStdString();
        }
        REQUIRE(registry.register_descriptor(descriptor));
    }

    const auto entries = audioalgo::editor::build_node_library_entries(registry);
    REQUIRE(entries.size() == nodes->size());
    for (const auto& expected_node : *nodes) {
        const auto type_id = expected_node.getProperty("type_id", {}).toString().toStdString();
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
            return candidate.type_id == type_id;
        });
        REQUIRE(entry != entries.end());

        const auto browser = expected_node.getProperty("browser", {});
        if (browser.isObject()) {
            CHECK(entry->browser_classification.category_id ==
                  browser.getProperty("category_id", {}).toString().toStdString());
            REQUIRE(entry->browser_classification.subcategory_id.has_value());
            CHECK(*entry->browser_classification.subcategory_id ==
                  browser.getProperty("subcategory_id", {}).toString().toStdString());
            CHECK(entry->browser_tags == string_array(browser.getProperty("tags", {})));

            const auto trusted = expected_node.getProperty("trusted_registration", {});
            CHECK(browser.getProperty("source_id", {}).toString().toStdString() !=
                  trusted.getProperty("source_id", {}).toString().toStdString());
            CHECK(browser.getProperty("language_id", {}).toString().toStdString() !=
                  trusted.getProperty("language_id", {}).toString().toStdString());
        } else {
            CHECK(entry->browser_classification.category_id == "uncategorized");
            CHECK_FALSE(entry->browser_classification.subcategory_id.has_value());
            CHECK(entry->browser_tags.empty());
        }

        const auto trusted = expected_node.getProperty("trusted_registration", {});
        CHECK(entry->source_id == trusted.getProperty("source_id", {}).toString().toStdString());
        CHECK(entry->language_id == trusted.getProperty("language_id", {}).toString().toStdString());
    }

    const auto legacy = std::find_if(entries.begin(), entries.end(), [](const auto& candidate) {
        return candidate.type_id == "legacy.print_a";
    });
    REQUIRE(legacy != entries.end());
    CHECK(legacy->display_name == "PrintA");
}

TEST_CASE("NLC-01.c legacy PrintA remains visible in the production node library",
          "[NLC-01.c][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::NodeRegistry registry;
    audioalgo::NodeDescriptor descriptor{
        "legacy.print_a", "1.0.0", "PrintA",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
        "legacy.print_package", "1.0.0", "", "fixture-manifest-sha256", "python"};
    descriptor.plugin_origin->managed_content_sha256 = "b24721e7d835e65d02fc5f5cd87806bd";
    REQUIRE(registry.register_descriptor(descriptor));

    std::vector<std::string> clicked_ids;
    audioalgo::editor::NodeLibrary library(
        registry, [&](std::string type_id) { clicked_ids.push_back(std::move(type_id)); }, [] { return 7; });
    auto* print_a = dynamic_cast<juce::Button*>(find_component(library, "library.legacy.print_a"));
    REQUIRE(print_a != nullptr);
    CHECK(print_a->getButtonText() == "PrintA");
    REQUIRE(print_a->isEnabled());
    print_a->triggerClick();
    auto* message_manager = juce::MessageManager::getInstance();
    REQUIRE(message_manager != nullptr);
    const auto click_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (clicked_ids.empty() && std::chrono::steady_clock::now() < click_deadline)
        message_manager->runDispatchLoopUntil(10);
    REQUIRE(clicked_ids.size() == 1);
    CHECK(clicked_ids.front() == "legacy.print_a");
}

TEST_CASE("NLC-02.a browser schema v1 is consistent across builtin Python and authoring routes",
          "[NLC-02.a][node_library][registration]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc02a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC02A_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';

    constexpr const char* category = "audio.dynamics";
    constexpr const char* subcategory = "gain";
    constexpr const char* description = "调整音频电平";
    const std::vector<std::string> tags{"gain", "volume", "增益"};

    audioalgo::NodeRegistry builtins;
    REQUIRE(audioalgo::register_builtin_nodes(builtins));
    const auto* builtin_gain = builtins.find("builtin.gain");
    REQUIRE(builtin_gain != nullptr);
    CHECK(builtin_gain->browser_classification.has_value());
    if (builtin_gain->browser_classification) {
        CHECK(builtin_gain->browser_classification->category_id == category);
        REQUIRE(builtin_gain->browser_classification->subcategory_id.has_value());
        CHECK(*builtin_gain->browser_classification->subcategory_id == subcategory);
    }
    const auto builtin_entries = audioalgo::editor::build_node_library_entries(builtins);
    const auto builtin_entry = std::find_if(builtin_entries.begin(), builtin_entries.end(), [](const auto& entry) {
        return entry.type_id == "builtin.gain";
    });
    REQUIRE(builtin_entry != builtin_entries.end());
    CHECK(builtin_entry->browser_classification.category_id == category);
    REQUIRE(builtin_entry->browser_classification.subcategory_id.has_value());
    CHECK(*builtin_entry->browser_classification.subcategory_id == subcategory);

    const auto python_root = artifact_root / "external-python-v1";
    const auto legacy_root = artifact_root / "legacy-python-no-browser";
    write_python_package(python_root, true);
    write_python_package(legacy_root, false);
    const auto legacy_bytes_before = python_browser_manifest(false);
    CHECK(std::filesystem::file_size(legacy_root / "manifest.json") == legacy_bytes_before.size());

    audioalgo::NodeRegistry python_registry;
    const auto python_discovery = audioalgo::load_native_plugins(python_registry, {python_root});
    REQUIRE(python_discovery.diagnostics.empty());
    REQUIRE(python_discovery.loaded_plugin_ids == std::vector<std::string>{"fixture.browser_python"});
    const auto* python = python_registry.find("fixture.browser_python");
    REQUIRE(python != nullptr);
    check_browser_descriptor(*python, category, subcategory, tags, description);
    const auto python_entries = audioalgo::editor::build_node_library_entries(python_registry);
    const auto python_entry = std::find_if(python_entries.begin(), python_entries.end(), [](const auto& entry) {
        return entry.type_id == "fixture.browser_python";
    });
    REQUIRE(python_entry != python_entries.end());
    check_browser_entry(*python_entry, category, subcategory, tags, description);
    CHECK(python_entry->source_id == "third_party");
    CHECK(python_entry->language_id == "python");

    audioalgo::NodeRegistry legacy_registry;
    const auto legacy_discovery = audioalgo::load_native_plugins(legacy_registry, {legacy_root});
    REQUIRE(legacy_discovery.diagnostics.empty());
    const auto* legacy = legacy_registry.find("fixture.browser_python");
    REQUIRE(legacy != nullptr);
    CHECK_FALSE(legacy->browser_classification.has_value());
    CHECK(legacy->browser_tags.empty());
    std::ifstream legacy_manifest_after(legacy_root / "manifest.json", std::ios::binary);
    REQUIRE(legacy_manifest_after.good());
    const std::string legacy_bytes_after{std::istreambuf_iterator<char>(legacy_manifest_after), std::istreambuf_iterator<char>()};
    CHECK(legacy_bytes_after == legacy_bytes_before);

    CHECK(python->type_id == legacy->type_id);
    CHECK(python->version == legacy->version);
    CHECK(python->display_name == legacy->display_name);
    REQUIRE(python->ports.size() == legacy->ports.size());
    for (std::size_t index = 0; index < python->ports.size(); ++index) {
        CHECK(python->ports[index].id == legacy->ports[index].id);
        CHECK(python->ports[index].type_id == legacy->ports[index].type_id);
        CHECK(python->ports[index].direction == legacy->ports[index].direction);
        CHECK(python->ports[index].required == legacy->ports[index].required);
    }
    REQUIRE(python->parameters.size() == legacy->parameters.size());
    CHECK(python->parameters.front().id == legacy->parameters.front().id);
    CHECK(python->parameters.front().kind == legacy->parameters.front().kind);
    CHECK(std::get<double>(python->parameters.front().default_value) == 1.0);
    CHECK(std::get<double>(legacy->parameters.front().default_value) == 1.0);
    CHECK(python->audio_format.sample_rates_hz == legacy->audio_format.sample_rates_hz);
    CHECK(python->audio_format.channel_counts == legacy->audio_format.channel_counts);
    CHECK(python->capabilities.requires_offline_task);
    CHECK(legacy->capabilities.requires_offline_task);
    CHECK(python->capabilities.supports_variable_block_size == legacy->capabilities.supports_variable_block_size);

    const auto native_root = artifact_root / "native-browser-v1";
    std::filesystem::create_directories(native_root);
    std::ofstream native_manifest(native_root / "manifest.json", std::ios::binary);
    native_manifest.write(audioalgo::test_fixtures::native_browser_v1::manifest,
        static_cast<std::streamsize>(std::char_traits<char>::length(audioalgo::test_fixtures::native_browser_v1::manifest)));
    native_manifest.close();
    REQUIRE(native_manifest.good());
    std::filesystem::copy_file(AUDIOALGO_TEST_NATIVE_BROWSER_DLL, native_root / "native_browser.dll",
                               std::filesystem::copy_options::overwrite_existing);
    audioalgo::NodeRegistry native_registry;
    REQUIRE(audioalgo::register_builtin_nodes(native_registry));
    const auto native_discovery = audioalgo::load_native_plugins(native_registry, {native_root});
    REQUIRE(native_discovery.diagnostics.empty());
    REQUIRE(native_discovery.loaded_plugin_ids == std::vector<std::string>{"fixture.native_browser"});
    const auto* native = native_registry.find("fixture.native_browser" );
    REQUIRE(native != nullptr);
    check_browser_descriptor(*native, category, subcategory, tags, description);
    const auto native_entries = audioalgo::editor::build_node_library_entries(native_registry);
    const auto native_entry = std::find_if(native_entries.begin(), native_entries.end(), [](const auto& entry) {
        return entry.type_id == "fixture.native_browser";
    });
    REQUIRE(native_entry != native_entries.end());
    check_browser_entry(*native_entry, category, subcategory, tags, description);
    CHECK(native_entry->source_id == "third_party");
    CHECK(native_entry->language_id == "cpp");

    const auto mismatch_root = artifact_root / "native-browser-mismatch";
    std::filesystem::create_directories(mismatch_root);
    auto mismatch_manifest = juce::JSON::parse(juce::String::fromUTF8(
        audioalgo::test_fixtures::native_browser_v1::manifest,
        static_cast<int>(std::char_traits<char>::length(audioalgo::test_fixtures::native_browser_v1::manifest))));
    REQUIRE(mismatch_manifest.isObject());
    auto* mismatch_nodes = mismatch_manifest.getProperty("nodes", {}).getArray();
    REQUIRE(mismatch_nodes != nullptr);
    auto browser = (*mismatch_nodes)[0].getProperty("browser", {});
    REQUIRE(browser.isObject());
    browser.getDynamicObject()->setProperty("category_id", "audio.filters");
    std::ofstream(mismatch_root / "manifest.json", std::ios::binary)
        << juce::JSON::toString(mismatch_manifest, false).toStdString();
    std::filesystem::copy_file(AUDIOALGO_TEST_NATIVE_BROWSER_DLL, mismatch_root / "native_browser.dll",
                               std::filesystem::copy_options::overwrite_existing);
    audioalgo::NodeRegistry mismatch_registry;
    const auto mismatch_discovery = audioalgo::load_native_plugins(mismatch_registry, {mismatch_root});
    REQUIRE(mismatch_discovery.diagnostics.size() == 1);
    CHECK(mismatch_discovery.diagnostics.front().code == "plugin_descriptor_mismatch");
    CHECK(mismatch_registry.find("fixture.native_browser") == nullptr);

    const auto old_identity_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/PC0601-native-log-queue/baseline-identity.json";
    std::ifstream old_identity_input(old_identity_path, std::ios::binary);
    REQUIRE(old_identity_input.good());
    const std::string old_identity_bytes{std::istreambuf_iterator<char>(old_identity_input), std::istreambuf_iterator<char>()};
    const auto old_identity = juce::JSON::parse(juce::String::fromUTF8(
        old_identity_bytes.c_str(), static_cast<int>(old_identity_bytes.size())));
    REQUIRE(old_identity.isObject());
    const auto* old_artifacts = old_identity.getProperty("artifacts", {}).getArray();
    REQUIRE(old_artifacts != nullptr);
    std::filesystem::path old_native_package;
    std::string old_native_hash;
    for (const auto& artifact : *old_artifacts) {
        if (artifact.getProperty("path", {}).toString().toStdString().ends_with("external_stateful.dll")) {
            const auto dll_path = std::filesystem::u8path(artifact.getProperty("path", {}).toString().toStdString());
            old_native_package = dll_path.parent_path();
            old_native_hash = artifact.getProperty("sha256", {}).toString().toStdString();
        }
    }
    REQUIRE_FALSE(old_native_package.empty());
    REQUIRE(std::filesystem::is_regular_file(old_native_package / "manifest.json"));
    REQUIRE(std::filesystem::is_regular_file(old_native_package / "external_stateful.dll"));
    std::ifstream old_manifest_before(old_native_package / "manifest.json", std::ios::binary);
    REQUIRE(old_manifest_before.good());
    const std::string old_manifest_bytes_before{std::istreambuf_iterator<char>(old_manifest_before), std::istreambuf_iterator<char>()};
    audioalgo::NodeRegistry old_native_registry;
    const auto old_native_discovery = audioalgo::load_native_plugins(old_native_registry, {old_native_package});
    REQUIRE(old_native_discovery.diagnostics.empty());
    REQUIRE(old_native_discovery.loaded_plugin_ids == std::vector<std::string>{"example.external_stateful"});
    const auto* old_native = old_native_registry.find("example.external_stateful");
    REQUIRE(old_native != nullptr);
    CHECK_FALSE(old_native->browser_classification.has_value());
    CHECK(old_native->browser_tags.empty());
    CHECK(juce::SHA256(juce::File((old_native_package / "external_stateful.dll").generic_string())).toHexString().toStdString() == old_native_hash);
    std::ifstream old_manifest_after(old_native_package / "manifest.json", std::ios::binary);
    REQUIRE(old_manifest_after.good());
    const std::string old_manifest_bytes_after{std::istreambuf_iterator<char>(old_manifest_after), std::istreambuf_iterator<char>()};
    CHECK(old_manifest_bytes_after == old_manifest_bytes_before);

    auto draft = audioalgo::authoring::python_draft("nlc02a-authoring");
    auto declaration = juce::JSON::parse(juce::String::fromUTF8(
        draft.declaration.c_str(), static_cast<int>(draft.declaration.size())));
    REQUIRE(declaration.isObject());
    constexpr std::string_view authored_browser_json =
        R"json({"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["gain","volume","增益"],"description":"调整音频电平"})json";
    declaration.getDynamicObject()->setProperty("browser", juce::JSON::parse(juce::String::fromUTF8(
        authored_browser_json.data(), static_cast<int>(authored_browser_json.size()))));
    draft.declaration = juce::JSON::toString(declaration, false).toStdString();
    auto authoring = std::make_shared<audioalgo::authoring::PythonAuthoring>(artifact_root / "author-library");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto checked = authoring->check(draft, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    const auto& ready = std::get<audioalgo::authoring::ReadyPackage>(checked);
    check_browser_descriptor(ready.descriptor, category, subcategory, tags, description);
    auto applied = authoring->apply(ready, draft, std::make_shared<audioalgo::NodeRegistry>(), cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied));
    const auto& installed = std::get<audioalgo::authoring::AppliedPackage>(applied);
    const auto* authored = installed.catalog->find(ready.descriptor.type_id);
    REQUIRE(authored != nullptr);
    check_browser_descriptor(*authored, category, subcategory, tags, description);
    CHECK(std::filesystem::is_regular_file(installed.package_root / "manifest.json"));
}

TEST_CASE("NLC-02.b damaged browser metadata degrades without hiding executable nodes",
          "[NLC-02.b][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc02b/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC02B_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';

    juce::Array<juce::var> observations;
    const auto record_observation = [&](const std::string& case_id,
                                       const audioalgo::NodeDescriptor* descriptor,
                                       const audioalgo::PluginDiscovery& discovery) {
        const auto utf8 = [](const std::string& value) { return juce::String::fromUTF8(value.c_str()); };
        juce::DynamicObject::Ptr observation = new juce::DynamicObject;
        observation->setProperty("case_id", utf8(case_id));
        observation->setProperty("registered", descriptor != nullptr);
        observation->setProperty("browser_classification_present",
                                 descriptor != nullptr && descriptor->browser_classification.has_value());
        juce::Array<juce::var> tags;
        juce::Array<juce::var> diagnostics;
        if (descriptor != nullptr) {
            const auto classification = audioalgo::resolve_node_browser_classification(
                descriptor->browser_classification);
            observation->setProperty("category_id", utf8(classification.category_id));
            observation->setProperty("subcategory_id",
                descriptor->browser_classification && descriptor->browser_classification->subcategory_id
                    ? juce::var(utf8(*descriptor->browser_classification->subcategory_id)) : juce::var{});
            for (const auto& tag : descriptor->browser_tags)
                tags.add(utf8(tag));
            observation->setProperty("description", utf8(descriptor->browser_description));
            observation->setProperty("requires_offline_task", descriptor->capabilities.requires_offline_task);
        } else {
            observation->setProperty("category_id", juce::var{});
            observation->setProperty("subcategory_id", juce::var{});
            observation->setProperty("description", juce::String{});
            observation->setProperty("requires_offline_task", false);
        }
        for (const auto& diagnostic : discovery.diagnostics) {
            juce::DynamicObject::Ptr value = new juce::DynamicObject;
            value->setProperty("code", utf8(diagnostic.code));
            value->setProperty("message", utf8(diagnostic.message));
            diagnostics.add(juce::var(value.get()));
        }
        observation->setProperty("tags", tags);
        observation->setProperty("diagnostics", diagnostics);
        observations.add(juce::var(observation.get()));
    };

    const auto unknown_category_root = artifact_root / "unknown-category";
    write_python_browser_case_package(unknown_category_root, "fixture.browser_unknown_category",
        "fixture.unknown_category",
        R"json(,"browser":{"schema_version":1,"category_id":"audio.future_domain","subcategory_id":"future-subcategory","tags":["keep-me","future"],"description":"future category remains searchable"})json");
    audioalgo::NodeRegistry unknown_category_registry;
    const auto unknown_category_discovery = audioalgo::load_native_plugins(
        unknown_category_registry, {unknown_category_root});
    CHECK(unknown_category_discovery.diagnostics.empty());
    CHECK(unknown_category_discovery.loaded_plugin_ids ==
          std::vector<std::string>{"fixture.browser_unknown_category"});
    const auto* unknown_category = unknown_category_registry.find("fixture.unknown_category");
    REQUIRE(unknown_category != nullptr);
    const auto unknown_category_classification = audioalgo::resolve_node_browser_classification(
        unknown_category->browser_classification);
    CHECK(unknown_category_classification.category_id == "uncategorized");
    CHECK_FALSE(unknown_category_classification.subcategory_id.has_value());
    CHECK(unknown_category->browser_tags == std::vector<std::string>{"keep-me", "future"});
    CHECK(unknown_category->browser_description == "future category remains searchable");
    record_observation("unknown-category", unknown_category, unknown_category_discovery);

    const auto unknown_subcategory_root = artifact_root / "unknown-subcategory";
    write_python_browser_case_package(unknown_subcategory_root, "fixture.browser_unknown_subcategory",
        "fixture.unknown_subcategory",
        R"json(,"browser":{"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"future-gain","tags":["retain-tag"],"description":"retain description"})json");
    audioalgo::NodeRegistry unknown_subcategory_registry;
    const auto unknown_subcategory_discovery = audioalgo::load_native_plugins(
        unknown_subcategory_registry, {unknown_subcategory_root});
    CHECK(unknown_subcategory_discovery.diagnostics.empty());
    const auto* unknown_subcategory = unknown_subcategory_registry.find("fixture.unknown_subcategory");
    REQUIRE(unknown_subcategory != nullptr);
    REQUIRE(unknown_subcategory->browser_classification.has_value());
    CHECK(unknown_subcategory->browser_classification->category_id == "audio.dynamics");
    CHECK_FALSE(unknown_subcategory->browser_classification->subcategory_id.has_value());
    CHECK(unknown_subcategory->browser_tags == std::vector<std::string>{"retain-tag"});
    CHECK(unknown_subcategory->browser_description == "retain description");
    record_observation("unknown-subcategory", unknown_subcategory, unknown_subcategory_discovery);

    const auto check_browser_downgrade = [&](const std::string& directory, const std::string& plugin_id,
                                              const std::string& type_id, const std::string& browser_field,
                                              const std::string& expected_field) {
        const auto package = artifact_root / directory;
        write_python_browser_case_package(package, plugin_id, type_id, browser_field);
        audioalgo::NodeRegistry registry;
        const auto discovery = audioalgo::load_native_plugins(registry, {package});
        CHECK(discovery.loaded_plugin_ids == std::vector<std::string>{plugin_id});
        CHECK(discovery.diagnostics.size() == 1);
        if (!discovery.diagnostics.empty()) {
            CHECK(discovery.diagnostics.front().code == "node_browser_metadata_invalid");
            CHECK(discovery.diagnostics.front().plugin_id == plugin_id);
            CHECK(discovery.diagnostics.front().message.find(expected_field) != std::string::npos);
            CHECK(discovery.diagnostics.front().manifest == package / "manifest.json");
        }
        const auto* descriptor = registry.find(type_id);
        REQUIRE(descriptor != nullptr);
        CHECK_FALSE(descriptor->browser_classification.has_value());
        CHECK(descriptor->browser_tags.empty());
        CHECK(descriptor->browser_description.empty());
        CHECK(descriptor->capabilities.requires_offline_task);
        record_observation(directory, descriptor, discovery);
        return registry;
    };

    auto bad_browser_type_registry = check_browser_downgrade("bad-browser-type",
        "fixture.browser_bad_type", "fixture.bad_browser_type", R"json(,"browser":"not-an-object")json",
        "browser");
    auto bad_version_type_registry = check_browser_downgrade("bad-version-type",
        "fixture.browser_bad_version_type", "fixture.bad_version_type",
        R"json(,"browser":{"schema_version":"one","category_id":"audio.dynamics","tags":["must-drop"],"description":"must drop"})json",
        "schema_version");
    auto future_version_registry = check_browser_downgrade("future-version",
        "fixture.browser_future_version", "fixture.future_version",
        R"json(,"browser":{"schema_version":2,"category_id":"audio.dynamics","tags":["must-drop"],"description":"must drop"})json",
        "schema_version");
    auto bad_category_type_registry = check_browser_downgrade("bad-category-type",
        "fixture.browser_bad_category_type", "fixture.bad_category_type",
        R"json(,"browser":{"schema_version":1,"category_id":42,"tags":["must-drop"],"description":"must drop"})json",
        "category_id");
    CHECK(bad_browser_type_registry.find("fixture.bad_browser_type") != nullptr);
    CHECK(bad_version_type_registry.find("fixture.bad_version_type") != nullptr);
    CHECK(future_version_registry.find("fixture.future_version") != nullptr);
    CHECK(bad_category_type_registry.find("fixture.bad_category_type") != nullptr);

    std::vector<std::string> clicked_ids;
    audioalgo::editor::NodeLibrary library(
        unknown_category_registry, [&](std::string type_id) { clicked_ids.push_back(std::move(type_id)); }, [] { return 9; });
    library.setSize(240, 620);
    auto* unknown_category_button = dynamic_cast<juce::Button*>(
        find_component(library, "library.fixture.unknown_category"));
    REQUIRE(unknown_category_button != nullptr);
    CHECK(unknown_category_button->isEnabled());
    unknown_category_button->triggerClick();
    auto* message_manager = juce::MessageManager::getInstance();
    REQUIRE(message_manager != nullptr);
    const auto click_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (clicked_ids.empty() && std::chrono::steady_clock::now() < click_deadline)
        message_manager->runDispatchLoopUntil(10);
    REQUIRE(clicked_ids == std::vector<std::string>{"fixture.unknown_category"});
    audioalgo::ProjectModel project;
    project.project_id = "nlc02b-invalid-browser-stays-usable";
    CHECK(audioalgo::editor::try_add_node_from_descriptor(
        project, unknown_category_registry, clicked_ids.front(), "browser-node", 120.0, 90.0));
    REQUIRE(project.nodes.size() == 1);
    CHECK(project.nodes.front().type_id == "fixture.unknown_category");
    const auto processor = unknown_category_registry.create_processor("fixture.unknown_category", "browser-node");
    CHECK(processor.processor != nullptr);
    CHECK_FALSE(processor.diagnostic.has_value());
    const bool processor_created = processor.processor != nullptr && !processor.diagnostic.has_value();

    const auto invalid_execution_root = artifact_root / "invalid-execution";
    write_python_browser_case_package(invalid_execution_root, "fixture.browser_invalid_execution",
        "fixture.invalid_execution", R"json(,"browser":17)json", "unsupported_execution_kind");
    audioalgo::NodeRegistry invalid_execution_registry;
    const auto invalid_execution_discovery = audioalgo::load_native_plugins(
        invalid_execution_registry, {invalid_execution_root});
    CHECK(invalid_execution_discovery.loaded_plugin_ids.empty());
    REQUIRE(invalid_execution_discovery.diagnostics.size() == 1);
    CHECK(invalid_execution_discovery.diagnostics.front().code == "plugin_manifest_invalid");
    CHECK(invalid_execution_registry.find("fixture.invalid_execution") == nullptr);
    record_observation("invalid-execution", invalid_execution_registry.find("fixture.invalid_execution"),
                       invalid_execution_discovery);

    juce::DynamicObject::Ptr component = new juce::DynamicObject;
    component->setProperty("type_id", "fixture.unknown_category");
    component->setProperty("button_enabled", unknown_category_button->isEnabled());
    component->setProperty("clicked_type_id", clicked_ids.empty() ? juce::var{} : juce::var(juce::String::fromUTF8(clicked_ids.front().c_str())));
    component->setProperty("node_added", !project.nodes.empty() && project.nodes.front().type_id == "fixture.unknown_category");
    component->setProperty("processor_created", processor_created);
    juce::DynamicObject::Ptr root = new juce::DynamicObject;
    root->setProperty("schema_version", 1);
    root->setProperty("observations", observations);
    root->setProperty("component", juce::var(component.get()));
    const auto observation_path = artifact_root / "browser-fallback-observations.json";
    std::ofstream observation_file(observation_path, std::ios::binary);
    observation_file << juce::JSON::toString(juce::var(root.get()), true).toStdString();
    observation_file.close();
    REQUIRE(observation_file.good());
}

TEST_CASE("NLC-02.c browser metadata enforces UTF-8 byte budgets and stable tag normalization",
          "[NLC-02.c][node_library][registration]") {
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc02c/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC02C_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';

    const auto expectation_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_browser/boundaries_v1.json";
    std::ifstream expectation_file(expectation_path, std::ios::binary);
    REQUIRE(expectation_file.good());
    const std::string expectation_bytes{std::istreambuf_iterator<char>(expectation_file),
                                        std::istreambuf_iterator<char>()};
    const auto expectation = juce::JSON::parse(juce::String::fromUTF8(
        expectation_bytes.data(), static_cast<int>(expectation_bytes.size())));
    REQUIRE(expectation.isObject());
    const auto limits = expectation.getProperty("limits", {});
    REQUIRE(limits.isObject());
    const auto classification_limit = static_cast<std::size_t>(static_cast<int>(limits.getProperty("classification_utf8_bytes", {})));
    const auto tag_count_limit = static_cast<std::size_t>(static_cast<int>(limits.getProperty("tag_count", {})));
    const auto tag_byte_limit = static_cast<std::size_t>(static_cast<int>(limits.getProperty("tag_utf8_bytes", {})));
    const auto description_limit = static_cast<std::size_t>(static_cast<int>(limits.getProperty("description_utf8_bytes", {})));
    const auto manifest_limit = static_cast<std::size_t>(static_cast<int>(limits.getProperty("manifest_bytes", {})));
    CHECK(classification_limit == 96);
    CHECK(tag_count_limit == 32);
    CHECK(tag_byte_limit == 64);
    CHECK(description_limit == 2048);
    CHECK(manifest_limit == 1024 * 1024);

    const auto as_utf8 = [](const juce::String& value) {
        const auto encoded = value.toUTF8();
        return std::string(encoded.getAddress());
    };
    const auto as_juce = [](const std::string& value) {
        return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
    };
    const auto scalar = as_utf8(expectation.getProperty("multibyte_scalar", {}).toString());
    REQUIRE(scalar.size() == 3);
    const auto repeated = [](const std::string& value, std::size_t count) {
        std::string result;
        result.reserve(value.size() * count);
        for (std::size_t i = 0; i < count; ++i)
            result += value;
        return result;
    };
    const auto serialize_browser = [&](const std::string& category,
                                       const std::optional<std::string>& subcategory,
                                       const std::vector<std::string>& tags,
                                       const std::string& description,
                                       const std::function<void(juce::DynamicObject&)>& add_unknown = {}) {
        juce::DynamicObject::Ptr browser = new juce::DynamicObject;
        browser->setProperty("schema_version", 1);
        browser->setProperty("category_id", as_juce(category));
        if (subcategory)
            browser->setProperty("subcategory_id", as_juce(*subcategory));
        juce::Array<juce::var> tag_values;
        for (const auto& tag : tags)
            tag_values.add(as_juce(tag));
        browser->setProperty("tags", tag_values);
        browser->setProperty("description", as_juce(description));
        if (add_unknown)
            add_unknown(*browser);
        return std::string(",\"browser\":") + as_utf8(juce::JSON::toString(juce::var(browser.get()), false));
    };

    struct Observation {
        std::string case_id;
        bool registered = false;
        bool has_browser_classification = false;
        std::string category_id;
        std::optional<std::string> subcategory_id;
        std::vector<std::string> tags;
        std::string description;
        std::vector<std::string> diagnostic_codes;
        std::vector<std::string> diagnostic_messages;
        bool offline_task = false;
        bool raw_manifest_unchanged = false;
        std::size_t manifest_bytes = 0;
    };
    juce::Array<juce::var> observations;
    const auto observe_package = [&](const std::string& case_id,
                                     const std::string& browser_field) {
        const auto package = artifact_root / case_id;
        const auto plugin_id = "fixture.browser_boundary_" + case_id;
        const auto type_id = "fixture.boundary_" + case_id;
        write_python_browser_case_package(package, plugin_id, type_id, browser_field);
        const auto manifest_path = package / "manifest.json";
        std::ifstream before_file(manifest_path, std::ios::binary);
        const std::string before{std::istreambuf_iterator<char>(before_file),
                                 std::istreambuf_iterator<char>()};
        audioalgo::NodeRegistry registry;
        const auto discovery = audioalgo::load_native_plugins(registry, {package});
        const auto* descriptor = registry.find(type_id);
        Observation result;
        result.case_id = case_id;
        result.registered = descriptor != nullptr;
        result.manifest_bytes = before.size();
        result.raw_manifest_unchanged = std::filesystem::file_size(manifest_path) == before.size();
        std::ifstream after_file(manifest_path, std::ios::binary);
        const std::string after{std::istreambuf_iterator<char>(after_file),
                                std::istreambuf_iterator<char>()};
        result.raw_manifest_unchanged = result.raw_manifest_unchanged && after == before;
        if (descriptor != nullptr) {
            result.has_browser_classification = descriptor->browser_classification.has_value();
            if (descriptor->browser_classification) {
                const auto classification = audioalgo::resolve_node_browser_classification(
                    descriptor->browser_classification);
                result.category_id = classification.category_id;
                result.subcategory_id = classification.subcategory_id;
            }
            result.tags = descriptor->browser_tags;
            result.description = descriptor->browser_description;
            result.offline_task = descriptor->capabilities.requires_offline_task;
        }
        for (const auto& diagnostic : discovery.diagnostics) {
            result.diagnostic_codes.push_back(diagnostic.code);
            result.diagnostic_messages.push_back(diagnostic.message);
        }

        juce::DynamicObject::Ptr value = new juce::DynamicObject;
        value->setProperty("case_id", as_juce(result.case_id));
        value->setProperty("registered", result.registered);
        value->setProperty("browser_classification_present", result.has_browser_classification);
        value->setProperty("category_id", as_juce(result.category_id));
        value->setProperty("subcategory_id", result.subcategory_id ? juce::var(as_juce(*result.subcategory_id)) : juce::var{});
        juce::Array<juce::var> actual_tags;
        for (const auto& tag : result.tags)
            actual_tags.add(as_juce(tag));
        value->setProperty("tags", actual_tags);
        value->setProperty("description", as_juce(result.description));
        juce::Array<juce::var> diagnostic_codes;
        for (const auto& code : result.diagnostic_codes)
            diagnostic_codes.add(as_juce(code));
        value->setProperty("diagnostic_codes", diagnostic_codes);
        juce::Array<juce::var> diagnostic_messages;
        for (const auto& message : result.diagnostic_messages)
            diagnostic_messages.add(as_juce(message));
        value->setProperty("diagnostic_messages", diagnostic_messages);
        value->setProperty("offline_task", result.offline_task);
        value->setProperty("raw_manifest_unchanged", result.raw_manifest_unchanged);
        value->setProperty("manifest_bytes", static_cast<juce::int64>(result.manifest_bytes));
        observations.add(juce::var(value.get()));
        return result;
    };
    const auto expect_retained = [&](const Observation& result, const std::string& category,
                                    const std::optional<std::string>& subcategory,
                                    const std::vector<std::string>& tags,
                                    const std::string& description) {
        CHECK(result.registered);
        CHECK(result.diagnostic_codes.empty());
        CHECK(result.has_browser_classification);
        CHECK(result.category_id == category);
        CHECK(result.subcategory_id == subcategory);
        CHECK(result.tags == tags);
        CHECK(result.description == description);
        CHECK(result.offline_task);
        CHECK(result.raw_manifest_unchanged);
    };
    const auto expect_downgraded = [&](const Observation& result, const std::string& diagnostic_field) {
        CHECK(result.registered);
        CHECK(result.diagnostic_codes == std::vector<std::string>{"node_browser_metadata_invalid"});
        CHECK(result.diagnostic_messages.size() == 1);
        if (!result.diagnostic_messages.empty())
            CHECK(result.diagnostic_messages.front().find(diagnostic_field) != std::string::npos);
        CHECK_FALSE(result.has_browser_classification);
        CHECK(result.tags.empty());
        CHECK(result.description.empty());
        CHECK(result.offline_task);
        CHECK(result.raw_manifest_unchanged);
    };

    const auto keep_tags = std::vector<std::string>{"keep"};
    const auto category96 = std::string(classification_limit, 'c');
    REQUIRE(category96.size() == classification_limit);
    auto result = observe_package("category-96-ascii", serialize_browser(category96, {}, keep_tags, "category boundary"));
    expect_retained(result, "uncategorized", {}, keep_tags, "category boundary");
    const auto category97 = std::string(classification_limit + 1, 'c');
    result = observe_package("category-97-ascii", serialize_browser(category97, {}, keep_tags, "must downgrade"));
    expect_downgraded(result, "browser.category_id");
    const auto category_utf8_96 = repeated(scalar, classification_limit / scalar.size());
    REQUIRE(category_utf8_96.size() == classification_limit);
    result = observe_package("category-96-utf8", serialize_browser(category_utf8_96, {}, keep_tags, "UTF-8 category boundary"));
    expect_retained(result, "uncategorized", {}, keep_tags, "UTF-8 category boundary");
    const auto category_utf8_97 = category_utf8_96 + "a";
    REQUIRE(category_utf8_97.size() == classification_limit + 1);
    result = observe_package("category-97-utf8", serialize_browser(category_utf8_97, {}, keep_tags, "must downgrade"));
    expect_downgraded(result, "browser.category_id");

    const auto subcategory_utf8_96 = category_utf8_96;
    result = observe_package("subcategory-96-utf8",
        serialize_browser("audio.dynamics", subcategory_utf8_96, keep_tags, "subcategory boundary"));
    expect_retained(result, "audio.dynamics", {}, keep_tags, "subcategory boundary");
    const auto subcategory_utf8_97 = subcategory_utf8_96 + "a";
    result = observe_package("subcategory-97-utf8",
        serialize_browser("audio.dynamics", subcategory_utf8_97, keep_tags, "must downgrade"));
    expect_downgraded(result, "browser.subcategory_id");

    std::vector<std::string> tags32;
    for (std::size_t index = 0; index < tag_count_limit; ++index)
        tags32.push_back("tag" + std::to_string(index));
    result = observe_package("tags-32", serialize_browser("audio.dynamics", {}, tags32, "tag count boundary"));
    expect_retained(result, "audio.dynamics", {}, tags32, "tag count boundary");
    auto tags33 = tags32;
    tags33.push_back("tag32");
    result = observe_package("tags-33", serialize_browser("audio.dynamics", {}, tags33, "must downgrade"));
    expect_downgraded(result, "browser.tags");

    const auto tag64 = repeated(scalar, tag_byte_limit / scalar.size()) + "x";
    REQUIRE(tag64.size() == tag_byte_limit);
    result = observe_package("tag-64-utf8", serialize_browser("audio.dynamics", {}, {tag64}, "tag byte boundary"));
    expect_retained(result, "audio.dynamics", {}, {tag64}, "tag byte boundary");
    const auto tag65 = tag64 + "y";
    REQUIRE(tag65.size() == tag_byte_limit + 1);
    result = observe_package("tag-65-utf8", serialize_browser("audio.dynamics", {}, {tag65}, "must downgrade"));
    expect_downgraded(result, "browser.tags");

    const auto description2048 = repeated(scalar, description_limit / scalar.size()) + "ab";
    REQUIRE(description2048.size() == description_limit);
    result = observe_package("description-2048-utf8",
        serialize_browser("audio.dynamics", {}, keep_tags, description2048));
    expect_retained(result, "audio.dynamics", {}, keep_tags, description2048);
    const auto description2049 = description2048 + "c";
    REQUIRE(description2049.size() == description_limit + 1);
    result = observe_package("description-2049-utf8",
        serialize_browser("audio.dynamics", {}, keep_tags, description2049));
    expect_downgraded(result, "browser.description");

    const std::vector<std::string> duplicate_tags{"first", "second", "first", "third", "second"};
    result = observe_package("tag-dedup-stable-order",
        serialize_browser("audio.dynamics", {}, duplicate_tags, "deduplicate in first-seen order"));
    expect_retained(result, "audio.dynamics", {}, {"first", "second", "third"},
                    "deduplicate in first-seen order");
    result = observe_package("tag-empty",
        serialize_browser("audio.dynamics", {}, {"valid", ""}, "must downgrade"));
    expect_downgraded(result, "browser.tags");

    const auto unknown_field_root = artifact_root / "unknown-browser-field-preserved";
    const auto unknown_browser_field = serialize_browser("audio.dynamics", {}, keep_tags,
        "unknown browser fields survive loading", [](juce::DynamicObject& browser) {
            juce::DynamicObject::Ptr unknown = new juce::DynamicObject;
            unknown->setProperty("revision", 9);
            unknown->setProperty("opaque", "preserve-me");
            browser.setProperty("x_future_browser_field", juce::var(unknown.get()));
        });
    write_python_browser_case_package(unknown_field_root, "fixture.browser_boundary_unknown",
        "fixture.boundary_unknown", unknown_browser_field);
    std::ifstream unknown_before_stream(unknown_field_root / "manifest.json", std::ios::binary);
    const std::string unknown_before{std::istreambuf_iterator<char>(unknown_before_stream),
                                     std::istreambuf_iterator<char>()};
    CHECK(unknown_before.find("x_future_browser_field") != std::string::npos);
    audioalgo::NodeRegistry unknown_registry;
    const auto unknown_discovery = audioalgo::load_native_plugins(unknown_registry, {unknown_field_root});
    const auto* unknown_descriptor = unknown_registry.find("fixture.boundary_unknown");
    REQUIRE(unknown_descriptor != nullptr);
    CHECK(unknown_discovery.diagnostics.empty());
    CHECK(unknown_descriptor->browser_tags == keep_tags);
    std::ifstream unknown_after_stream(unknown_field_root / "manifest.json", std::ios::binary);
    const std::string unknown_after{std::istreambuf_iterator<char>(unknown_after_stream),
                                    std::istreambuf_iterator<char>()};
    CHECK(unknown_after == unknown_before);
    juce::DynamicObject::Ptr unknown_observation = new juce::DynamicObject;
    unknown_observation->setProperty("case_id", "unknown-browser-field-preserved");
    unknown_observation->setProperty("registered", true);
    unknown_observation->setProperty("raw_manifest_unchanged", unknown_after == unknown_before);
    unknown_observation->setProperty("unknown_field_present", unknown_after.find("x_future_browser_field") != std::string::npos);
    observations.add(juce::var(unknown_observation.get()));

    const auto make_padded_manifest = [&](std::size_t byte_count) {
        auto bytes = python_browser_case_manifest("fixture.browser_budget",
            "fixture.browser_budget", serialize_browser("audio.dynamics", {}, keep_tags, "package budget"));
        REQUIRE(bytes.size() < byte_count);
        bytes.append(byte_count - bytes.size(), ' ');
        return bytes;
    };
    const auto budget_manifest = make_padded_manifest(manifest_limit);
    const auto budget_root = artifact_root / "manifest-1048576";
    std::filesystem::create_directories(budget_root);
    {
        std::ofstream output(budget_root / "manifest.json", std::ios::binary);
        output.write(budget_manifest.data(), static_cast<std::streamsize>(budget_manifest.size()));
        output.close();
        REQUIRE(output.good());
    }
    CHECK(std::filesystem::file_size(budget_root / "manifest.json") == manifest_limit);
    audioalgo::NodeRegistry budget_registry;
    const auto budget_discovery = audioalgo::load_native_plugins(budget_registry, {budget_root});
    CHECK(budget_discovery.diagnostics.empty());
    CHECK(budget_registry.find("fixture.browser_budget") != nullptr);
    juce::DynamicObject::Ptr budget_observation = new juce::DynamicObject;
    budget_observation->setProperty("case_id", "manifest-1048576");
    budget_observation->setProperty("registered", budget_registry.find("fixture.browser_budget") != nullptr);
    budget_observation->setProperty("diagnostic_codes", juce::Array<juce::var>());
    budget_observation->setProperty("manifest_bytes", static_cast<juce::int64>(budget_manifest.size()));
    observations.add(juce::var(budget_observation.get()));

    const auto over_budget_manifest = make_padded_manifest(manifest_limit + 1);
    const auto over_budget_root = artifact_root / "manifest-1048577";
    std::filesystem::create_directories(over_budget_root);
    {
        std::ofstream output(over_budget_root / "manifest.json", std::ios::binary);
        output.write(over_budget_manifest.data(), static_cast<std::streamsize>(over_budget_manifest.size()));
        output.close();
        REQUIRE(output.good());
    }
    CHECK(std::filesystem::file_size(over_budget_root / "manifest.json") == manifest_limit + 1);
    audioalgo::NodeRegistry over_budget_registry;
    const auto over_budget_discovery = audioalgo::load_native_plugins(over_budget_registry, {over_budget_root});
    CHECK(over_budget_discovery.loaded_plugin_ids.empty());
    REQUIRE(over_budget_discovery.diagnostics.size() == 1);
    CHECK(over_budget_discovery.diagnostics.front().code == "plugin_manifest_invalid");
    CHECK(over_budget_registry.find("fixture.browser_budget") == nullptr);
    juce::DynamicObject::Ptr over_budget_observation = new juce::DynamicObject;
    over_budget_observation->setProperty("case_id", "manifest-1048577");
    over_budget_observation->setProperty("registered", over_budget_registry.find("fixture.browser_budget") != nullptr);
    juce::Array<juce::var> over_budget_diagnostics;
    for (const auto& diagnostic : over_budget_discovery.diagnostics)
        over_budget_diagnostics.add(as_juce(diagnostic.code));
    over_budget_observation->setProperty("diagnostic_codes", over_budget_diagnostics);
    over_budget_observation->setProperty("manifest_bytes", static_cast<juce::int64>(over_budget_manifest.size()));
    observations.add(juce::var(over_budget_observation.get()));

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("cases", observations);
    const auto receipt_path = artifact_root / "boundary-observations.json";
    const auto receipt_text = as_utf8(juce::JSON::toString(juce::var(receipt.get()), true));
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-03.a node library uses fixed taxonomy order and stable per-category entry ordering",
          "[NLC-03.a][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto expectation = load_library_order_expectation();
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);

    const auto* nodes = expectation.getProperty("nodes", {}).getArray();
    const auto* expected_rows = expectation.getProperty("expected_rows", {}).getArray();
    REQUIRE(nodes != nullptr);
    REQUIRE(expected_rows != nullptr);

    audioalgo::NodeRegistry registry;
    for (const auto& node : *nodes) {
        audioalgo::NodeDescriptor descriptor{
            node.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
            node.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        const auto category = node.getProperty("category_id", {}).toString().toStdString();
        const auto subcategory = node.getProperty("subcategory_id", {}).toString().toStdString();
        descriptor.browser_classification = audioalgo::NodeBrowserClassification{
            category, subcategory.empty() ? std::nullopt : std::optional<std::string>{subcategory}};
        REQUIRE(registry.register_descriptor(descriptor));
    }

    audioalgo::editor::NodeLibrary library(registry, [](std::string) {}, [] { return std::uint64_t{1}; });
    library.setSize(240, 620);
    auto* content = find_component(library, "library.content");
    CHECK(content != nullptr);

    struct DisplayRow {
        int y = 0;
        std::string kind;
        std::string id;
        std::string text;
    };
    std::vector<DisplayRow> actual_rows;
    std::size_t actual_node_buttons = 0;
    if (content != nullptr) {
        for (int index = 0; index < content->getNumChildComponents(); ++index) {
            auto* child = content->getChildComponent(index);
            const auto component_id = child->getComponentID().toStdString();
            if (auto* button = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(child)) {
                constexpr std::string_view prefix = "library.";
                REQUIRE(component_id.starts_with(prefix));
                actual_rows.push_back({child->getY(), "node", component_id.substr(prefix.size()),
                                       button->getButtonText().toStdString()});
                ++actual_node_buttons;
            } else if (auto* label = dynamic_cast<juce::Label*>(child)) {
                constexpr std::string_view category_prefix = "library.category.";
                constexpr std::string_view subcategory_prefix = "library.subcategory.";
                if (component_id.starts_with(category_prefix))
                    actual_rows.push_back({child->getY(), "category",
                        component_id.substr(category_prefix.size()), label->getText().toStdString()});
                else if (component_id.starts_with(subcategory_prefix))
                    actual_rows.push_back({child->getY(), "subcategory",
                        component_id.substr(subcategory_prefix.size()), label->getText().toStdString()});
            }
        }
    }
    std::sort(actual_rows.begin(), actual_rows.end(), [](const auto& left, const auto& right) {
        return left.y < right.y;
    });

    CHECK(actual_node_buttons == static_cast<std::size_t>(nodes->size()));
    CHECK(actual_rows.size() == static_cast<std::size_t>(expected_rows->size()));
    const auto comparable_rows = std::min(actual_rows.size(), static_cast<std::size_t>(expected_rows->size()));
    for (std::size_t index = 0; index < comparable_rows; ++index) {
        const auto& actual = actual_rows[index];
        const auto& expected = expected_rows->getReference(static_cast<int>(index));
        CHECK(actual.kind == expected.getProperty("kind", {}).toString().toStdString());
        CHECK(actual.id == expected.getProperty("id", {}).toString().toStdString());
        CHECK(actual.text == expected.getProperty("text", {}).toString().toStdString());
    }

    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc03a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC03A_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("fixture_node_count", static_cast<juce::int64>(nodes->size()));
    receipt->setProperty("node_button_count", static_cast<juce::int64>(actual_node_buttons));
    juce::Array<juce::var> observed_rows;
    const auto as_juce_string = [](const std::string& value) {
        return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
    };
    for (const auto& row : actual_rows) {
        juce::DynamicObject::Ptr item = new juce::DynamicObject;
        item->setProperty("kind", as_juce_string(row.kind));
        item->setProperty("id", as_juce_string(row.id));
        item->setProperty("text", as_juce_string(row.text));
        item->setProperty("y", row.y);
        observed_rows.add(juce::var(item.get()));
    }
    receipt->setProperty("rows", observed_rows);
    const auto receipt_path = artifact_root / "component-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-03.b category folding is view-only and pointer or keyboard can reach entries",
          "[NLC-03.b][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto expectation = load_library_collapse_expectation();
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);
    const auto* nodes = expectation.getProperty("nodes", {}).getArray();
    REQUIRE(nodes != nullptr);

    audioalgo::NodeRegistry registry;
    for (const auto& node : *nodes) {
        audioalgo::NodeDescriptor descriptor{
            node.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
            node.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        descriptor.browser_classification = audioalgo::NodeBrowserClassification{
            node.getProperty("category_id", {}).toString().toStdString(),
            node.getProperty("subcategory_id", {}).toString().toStdString()};
        REQUIRE(registry.register_descriptor(descriptor));
    }

    const auto category_id = expectation.getProperty("category_id", {}).toString();
    const auto subcategory_id = expectation.getProperty("subcategory_id", {}).toString();
    const auto keyboard_type_id = expectation.getProperty("keyboard_node_type_id", {}).toString().toStdString();
    const auto* component_ids = expectation.getProperty("expected_component_ids", {}).getDynamicObject();
    const auto* sizes = expectation.getProperty("component_sizes", {}).getArray();
    REQUIRE(component_ids != nullptr);
    REQUIRE(sizes != nullptr);

    audioalgo::ProjectModel project;
    project.project_id = "nlc03b-fold-view-only";
    project.name = "Existing graph survives library folding";
    project.revision = 19;
    project.selected_output_node_id = "existing-node";
    project.selected_output_port_id = "audio_out";
    project.serialized_extensions = {{"vendor_project", R"json({"retained":true})json"}};
    audioalgo::NodeInstance existing;
    existing.id = "existing-node";
    existing.type_id = "fixture.fold.file";
    existing.x = -42.5;
    existing.y = 91.25;
    existing.serialized_extensions = {{"vendor_node", R"json({"keep":"exact"})json"}};
    project.nodes.push_back(existing);
    const auto initial = project;

    juce::Array<juce::var> state_observations;
    juce::Array<juce::var> project_observations;
    juce::Array<juce::var> layout_observations;

    std::vector<std::string> activated_type_ids;
    std::vector<bool> add_results;
    const auto on_entry = [&](std::string type_id) {
        activated_type_ids.push_back(type_id);
        add_results.push_back(audioalgo::editor::try_add_node_from_descriptor(
            project, registry, type_id, "keyboard-added-node", 155.0, -28.0));
    };
    audioalgo::editor::NodeLibrary library(registry, on_entry, [] { return std::uint64_t{7}; });
    library.setSize(220, 620);

    auto* content = find_component(library, "library.content");
    REQUIRE(content != nullptr);
    auto* category = find_component(library, component_ids->getProperty("category").toString());
    auto* gain_subcategory = find_component(library, component_ids->getProperty("gain_subcategory").toString());
    auto* gain_entry = find_component(library, component_ids->getProperty("gain_entry").toString());
    auto* other_subcategory = find_component(library, component_ids->getProperty("other_subcategory").toString());
    auto* other_entry = find_component(library, component_ids->getProperty("other_entry").toString());
    REQUIRE(category != nullptr);
    REQUIRE(gain_subcategory != nullptr);
    REQUIRE(gain_entry != nullptr);
    REQUIRE(other_subcategory != nullptr);
    REQUIRE(other_entry != nullptr);

    const auto visible_component_ids = [&] {
        std::vector<std::string> ids;
        for (int index = 0; index < content->getNumChildComponents(); ++index) {
            auto* child = content->getChildComponent(index);
            if (child->isVisible())
                ids.push_back(child->getComponentID().toStdString());
        }
        return ids;
    };
    const auto record_state = [&](const std::string& state_id) {
        juce::DynamicObject::Ptr state = new juce::DynamicObject;
        state->setProperty("state_id", juce::String::fromUTF8(state_id.data(), static_cast<int>(state_id.size())));
        juce::Array<juce::var> visible;
        for (const auto& id : visible_component_ids())
            visible.add(juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
        state->setProperty("visible_component_ids", visible);
        state_observations.add(juce::var(state.get()));
        return visible_component_ids();
    };
    const auto expected_ids = [&](const char* key) {
        return string_array(expectation.getProperty(key, {}));
    };
    const auto record_project = [&](const std::string& state_id, const audioalgo::ProjectModel& value) {
        juce::DynamicObject::Ptr state = new juce::DynamicObject;
        state->setProperty("state_id", juce::String::fromUTF8(state_id.data(), static_cast<int>(state_id.size())));
        state->setProperty("schema_version", static_cast<int>(value.schema_version));
        state->setProperty("project_id", juce::String::fromUTF8(value.project_id.data(), static_cast<int>(value.project_id.size())));
        state->setProperty("name", juce::String::fromUTF8(value.name.data(), static_cast<int>(value.name.size())));
        state->setProperty("revision", static_cast<juce::int64>(value.revision));
        state->setProperty("selected_output_node_id", juce::String::fromUTF8(value.selected_output_node_id.data(), static_cast<int>(value.selected_output_node_id.size())));
        state->setProperty("selected_output_port_id", juce::String::fromUTF8(value.selected_output_port_id.data(), static_cast<int>(value.selected_output_port_id.size())));
        juce::Array<juce::var> nodes_observed;
        for (const auto& node : value.nodes) {
            juce::DynamicObject::Ptr item = new juce::DynamicObject;
            item->setProperty("id", juce::String::fromUTF8(node.id.data(), static_cast<int>(node.id.size())));
            item->setProperty("type_id", juce::String::fromUTF8(node.type_id.data(), static_cast<int>(node.type_id.size())));
            item->setProperty("x", node.x);
            item->setProperty("y", node.y);
            item->setProperty("parameter_count", static_cast<juce::int64>(node.parameters.size()));
            juce::Array<juce::var> extensions;
            for (const auto& extension : node.serialized_extensions) {
                juce::DynamicObject::Ptr field = new juce::DynamicObject;
                field->setProperty("name", juce::String::fromUTF8(extension.name.data(), static_cast<int>(extension.name.size())));
                field->setProperty("value_json", juce::String::fromUTF8(extension.value_json.data(), static_cast<int>(extension.value_json.size())));
                extensions.add(juce::var(field.get()));
            }
            item->setProperty("extensions", extensions);
            nodes_observed.add(juce::var(item.get()));
        }
        state->setProperty("nodes", nodes_observed);
        juce::Array<juce::var> connections_observed;
        for (const auto& connection : value.connections) {
            juce::DynamicObject::Ptr item = new juce::DynamicObject;
            item->setProperty("from_node", juce::String::fromUTF8(connection.from_node.data(), static_cast<int>(connection.from_node.size())));
            item->setProperty("from_port", juce::String::fromUTF8(connection.from_port.data(), static_cast<int>(connection.from_port.size())));
            item->setProperty("to_node", juce::String::fromUTF8(connection.to_node.data(), static_cast<int>(connection.to_node.size())));
            item->setProperty("to_port", juce::String::fromUTF8(connection.to_port.data(), static_cast<int>(connection.to_port.size())));
            connections_observed.add(juce::var(item.get()));
        }
        state->setProperty("connections", connections_observed);
        juce::Array<juce::var> project_extensions;
        for (const auto& extension : value.serialized_extensions) {
            juce::DynamicObject::Ptr field = new juce::DynamicObject;
            field->setProperty("name", juce::String::fromUTF8(extension.name.data(), static_cast<int>(extension.name.size())));
            field->setProperty("value_json", juce::String::fromUTF8(extension.value_json.data(), static_cast<int>(extension.value_json.size())));
            project_extensions.add(juce::var(field.get()));
        }
        state->setProperty("project_extensions", project_extensions);
        project_observations.add(juce::var(state.get()));
    };

    const auto same_project = [](const audioalgo::ProjectModel& actual,
                                 const audioalgo::ProjectModel& expected) {
        if (actual.schema_version != expected.schema_version || actual.project_id != expected.project_id ||
            actual.name != expected.name || actual.revision != expected.revision ||
            actual.selected_output_node_id != expected.selected_output_node_id ||
            actual.selected_output_port_id != expected.selected_output_port_id ||
            actual.serialized_extensions != expected.serialized_extensions ||
            actual.nodes.size() != expected.nodes.size() || actual.connections.size() != expected.connections.size())
            return false;
        for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
            const auto& left = actual.nodes[index];
            const auto& right = expected.nodes[index];
            if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
                left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
                left.serialized_extensions != right.serialized_extensions || left.managed_version != right.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < actual.connections.size(); ++index) {
            const auto& left = actual.connections[index];
            const auto& right = expected.connections[index];
            if (left.from_node != right.from_node || left.from_port != right.from_port ||
                left.to_node != right.to_node || left.to_port != right.to_port ||
                left.serialized_extensions != right.serialized_extensions)
                return false;
        }
        return true;
    };

    CHECK(category->getWantsKeyboardFocus());
    CHECK(gain_subcategory->getWantsKeyboardFocus());
    CHECK(gain_entry->getWantsKeyboardFocus());
    CHECK(category->isVisible());
    CHECK(gain_subcategory->isVisible());
    CHECK(gain_entry->isVisible());
    CHECK(other_subcategory->isVisible());
    CHECK(other_entry->isVisible());
    CHECK(visible_component_ids() == expected_ids("expected_initial_component_ids"));
    const auto initial_visible_ids = record_state("initial_expanded");
    CHECK(initial_visible_ids == expected_ids("expected_initial_component_ids"));
    record_project("before_folding", project);

    CHECK(category->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(category->isVisible());
    CHECK_FALSE(gain_subcategory->isVisible());
    CHECK_FALSE(gain_entry->isVisible());
    CHECK_FALSE(other_subcategory->isVisible());
    CHECK_FALSE(other_entry->isVisible());
    CHECK(visible_component_ids() == expected_ids("expected_category_collapsed_component_ids"));
    CHECK(record_state("category_collapsed") == expected_ids("expected_category_collapsed_component_ids"));
    CHECK(activated_type_ids.empty());
    CHECK(same_project(project, initial));
    record_project("after_category_folding", project);

    const auto click_component = [](juce::Component& target) {
        const auto now = juce::Time::getCurrentTime();
        const juce::Point<float> position(8.0f, 10.0f);
        const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), position,
            juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            &target, &target, now, position, now, 1, false);
        target.mouseDown(event);
        target.mouseUp(event);
    };
    click_component(*category);
    CHECK(gain_subcategory->isVisible());
    CHECK(gain_entry->isVisible());
    CHECK(other_subcategory->isVisible());
    CHECK(other_entry->isVisible());
    CHECK(record_state("category_mouse_expanded") == expected_ids("expected_initial_component_ids"));

    CHECK(gain_subcategory->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK_FALSE(gain_entry->isVisible());
    CHECK(other_entry->isVisible());
    CHECK(record_state("gain_subcategory_collapsed") == expected_ids("expected_gain_collapsed_component_ids"));
    CHECK(activated_type_ids.empty());
    CHECK(same_project(project, initial));
    click_component(*gain_subcategory);
    CHECK(gain_entry->isVisible());
    CHECK(record_state("gain_subcategory_mouse_expanded") == expected_ids("expected_initial_component_ids"));

    auto* gain_button = dynamic_cast<juce::Button*>(gain_entry);
    REQUIRE(gain_button != nullptr);
    CHECK(gain_button->getWantsKeyboardFocus());
    CHECK(static_cast<juce::Component*>(gain_button)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* message_manager = juce::MessageManager::getInstance();
    REQUIRE(message_manager != nullptr);
    const auto activation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (activated_type_ids.empty() && std::chrono::steady_clock::now() < activation_deadline)
        message_manager->runDispatchLoopUntil(10);
    CHECK(activated_type_ids == std::vector<std::string>{keyboard_type_id});
    REQUIRE(add_results == std::vector<bool>{true});
    REQUIRE(project.nodes.size() == initial.nodes.size() + 1);
    CHECK(project.nodes.back().id == "keyboard-added-node");
    CHECK(project.nodes.back().type_id == keyboard_type_id);
    record_project("after_keyboard_entry_activation", project);

    const auto after_entry_activation = project;
    CHECK(category->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK_FALSE(gain_subcategory->isVisible());
    CHECK(same_project(project, after_entry_activation));
    CHECK(activated_type_ids.size() == 1);
    CHECK(record_state("fold_after_entry_activation") == expected_ids("expected_category_collapsed_component_ids"));
    CHECK(category->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(record_state("expanded_for_layout") == expected_ids("expected_initial_component_ids"));
    CHECK(static_cast<int>(sizes->size()) == 3);
    for (const auto& size : *sizes) {
        const auto width = static_cast<int>(size.getProperty("width", {}));
        const auto height = static_cast<int>(size.getProperty("height", {}));
        library.setSize(width, height);
        content = find_component(library, "library.content");
        REQUIRE(content != nullptr);
        category = find_component(library, component_ids->getProperty("category").toString());
        REQUIRE(category != nullptr);
        CHECK(category->isVisible());
        CHECK(category->getWidth() > 0);
        CHECK(category->getX() >= 0);
        CHECK(category->getRight() <= content->getWidth());
        int previous_bottom = -1;
        juce::DynamicObject::Ptr layout = new juce::DynamicObject;
        layout->setProperty("width", width);
        layout->setProperty("height", height);
        layout->setProperty("content_width", content->getWidth());
        layout->setProperty("content_height", content->getHeight());
        juce::Array<juce::var> layout_rows;
        for (int index = 0; index < content->getNumChildComponents(); ++index) {
            auto* child = content->getChildComponent(index);
            if (!child->isVisible())
                continue;
            CHECK(child->getHeight() > 0);
            CHECK(child->getY() >= previous_bottom);
            CHECK(child->getX() >= 0);
            CHECK(child->getRight() <= content->getWidth());
            CHECK(child->getBottom() <= content->getHeight());
            previous_bottom = child->getBottom();
            juce::DynamicObject::Ptr observed = new juce::DynamicObject;
            observed->setProperty("component_id", child->getComponentID());
            observed->setProperty("x", child->getX());
            observed->setProperty("y", child->getY());
            observed->setProperty("width", child->getWidth());
            observed->setProperty("height", child->getHeight());
            layout_rows.add(juce::var(observed.get()));
        }
        layout->setProperty("rows", layout_rows);
        layout_observations.add(juce::var(layout.get()));
        CHECK(same_project(project, after_entry_activation));
    }

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-03.b");
    receipt->setProperty("states", state_observations);
    receipt->setProperty("projects", project_observations);
    receipt->setProperty("responsive_layouts", layout_observations);
    juce::Array<juce::var> activated;
    for (const auto& type_id : activated_type_ids)
        activated.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
    receipt->setProperty("activated_type_ids", activated);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc03b/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC03B_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    const auto receipt_path = artifact_root / "component-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-03.c production library shows one current default per type and counts active results",
          "[NLC-03.c][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto expectation = load_library_defaults_expectation();
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);
    const auto* default_types = expectation.getProperty("default_types", {}).getArray();
    const auto* old_lock = expectation.getProperty("old_project_lock", {}).getDynamicObject();
    REQUIRE(default_types != nullptr);
    REQUIRE(old_lock != nullptr);

    const auto type_id = expectation.getProperty("default_types", {}).getArray()->getReference(0)
                             .getProperty("type_id", {}).toString().toStdString();
    const auto version = expectation.getProperty("default_types", {}).getArray()->getReference(0)
                             .getProperty("default_version", {}).toString().toStdString();
    const auto default_hash = expectation.getProperty("default_types", {}).getArray()->getReference(0)
                                  .getProperty("managed_content_sha256", {}).toString().toStdString();

    const auto populate_registry = [&](audioalgo::NodeRegistry& registry, bool renamed,
                                       bool filtered) {
        const auto* filtered_ids = expectation.getProperty("filtered_effective_type_ids", {}).getArray();
        for (const auto& item : *default_types) {
            const auto id = item.getProperty("type_id", {}).toString().toStdString();
            if (filtered && std::none_of(filtered_ids->begin(), filtered_ids->end(), [&](const auto& value) {
                    return value.toString().toStdString() == id;
                }))
                continue;
            const auto display = item.getProperty(renamed ? "renamed_display_name" : "display_name", {})
                                     .toString().toStdString();
            audioalgo::NodeDescriptor descriptor{
                id,
                item.getProperty("default_version", {}).toString().toStdString(),
                display,
                {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
                 {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
                {}, audioalgo::NodeExecutionKind::block};
            descriptor.browser_classification = audioalgo::NodeBrowserClassification{
                item.getProperty("category_id", {}).toString().toStdString(),
                item.getProperty("subcategory_id", {}).toString().toStdString()};
            const auto plugin_id = item.getProperty("plugin_id", {}).toString().toStdString();
            if (!plugin_id.empty()) {
                audioalgo::NodeDescriptor::PluginOrigin origin;
                origin.plugin_id = plugin_id;
                origin.version = item.getProperty("default_version", {}).toString().toStdString();
                origin.runtime = renamed ? "cpp" : item.getProperty("runtime", {}).toString().toStdString();
                origin.managed_content_sha256 = item.getProperty("managed_content_sha256", {}).toString().toStdString();
                origin.managed_package_path = item.getProperty("managed_package_path", {}).toString().toStdString();
                origin.managed_wrapper_version = item.getProperty("managed_wrapper_version", {}).toString().toStdString();
                descriptor.plugin_origin = std::move(origin);
            }
            REQUIRE(registry.register_descriptor(std::move(descriptor)));
        }
    };

    audioalgo::NodeRegistry current_defaults;
    populate_registry(current_defaults, false, false);
    const auto entries = audioalgo::editor::build_node_library_entries(current_defaults);
    const auto expected_entry_ids = string_array(expectation.getProperty("expected_default_entry_ids", {}));
    REQUIRE(expected_entry_ids.size() == default_types->size());
    REQUIRE(entries.size() == expected_entry_ids.size());
    std::vector<std::string> entry_ids;
    for (const auto& entry : entries)
        entry_ids.push_back("library." + entry.type_id);
    std::sort(entry_ids.begin(), entry_ids.end());
    auto sorted_expected_ids = expected_entry_ids;
    std::sort(sorted_expected_ids.begin(), sorted_expected_ids.end());
    CHECK(entry_ids == sorted_expected_ids);
    const auto* active_default = current_defaults.find(type_id);
    REQUIRE(active_default != nullptr);
    CHECK(active_default->version == version);
    REQUIRE(active_default->plugin_origin.has_value());
    CHECK(active_default->plugin_origin->managed_content_sha256 == default_hash);
    for (const auto& candidate : *default_types) {
        const auto candidate_id = candidate.getProperty("type_id", {}).toString().toStdString();
        const auto* active = current_defaults.find(candidate_id);
        REQUIRE(active != nullptr);
        CHECK(active->version == candidate.getProperty("default_version", {}).toString().toStdString());
        const auto non_default_versions = string_array(candidate.getProperty("non_default_versions", {}));
        CHECK(std::find(non_default_versions.begin(), non_default_versions.end(), active->version) ==
              non_default_versions.end());
        CHECK(std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
                  return entry.type_id == candidate_id;
              }) == 1);
    }

    audioalgo::ProjectModel old_project;
    old_project.schema_version = 2;
    old_project.project_id = "nlc03c-locked-project";
    old_project.name = "Keep the exact old managed package";
    old_project.revision = 73;
    old_project.selected_output_node_id = "locked-old-gain";
    old_project.selected_output_port_id = "audio_out";
    old_project.serialized_extensions = {{"vendor_project", R"json({"retain":"old"})json"}};
    audioalgo::NodeInstance old_node;
    old_node.id = old_lock->getProperty("node_id").toString().toStdString();
    old_node.type_id = old_lock->getProperty("type_id").toString().toStdString();
    old_node.parameters.emplace("gain", 0.375);
    old_node.x = -61.25;
    old_node.y = 42.5;
    old_node.serialized_extensions = {{"vendor_node", R"json({"retain":[1,2,3]})json"}};
    old_node.managed_version = audioalgo::NodeInstance::ManagedVersion{
        old_lock->getProperty("package_id").toString().toStdString(),
        old_lock->getProperty("content_sha256").toString().toStdString(),
        old_lock->getProperty("package_path").toString().toStdString(),
        old_lock->getProperty("wrapper_version").toString().toStdString(),
        {{"vendor_lock", R"json({"exact":true})json"}}};
    old_project.nodes.push_back(old_node);
    const auto old_project_before = old_project;
    const auto old_project_is_unchanged = [&] {
        if (old_project.schema_version != old_project_before.schema_version ||
            old_project.project_id != old_project_before.project_id || old_project.name != old_project_before.name ||
            old_project.revision != old_project_before.revision ||
            old_project.selected_output_node_id != old_project_before.selected_output_node_id ||
            old_project.selected_output_port_id != old_project_before.selected_output_port_id ||
            old_project.serialized_extensions != old_project_before.serialized_extensions ||
            old_project.connections.size() != old_project_before.connections.size() ||
            old_project.nodes.size() != old_project_before.nodes.size())
            return false;
        if (old_project.nodes.empty())
            return false;
        const auto& actual = old_project.nodes.front();
        const auto& expected = old_project_before.nodes.front();
        return actual.id == expected.id && actual.type_id == expected.type_id &&
               actual.parameters == expected.parameters && actual.x == expected.x && actual.y == expected.y &&
               actual.opaque_parameters == expected.opaque_parameters &&
               actual.serialized_extensions == expected.serialized_extensions &&
               actual.managed_version == expected.managed_version;
    };

    audioalgo::ProjectModel newly_created;
    newly_created.project_id = "nlc03c-default-project";
    newly_created.name = "Create from the current default";
    const audioalgo::NodeCatalog* active_catalog = &current_defaults;
    std::vector<std::string> activated_type_ids;
    audioalgo::editor::NodeLibrary library(current_defaults,
        [&](std::string activated) {
            activated_type_ids.push_back(activated);
            (void)audioalgo::editor::try_add_node_from_descriptor(
                newly_created, *active_catalog, activated, "current-default-node", 120.0, 80.0);
        }, [] { return std::uint64_t{11}; });
    library.setSize(260, 640);

    auto* content = find_component(library, "library.content");
    REQUIRE(content != nullptr);
    const auto count_description = [&](const std::string& component_id) {
        auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
            find_component(library, component_id));
        return heading == nullptr ? juce::String{} : heading->getDescription();
    };
    const auto expected_count = [&](const juce::var& counts, const std::string& key) {
        const auto* object = counts.getDynamicObject();
        REQUIRE(object != nullptr);
        return static_cast<int>(object->getProperty(juce::String::fromUTF8(key.data(), static_cast<int>(key.size()))));
    };
    const auto expects_count = [&](const std::string& component_id, int expected) {
        CHECK(count_description(component_id).contains(
            juce::String(expected) + (expected == 1 ? " entry" : " entries")));
    };
    const auto all_counts = expectation.getProperty("expected_counts_all", {});
    expects_count("library.category.audio.dynamics", expected_count(all_counts, "audio.dynamics"));
    expects_count("library.subcategory.audio.dynamics.gain", expected_count(all_counts, "audio.dynamics.gain"));
    expects_count("library.subcategory.audio.dynamics.compressor", expected_count(all_counts, "audio.dynamics.compressor"));
    expects_count("library.category.audio.io", expected_count(all_counts, "audio.io"));
    juce::Array<juce::var> count_states;
    const auto record_count_state = [&](const char* state_id) {
        juce::DynamicObject::Ptr counts = new juce::DynamicObject;
        counts->setProperty("state_id", state_id);
        for (const auto& component : {std::pair{"audio.io", "library.category.audio.io"},
                                      std::pair{"audio.dynamics", "library.category.audio.dynamics"},
                                      std::pair{"audio.dynamics.gain", "library.subcategory.audio.dynamics.gain"},
                                      std::pair{"audio.dynamics.compressor", "library.subcategory.audio.dynamics.compressor"}}) {
            auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
                find_component(library, component.second));
            if (heading != nullptr)
                counts->setProperty(component.first, heading->getDescription());
        }
        count_states.add(juce::var(counts.get()));
    };
    record_count_state("all_current_defaults");

    const auto entry_component_id = juce::String("library.") + juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size()));
    auto* default_button = dynamic_cast<juce::TextButton*>(find_component(library, entry_component_id));
    REQUIRE(default_button != nullptr);
    CHECK(default_button->getButtonText() == "Gain Current");
    int matching_entries = 0;
    for (int index = 0; index < content->getNumChildComponents(); ++index)
        if (content->getChildComponent(index)->getComponentID() == entry_component_id)
            ++matching_entries;
    CHECK(matching_entries == 1);
    CHECK(old_project_is_unchanged());

    audioalgo::NodeRegistry renamed_defaults;
    populate_registry(renamed_defaults, true, false);
    active_catalog = &renamed_defaults;
    library.refresh(renamed_defaults);
    content = find_component(library, "library.content");
    REQUIRE(content != nullptr);
    auto* renamed_button = dynamic_cast<juce::TextButton*>(find_component(library, entry_component_id));
    REQUIRE(renamed_button != nullptr);
    CHECK(renamed_button->getButtonText() == juce::String::fromUTF8("\xE5\xA2\x9E\xE7\x9B\x8A \xF0\x9F\x8E\x9A"));
    CHECK(renamed_button->getComponentID() == entry_component_id);
    const auto renamed_display_name = renamed_button->getButtonText();
    const auto renamed_component_id = renamed_button->getComponentID();
    CHECK(old_project_is_unchanged());
    const auto renamed_entries = audioalgo::editor::build_node_library_entries(renamed_defaults);
    const auto renamed_entry = std::find_if(renamed_entries.begin(), renamed_entries.end(), [&](const auto& entry) {
        return entry.type_id == type_id;
    });
    REQUIRE(renamed_entry != renamed_entries.end());
    const auto initial_entry = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.type_id == type_id;
    });
    REQUIRE(initial_entry != entries.end());
    CHECK(initial_entry->type_id == renamed_entry->type_id);
    CHECK(initial_entry->source_id == renamed_entry->source_id);
    CHECK(initial_entry->language_id == "python");
    CHECK(renamed_entry->language_id == "cpp");
    CHECK(renamed_entry->display_name == juce::String::fromUTF8("\xE5\xA2\x9E\xE7\x9B\x8A \xF0\x9F\x8E\x9A").toStdString());
    CHECK(renamed_button->isEnabled());
    renamed_button->triggerClick();
    auto* message_manager = juce::MessageManager::getInstance();
    REQUIRE(message_manager != nullptr);
    const auto activation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (activated_type_ids.empty() && std::chrono::steady_clock::now() < activation_deadline)
        message_manager->runDispatchLoopUntil(10);
    CHECK(activated_type_ids == std::vector<std::string>{type_id});
    REQUIRE(newly_created.nodes.size() == 1);
    CHECK(newly_created.nodes.front().type_id == type_id);
    REQUIRE(newly_created.nodes.front().managed_version.has_value());
    CHECK(newly_created.nodes.front().managed_version->package_id == "fixture.managed.audio");
    CHECK(newly_created.nodes.front().managed_version->content_sha256 == default_hash);
    CHECK(newly_created.nodes.front().managed_version->package_path == "managed/fixture.managed.audio/2.4.1");
    CHECK(newly_created.nodes.front().managed_version->wrapper_version == "python-wrapper-7");
    CHECK(old_project.nodes.front().managed_version->content_sha256 == old_lock->getProperty("content_sha256").toString().toStdString());

    audioalgo::NodeRegistry filtered_results;
    populate_registry(filtered_results, false, true);
    library.refresh(filtered_results);
    content = find_component(library, "library.content");
    REQUIRE(content != nullptr);
    const auto filtered_counts = expectation.getProperty("expected_counts_filtered", {});
    expects_count("library.category.audio.dynamics", expected_count(filtered_counts, "audio.dynamics"));
    expects_count("library.subcategory.audio.dynamics.gain", expected_count(filtered_counts, "audio.dynamics.gain"));
    CHECK(find_component(library, "library.category.audio.io") == nullptr);
    CHECK(find_component(library, entry_component_id) != nullptr);
    CHECK(find_component(library, "library.fixture.builtin.compressor") == nullptr);
    record_count_state("filtered_current_results");
    CHECK(old_project_is_unchanged());

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-03.c");
    juce::Array<juce::var> expected_default_ids;
    for (const auto& id : expected_entry_ids)
        expected_default_ids.add(juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
    receipt->setProperty("expected_default_entry_ids", expected_default_ids);
    juce::Array<juce::var> defaults_observed;
    for (const auto& id : entry_ids)
        defaults_observed.add(juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
    receipt->setProperty("default_entry_ids", defaults_observed);
    receipt->setProperty("default_type_id", juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
    receipt->setProperty("default_version", juce::String::fromUTF8(version.data(), static_cast<int>(version.size())));
    receipt->setProperty("default_display_name", "Gain Current");
    receipt->setProperty("renamed_display_name", renamed_display_name);
    receipt->setProperty("stable_component_id", renamed_component_id);
    juce::Array<juce::var> activated_ids;
    for (const auto& activated : activated_type_ids)
        activated_ids.add(juce::String::fromUTF8(activated.data(), static_cast<int>(activated.size())));
    receipt->setProperty("activated_type_ids", activated_ids);
    const auto old_lock_hash = old_project.nodes.front().managed_version->content_sha256;
    const auto created_lock_hash = newly_created.nodes.front().managed_version->content_sha256;
    receipt->setProperty("old_project_lock_sha256",
        juce::String::fromUTF8(old_lock_hash.data(), static_cast<int>(old_lock_hash.size())));
    receipt->setProperty("created_project_lock_sha256",
        juce::String::fromUTF8(created_lock_hash.data(), static_cast<int>(created_lock_hash.size())));
    receipt->setProperty("count_states", count_states);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc03c/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    const auto receipt_path = artifact_root / "component-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-04.a production node library searches fixed metadata fields with AND and ASCII case folding",
          "[NLC-04.a][node_library][search]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto expectation = load_library_search_expectation();
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);
    const auto* expected_nodes = expectation.getProperty("nodes", {}).getArray();
    const auto* expected_queries = expectation.getProperty("queries", {}).getArray();
    REQUIRE(expected_nodes != nullptr);
    REQUIRE(expected_queries != nullptr);

    audioalgo::NodeRegistry registry;
    const auto taxonomy = audioalgo::node_browser_taxonomy();
    for (const auto& expected : *expected_nodes) {
        const auto type_id = expected.getProperty("type_id", {}).toString().toStdString();
        const auto category_id = expected.getProperty("category_id", {}).toString().toStdString();
        const auto subcategory_id = expected.getProperty("subcategory_id", {}).toString().toStdString();
        const auto category = std::find_if(taxonomy.begin(), taxonomy.end(), [&](const auto& item) {
            return item.id == category_id;
        });
        REQUIRE(category != taxonomy.end());
        CHECK(std::string(category->display_name) ==
              expected.getProperty("category_display_name", {}).toString().toStdString());

        audioalgo::NodeDescriptor descriptor{
            type_id,
            expected.getProperty("version", {}).toString().toStdString(),
            expected.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        descriptor.browser_classification = audioalgo::NodeBrowserClassification{category_id, subcategory_id};
        descriptor.browser_description = expected.getProperty("description", {}).toString().toStdString();
        descriptor.browser_tags = string_array(expected.getProperty("tags", {}));
        REQUIRE(registry.register_descriptor(descriptor));
    }

    audioalgo::editor::NodeLibrary library(registry, [](std::string) {}, [] { return std::uint64_t{3}; });
    library.setSize(300, 800);
    auto* search = dynamic_cast<juce::TextEditor*>(find_component(library, "library.search"));
    CHECK(search != nullptr);

    const auto visible_entry_ids = [&] {
        std::vector<std::string> ids;
        for (const auto& expected : *expected_nodes) {
            const auto type_id = expected.getProperty("type_id", {}).toString().toStdString();
            const auto component_id = "library." + type_id;
            auto* button = dynamic_cast<juce::Button*>(find_component(library, component_id));
            if (button != nullptr && button->isVisible())
                ids.push_back(type_id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };

    auto all_type_ids = string_array(expectation.getProperty("all_type_ids", {}));
    std::sort(all_type_ids.begin(), all_type_ids.end());
    CHECK(visible_entry_ids() == all_type_ids);
    const auto assert_matching_counts = [&](const std::vector<std::string>& expected_type_ids) {
        const auto expected_set = std::set<std::string>(expected_type_ids.begin(), expected_type_ids.end());
        const auto expected_description = [](std::string_view group, std::size_t count) {
            return juce::String(group.data(), static_cast<int>(group.size())) + "; " +
                   juce::String(static_cast<int>(count)) + (count == 1 ? " entry" : " entries") +
                   "; expanded; press Enter or Space to collapse";
        };
        std::set<std::string> categories;
        std::set<std::pair<std::string, std::string>> subcategories;
        for (const auto& expected : *expected_nodes) {
            const auto category_id = expected.getProperty("category_id", {}).toString().toStdString();
            const auto subcategory_id = expected.getProperty("subcategory_id", {}).toString().toStdString();
            categories.insert(category_id);
            subcategories.emplace(category_id, subcategory_id);
        }
        for (const auto& category_id : categories) {
            const auto expected_count = static_cast<std::size_t>(std::count_if(
                expected_nodes->begin(), expected_nodes->end(), [&](const juce::var& expected) {
                    return expected.getProperty("category_id", {}).toString().toStdString() == category_id &&
                           expected_set.contains(expected.getProperty("type_id", {}).toString().toStdString());
                }));
            auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
                find_component(library, "library.category." + juce::String::fromUTF8(
                    category_id.data(), static_cast<int>(category_id.size()))));
            REQUIRE(heading != nullptr);
            CHECK(heading->getDescription() == expected_description("category", expected_count));
        }
        for (const auto& [category_id, subcategory_id] : subcategories) {
            const auto expected_count = static_cast<std::size_t>(std::count_if(
                expected_nodes->begin(), expected_nodes->end(), [&](const juce::var& expected) {
                    return expected.getProperty("category_id", {}).toString().toStdString() == category_id &&
                           expected.getProperty("subcategory_id", {}).toString().toStdString() == subcategory_id &&
                           expected_set.contains(expected.getProperty("type_id", {}).toString().toStdString());
                }));
            const auto component_id = "library.subcategory." +
                juce::String::fromUTF8(category_id.data(), static_cast<int>(category_id.size())) + "." +
                juce::String::fromUTF8(subcategory_id.data(), static_cast<int>(subcategory_id.size()));
            auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
                find_component(library, component_id));
            REQUIRE(heading != nullptr);
            CHECK(heading->getDescription() == expected_description("subcategory", expected_count));
        }
    };
    juce::Array<juce::var> observations;
    const auto record_observation = [&](const juce::String& query_id, const juce::String& query) {
        juce::DynamicObject::Ptr observation = new juce::DynamicObject;
        observation->setProperty("query_id", query_id);
        observation->setProperty("query", query);
        juce::Array<juce::var> observed_ids;
        for (const auto& id : visible_entry_ids())
            observed_ids.add(juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
        observation->setProperty("visible_type_ids", observed_ids);

        juce::DynamicObject::Ptr group_descriptions = new juce::DynamicObject;
        std::set<std::string> categories;
        std::set<std::pair<std::string, std::string>> subcategories;
        for (const auto& expected : *expected_nodes) {
            const auto category_id = expected.getProperty("category_id", {}).toString().toStdString();
            const auto subcategory_id = expected.getProperty("subcategory_id", {}).toString().toStdString();
            categories.insert(category_id);
            subcategories.emplace(category_id, subcategory_id);
        }
        for (const auto& category_id : categories) {
            const auto component_id = "library.category." + juce::String::fromUTF8(
                category_id.data(), static_cast<int>(category_id.size()));
            if (auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
                    find_component(library, component_id)))
                group_descriptions->setProperty("category:" + component_id.fromFirstOccurrenceOf(
                    "library.category.", false, false), heading->getDescription());
        }
        for (const auto& [category_id, subcategory_id] : subcategories) {
            const auto component_id = "library.subcategory." +
                juce::String::fromUTF8(category_id.data(), static_cast<int>(category_id.size())) + "." +
                juce::String::fromUTF8(subcategory_id.data(), static_cast<int>(subcategory_id.size()));
            if (auto* heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
                    find_component(library, component_id)))
                group_descriptions->setProperty(
                    "subcategory:" + juce::String::fromUTF8(category_id.data(), static_cast<int>(category_id.size())) +
                        "." + juce::String::fromUTF8(subcategory_id.data(), static_cast<int>(subcategory_id.size())),
                    heading->getDescription());
        }
        observation->setProperty("group_descriptions", juce::var(group_descriptions.get()));
        observations.add(juce::var(observation.get()));
    };
    assert_matching_counts(all_type_ids);
    record_observation("__empty", {});
    for (const auto& query_case : *expected_queries) {
        const auto query_id = query_case.getProperty("id", {}).toString();
        const auto query = query_case.getProperty("query", {}).toString();
        INFO("query case " << query_id << ": " << query);
        if (search != nullptr) {
            search->setText(query, true);
            if (auto* message_manager = juce::MessageManager::getInstance())
                message_manager->runDispatchLoopUntil(10);
        }
        auto expected_type_ids = string_array(query_case.getProperty("expected_type_ids", {}));
        std::sort(expected_type_ids.begin(), expected_type_ids.end());
        CHECK(visible_entry_ids() == expected_type_ids);
        assert_matching_counts(expected_type_ids);
        record_observation(query_id, query);
    }

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-04.a");
    receipt->setProperty("observations", observations);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc04a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    const auto receipt_path = artifact_root / "component-search-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-04.b search reveals collapsed nodes, restores folding, and remains view-only",
          "[NLC-04.b][node_library][search]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto search_expectation = load_library_search_expectation();
    REQUIRE(search_expectation.isObject());
    const auto* expected_nodes = search_expectation.getProperty("nodes", {}).getArray();
    REQUIRE(expected_nodes != nullptr);
    const auto view_expectation_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_browser/library_search_view_v1.json";
    std::ifstream view_expectation_file(view_expectation_path, std::ios::binary);
    REQUIRE(view_expectation_file.good());
    const std::string view_expectation_bytes{
        std::istreambuf_iterator<char>(view_expectation_file), std::istreambuf_iterator<char>()};
    const auto view_expectation = juce::JSON::parse(
        juce::String::fromUTF8(view_expectation_bytes.data(), static_cast<int>(view_expectation_bytes.size())));
    REQUIRE(view_expectation.isObject());

    audioalgo::NodeRegistry registry;
    for (const auto& expected : *expected_nodes) {
        const auto type_id = expected.getProperty("type_id", {}).toString().toStdString();
        const auto category_id = expected.getProperty("category_id", {}).toString().toStdString();
        const auto subcategory_id = expected.getProperty("subcategory_id", {}).toString().toStdString();
        audioalgo::NodeDescriptor descriptor{
            type_id,
            expected.getProperty("version", {}).toString().toStdString(),
            expected.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        descriptor.browser_classification = audioalgo::NodeBrowserClassification{category_id, subcategory_id};
        descriptor.browser_description = expected.getProperty("description", {}).toString().toStdString();
        descriptor.browser_tags = string_array(expected.getProperty("tags", {}));
        REQUIRE(registry.register_descriptor(descriptor));
    }

    audioalgo::ProjectModel project;
    project.project_id = "nlc04b-search-view-only";
    project.name = "Search must not edit this graph";
    project.revision = 23;
    project.serialized_extensions = {{"vendor_project", R"json({"keep":true})json"}};
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    controller.mark_saved();
    controller.mark_result_current();

    std::vector<std::string> activated_type_ids;
    audioalgo::editor::NodeLibrary library(registry, [&](std::string type_id) {
        activated_type_ids.push_back(std::move(type_id));
    }, [] { return std::uint64_t{11}; });
    library.setSize(300, 760);

    auto* search = dynamic_cast<juce::TextEditor*>(find_component(library, "library.search"));
    auto* clear_search = dynamic_cast<juce::Button*>(find_component(library, "library.search.clear"));
    auto* no_results = dynamic_cast<juce::Label*>(find_component(library, "library.search.no_results"));
    REQUIRE(search != nullptr);
    CHECK(clear_search != nullptr);
    CHECK(no_results != nullptr);

    const auto category_component_id = view_expectation.getProperty("collapsed_category_component_id", {}).toString();
    const auto category_heading_component_id = view_expectation.getProperty("category_heading_component_id", {}).toString();
    const auto subcategory_component_id = view_expectation.getProperty("collapsed_subcategory_component_id", {}).toString();
    const auto subcategory_heading_component_id = view_expectation.getProperty("subcategory_heading_component_id", {}).toString();
    auto* category_to_collapse = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
        find_component(library, category_component_id));
    auto* subcategory_to_collapse = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
        find_component(library, subcategory_component_id));
    auto* category_heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
        find_component(library, category_heading_component_id));
    auto* subcategory_heading = dynamic_cast<audioalgo::editor::NodeLibraryHeading*>(
        find_component(library, subcategory_heading_component_id));
    REQUIRE(category_to_collapse != nullptr);
    REQUIRE(subcategory_to_collapse != nullptr);
    REQUIRE(category_heading != nullptr);
    REQUIRE(subcategory_heading != nullptr);

    const auto visible_type_ids = [&] {
        std::vector<std::string> result;
        for (const auto& expected : *expected_nodes) {
            const auto type_id = expected.getProperty("type_id", {}).toString().toStdString();
            auto* button = dynamic_cast<juce::Button*>(
                find_component(library, juce::String("library.") + juce::String::fromUTF8(
                    type_id.data(), static_cast<int>(type_id.size()))));
            if (button != nullptr && button->isVisible())
                result.push_back(type_id);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    const auto set_query = [&](const juce::String& query) {
        search->setText(query, true);
        if (auto* manager = juce::MessageManager::getInstance())
            manager->runDispatchLoopUntil(10);
    };
    const auto same_project = [](const audioalgo::ProjectModel& actual,
                                 const audioalgo::ProjectModel& expected) {
        if (actual.schema_version != expected.schema_version || actual.project_id != expected.project_id ||
            actual.name != expected.name || actual.revision != expected.revision ||
            actual.selected_output_node_id != expected.selected_output_node_id ||
            actual.selected_output_port_id != expected.selected_output_port_id ||
            actual.serialized_extensions != expected.serialized_extensions ||
            actual.nodes.size() != expected.nodes.size() || actual.connections.size() != expected.connections.size())
            return false;
        for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
            const auto& left = actual.nodes[index];
            const auto& right = expected.nodes[index];
            if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
                left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
                left.serialized_extensions != right.serialized_extensions || left.managed_version != right.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < actual.connections.size(); ++index) {
            const auto& left = actual.connections[index];
            const auto& right = expected.connections[index];
            if (left.from_node != right.from_node || left.from_port != right.from_port ||
                left.to_node != right.to_node || left.to_port != right.to_port ||
                left.serialized_extensions != right.serialized_extensions)
                return false;
        }
        return true;
    };
    const auto project_unchanged = [&](const audioalgo::ProjectModel& expected, bool expected_dirty,
                                       bool expected_stale, std::uint64_t expected_revision) {
        CHECK(same_project(controller.project(), expected));
        CHECK(controller.revision() == expected_revision);
        CHECK(controller.dirty() == expected_dirty);
        CHECK(controller.results_stale() == expected_stale);
    };

    juce::Array<juce::var> observations;
    const auto record_state = [&](const juce::String& state_id) {
        juce::DynamicObject::Ptr state = new juce::DynamicObject;
        state->setProperty("state_id", state_id);
        juce::Array<juce::var> visible;
        for (const auto& type_id : visible_type_ids())
            visible.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
        state->setProperty("visible_type_ids", visible);
        state->setProperty("category_description", category_heading->getDescription());
        state->setProperty("subcategory_description", subcategory_heading->getDescription());
        state->setProperty("no_results_visible", no_results != nullptr && no_results->isVisible());
        state->setProperty("no_results_text", no_results != nullptr ? no_results->getText() : juce::String{});
        state->setProperty("no_results_component_id",
                           no_results != nullptr ? no_results->getComponentID() : juce::String{});
        state->setProperty("clear_component_id",
                           clear_search != nullptr ? clear_search->getComponentID() : juce::String{});
        state->setProperty("clear_enabled", clear_search != nullptr && clear_search->isEnabled());
        state->setProperty("revision", static_cast<juce::int64>(controller.revision()));
        state->setProperty("dirty", controller.dirty());
        state->setProperty("results_stale", controller.results_stale());
        state->setProperty("undo_count", static_cast<juce::int64>(controller.history().undo_size()));
        state->setProperty("redo_count", static_cast<juce::int64>(controller.history().redo_size()));
        observations.add(juce::var(state.get()));
    };
    const auto expected_type_ids = [&](const char* field) {
        auto values = string_array(view_expectation.getProperty(field, {}));
        std::sort(values.begin(), values.end());
        return values;
    };
    const auto expect_unchanged_clean = [&] {
        project_unchanged(project, false, false, controller.revision());
        CHECK_FALSE(controller.dirty());
        CHECK_FALSE(controller.results_stale());
    };

    CHECK((clear_search != nullptr && clear_search->isVisible()));
    CHECK((clear_search == nullptr || !clear_search->isEnabled()));
    CHECK((no_results == nullptr || !no_results->isVisible()));
    CHECK(category_to_collapse->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(subcategory_to_collapse->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK(category_heading->getDescription().contains("collapsed"));
    CHECK(subcategory_heading->getDescription().contains("collapsed"));
    CHECK(visible_type_ids() == expected_type_ids("empty_query_visible_type_ids"));
    record_state("both_groups_collapsed");
    expect_unchanged_clean();

    const auto collapsed_category_query = view_expectation.getProperty("collapsed_category_query", {}).toString();
    set_query(collapsed_category_query);
    CHECK(visible_type_ids() == expected_type_ids("collapsed_category_query_type_ids"));
    CHECK(category_heading->getDescription().contains("expanded"));
    CHECK(subcategory_heading->getDescription().contains("expanded"));
    CHECK((no_results == nullptr || !no_results->isVisible()));
    CHECK((clear_search != nullptr && clear_search->isEnabled()));
    record_state("search_inside_collapsed_category");
    expect_unchanged_clean();

    const auto collapsed_subcategory_query = view_expectation.getProperty("collapsed_subcategory_query", {}).toString();
    set_query(collapsed_subcategory_query);
    CHECK(visible_type_ids() == expected_type_ids("collapsed_subcategory_query_type_ids"));
    CHECK(subcategory_heading->getDescription().contains("expanded"));
    record_state("search_inside_collapsed_subcategory");
    expect_unchanged_clean();

    set_query({});
    CHECK(search->getText().isEmpty());
    CHECK((clear_search == nullptr || !clear_search->isEnabled()));
    CHECK((no_results == nullptr || !no_results->isVisible()));
    CHECK(category_heading->getDescription().contains("collapsed"));
    CHECK(subcategory_heading->getDescription().contains("collapsed"));
    CHECK(visible_type_ids() == expected_type_ids("empty_query_visible_type_ids"));
    record_state("clear_restores_both_collapsed_groups");
    expect_unchanged_clean();

    const auto no_results_query = view_expectation.getProperty("no_results_query", {}).toString();
    set_query(no_results_query);
    CHECK(visible_type_ids().empty());
    CHECK((no_results != nullptr && no_results->isVisible()));
    if (no_results != nullptr)
        CHECK(no_results->getText() == view_expectation.getProperty("no_results_message", {}).toString());
    CHECK((clear_search != nullptr && clear_search->isVisible()));
    CHECK((clear_search != nullptr && clear_search->isEnabled()));
    record_state("no_results_message_and_clear_entry");
    expect_unchanged_clean();
    if (clear_search != nullptr)
        clear_search->triggerClick();
    else
        set_query({});
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    CHECK(search->getText().isEmpty());
    CHECK((no_results == nullptr || !no_results->isVisible()));
    CHECK(category_heading->getDescription().contains("collapsed"));
    CHECK(subcategory_heading->getDescription().contains("collapsed"));
    CHECK(visible_type_ids() == expected_type_ids("empty_query_visible_type_ids"));
    record_state("clear_no_results_restores_both_collapsed_groups");
    expect_unchanged_clean();

    REQUIRE(controller.add_node("fixture.audio.gain", "dirty-existing-node", -17.25, 94.5).status ==
            audioalgo::editor::ProjectEditStatus::applied);
    const auto dirty_project = controller.project();
    const auto dirty_revision = controller.revision();
    REQUIRE(controller.dirty());
    REQUIRE(controller.results_stale());
    set_query(collapsed_category_query);
    CHECK(visible_type_ids() == expected_type_ids("collapsed_category_query_type_ids"));
    record_state("dirty_and_stale_graph_during_search");
    project_unchanged(dirty_project, true, true, dirty_revision);
    CHECK(activated_type_ids.empty());
    CHECK(controller.history().undo_size() == 1);
    CHECK(controller.history().redo_size() == 0);

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-04.b");
    receipt->setProperty("observations", observations);
    juce::Array<juce::var> activated;
    for (const auto& type_id : activated_type_ids)
        activated.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
    receipt->setProperty("activated_type_ids", activated);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc04b/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC04B_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    const auto receipt_path = artifact_root / "component-search-view-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-04.c search editing cannot delete graph nodes and filtered entries remain activatable",
          "[NLC-04.c][node_library][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto expectation = load_library_keyboard_safety_expectation();
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);
    const auto query = expectation.getProperty("query", {}).toString();
    const auto target_type_id = expectation.getProperty("result_type_id", {}).toString().toStdString();
    const auto nonmatching_type_id = expectation.getProperty("nonmatching_type_id", {}).toString().toStdString();
    const auto existing_node_id = expectation.getProperty("existing_node", {}).getProperty("id", {}).toString().toStdString();
    const auto click_node_id = expectation.getProperty("click_added_node_id", {}).toString().toStdString();
    const auto keyboard_node_id = expectation.getProperty("keyboard_added_node_id", {}).toString().toStdString();
    const auto drag_node_id = expectation.getProperty("drag_added_node_id", {}).toString().toStdString();
    const auto initial_revision = static_cast<std::uint64_t>(
        expectation.getProperty("initial_revision", {}).toString().getLargeIntValue());
    REQUIRE(query.isNotEmpty());
    REQUIRE(!target_type_id.empty());
    REQUIRE(!existing_node_id.empty());

    audioalgo::NodeRegistry registry;
    for (const auto& [type_id, display_name] : std::vector<std::pair<std::string, std::string>>{
             {target_type_id, "Gain keyboard fixture"}, {"fixture.keyboard.echo", "Echo keyboard fixture"}}) {
        audioalgo::NodeDescriptor descriptor{
            type_id, "1.0.0", display_name,
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        descriptor.browser_classification = audioalgo::NodeBrowserClassification{"audio.dynamics", "gain"};
        descriptor.browser_tags = type_id == target_type_id
            ? std::vector<std::string>{"keyboard", "gain"}
            : std::vector<std::string>{"keyboard", "echo"};
        REQUIRE(registry.register_descriptor(descriptor));
    }

    audioalgo::ProjectModel initial_project;
    initial_project.project_id = "nlc04c-search-keyboard-safety";
    initial_project.name = "Search text must not delete the selected graph";
    initial_project.revision = initial_revision;
    initial_project.selected_output_node_id = "existing.keyboard.output";
    initial_project.selected_output_port_id = "audio_out";
    initial_project.serialized_extensions = {{"vendor_project", R"json({"retained":true})json"}};
    audioalgo::NodeInstance existing_node;
    existing_node.id = existing_node_id;
    existing_node.type_id = target_type_id;
    existing_node.x = static_cast<double>(expectation.getProperty("existing_node", {}).getProperty("x", {}));
    existing_node.y = static_cast<double>(expectation.getProperty("existing_node", {}).getProperty("y", {}));
    existing_node.serialized_extensions = {{"vendor_node", R"json({"retain":"exact"})json"}};
    audioalgo::NodeInstance output_node;
    output_node.id = "existing.keyboard.output";
    output_node.type_id = "fixture.keyboard.echo";
    output_node.x = 188.0;
    output_node.y = -16.0;
    initial_project.nodes = {existing_node, output_node};
    initial_project.connections.push_back({existing_node_id, "audio_out", output_node.id, "audio_in"});

    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(initial_project);
    controller.mark_saved();
    controller.mark_result_current();
    std::vector<std::string> dropped_node_ids;
    std::vector<audioalgo::editor::ProjectEditStatus> drop_statuses;
    std::uint64_t observed_drop_epoch = 0;
    std::uint64_t observed_drop_revision = 0;
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::GraphCanvas canvas(
        controller.project(), registry, selection, controller,
        [](std::string) {}, [] {}, [](std::string) {},
        [&](std::string type_id, audioalgo::editor::EditorPoint, std::uint64_t source_epoch,
            std::uint64_t drop_revision, std::optional<audioalgo::Connection>) {
            dropped_node_ids.push_back(drag_node_id);
            observed_drop_epoch = source_epoch;
            observed_drop_revision = drop_revision;
            drop_statuses.push_back(controller.add_node(type_id, drag_node_id, 320.0, 220.0).status);
        },
        [](juce::Component* source) -> std::optional<std::uint64_t> {
            const auto* button = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(source);
            return button == nullptr ? std::nullopt : std::optional<std::uint64_t>(button->gesture_project_epoch());
        });
    canvas.setSize(760, 520);
    canvas.setVisible(true);
    canvas.set_project_epoch(19);
    canvas.select(existing_node_id);
    REQUIRE(canvas.selected_node_id() == existing_node_id);

    std::vector<std::string> activated_type_ids;
    std::vector<std::string> added_node_ids;
    std::vector<audioalgo::editor::ProjectEditStatus> add_statuses;
    const auto on_entry = [&](std::string type_id) {
        const auto node_id = added_node_ids.empty() ? click_node_id : keyboard_node_id;
        activated_type_ids.push_back(type_id);
        added_node_ids.push_back(node_id);
        add_statuses.push_back(controller.add_node(type_id, node_id, 240.0, -36.0).status);
    };
    const auto wait_for_activations = [&](std::size_t expected_count) {
        auto* message_manager = juce::MessageManager::getInstance();
        if (message_manager == nullptr)
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (activated_type_ids.size() < expected_count && std::chrono::steady_clock::now() < deadline)
            message_manager->runDispatchLoopUntil(10);
        return activated_type_ids.size() == expected_count;
    };
    audioalgo::editor::NodeLibrary library(registry, on_entry, [] { return std::uint64_t{19}; });
    library.setSize(320, 640);
    auto* search = dynamic_cast<juce::TextEditor*>(find_component(library, "library.search"));
    auto* entry = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(
        find_component(library, "library." + juce::String::fromUTF8(
            target_type_id.data(), static_cast<int>(target_type_id.size()))));
    auto* nonmatching_entry = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(
        find_component(library, "library." + juce::String::fromUTF8(
            nonmatching_type_id.data(), static_cast<int>(nonmatching_type_id.size()))));
    REQUIRE(search != nullptr);
    REQUIRE(entry != nullptr);
    REQUIRE(nonmatching_entry != nullptr);

    const auto same_graph = [&](const audioalgo::ProjectModel& actual) {
        if (actual.schema_version != initial_project.schema_version ||
            actual.project_id != initial_project.project_id || actual.name != initial_project.name ||
            actual.revision != initial_project.revision ||
            actual.selected_output_node_id != initial_project.selected_output_node_id ||
            actual.selected_output_port_id != initial_project.selected_output_port_id ||
            actual.serialized_extensions != initial_project.serialized_extensions ||
            actual.nodes.size() != initial_project.nodes.size() ||
            actual.connections.size() != initial_project.connections.size())
            return false;
        for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
            const auto& actual_node = actual.nodes[index];
            const auto& expected_node = initial_project.nodes[index];
            if (actual_node.id != expected_node.id || actual_node.type_id != expected_node.type_id ||
                actual_node.parameters != expected_node.parameters || actual_node.x != expected_node.x ||
                actual_node.y != expected_node.y || actual_node.opaque_parameters != expected_node.opaque_parameters ||
                actual_node.serialized_extensions != expected_node.serialized_extensions ||
                actual_node.managed_version != expected_node.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < actual.connections.size(); ++index) {
            const auto& actual_edge = actual.connections[index];
            const auto& expected_edge = initial_project.connections[index];
            if (actual_edge.from_node != expected_edge.from_node || actual_edge.from_port != expected_edge.from_port ||
                actual_edge.to_node != expected_edge.to_node || actual_edge.to_port != expected_edge.to_port)
                return false;
        }
        return true;
    };
    const auto verify_graph_unchanged = [&]() -> bool {
        return same_graph(controller.project()) && controller.revision() == initial_revision &&
               !controller.dirty() && !controller.results_stale() &&
               controller.history().undo_size() == 0 && controller.history().redo_size() == 0 &&
               canvas.selected_node_id() == existing_node_id;
    };
    juce::Array<juce::var> observations;
    const auto record_state = [&](const std::string& state_id) {
        juce::DynamicObject::Ptr state = new juce::DynamicObject;
        state->setProperty("state_id", juce::String::fromUTF8(state_id.data(), static_cast<int>(state_id.size())));
        state->setProperty("query", search->getText());
        state->setProperty("result_visible", entry->isVisible());
        state->setProperty("nonmatching_result_visible", nonmatching_entry->isVisible());
        const auto graph_unchanged = verify_graph_unchanged();
        state->setProperty("graph_unchanged", graph_unchanged);
        state->setProperty("revision", static_cast<juce::int64>(controller.revision()));
        state->setProperty("dirty", controller.dirty());
        state->setProperty("results_stale", controller.results_stale());
        state->setProperty("undo_size", static_cast<int>(controller.history().undo_size()));
        state->setProperty("redo_size", static_cast<int>(controller.history().redo_size()));
        state->setProperty("selected_node_id", juce::String::fromUTF8(
            canvas.selected_node_id().data(), static_cast<int>(canvas.selected_node_id().size())));
        observations.add(juce::var(state.get()));
    };

    search->setText(query, true);
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    CHECK(entry->isVisible());
    CHECK_FALSE(nonmatching_entry->isVisible());
    search->setHighlightedRegion({0, query.length()});
    CHECK(search->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(search->getText().isEmpty());
    const auto delete_preserved_graph = verify_graph_unchanged();
    CHECK(delete_preserved_graph);
    record_state("delete_selected_search_text_preserves_graph");

    search->setText(query, true);
    search->setCaretPosition(query.length());
    CHECK(search->keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey)));
    CHECK(search->getText() == query.substring(0, query.length() - 1));
    const auto backspace_preserved_graph = verify_graph_unchanged();
    CHECK(backspace_preserved_graph);
    record_state("backspace_edits_search_text_only");

    search->setText(query, true);
    search->setHighlightedRegion({0, 0});
    CHECK(search->keyPressed(juce::KeyPress('a', juce::ModifierKeys::ctrlModifier, 0)));
    CHECK(search->getHighlightedRegion().getStart() == 0);
    CHECK(search->getHighlightedRegion().getEnd() == query.length());
    const auto ctrl_a_preserved_graph = verify_graph_unchanged();
    CHECK(ctrl_a_preserved_graph);
    record_state("ctrl_a_selects_query_text_only");
    CHECK(search->keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey)));
    CHECK(search->getText().isEmpty());
    const auto selection_delete_preserved_graph = verify_graph_unchanged();
    CHECK(selection_delete_preserved_graph);
    record_state("backspace_after_text_selection_preserves_graph");

    search->setText(query, true);
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    CHECK(entry->isVisible());
    CHECK_FALSE(nonmatching_entry->isVisible());
    CHECK((controller.revision() == initial_revision));
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());

    const auto click_time = juce::Time::getCurrentTime();
    const juce::Point<float> click_position(8.0f, 10.0f);
    const juce::MouseEvent click_event(juce::Desktop::getInstance().getMainMouseSource(), click_position,
        juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        entry, entry, click_time, click_position, click_time, 1, false);
    entry->mouseDown(click_event);
    entry->mouseUp(click_event);
    CHECK(wait_for_activations(1));
    REQUIRE(added_node_ids.size() == 1);
    CHECK(activated_type_ids.back() == target_type_id);
    CHECK(add_statuses.back() == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(controller.project().nodes.size() == initial_project.nodes.size() + 1);
    CHECK(controller.project().nodes.back().id == click_node_id);
    CHECK(controller.project().nodes.back().type_id == target_type_id);
    CHECK(controller.revision() == initial_revision + 1);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK(controller.history().undo_size() == 1);
    record_state("filtered_entry_click_adds_exact_type_once");

    CHECK(static_cast<juce::Component*>(entry)->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK(wait_for_activations(2));
    REQUIRE(added_node_ids.size() == 2);
    CHECK(activated_type_ids.back() == target_type_id);
    CHECK(add_statuses.back() == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(controller.project().nodes.size() == initial_project.nodes.size() + 2);
    CHECK(controller.project().nodes.back().id == keyboard_node_id);
    CHECK(controller.project().nodes.back().type_id == target_type_id);
    CHECK(controller.revision() == initial_revision + 2);
    CHECK(controller.history().undo_size() == 2);
    record_state("filtered_entry_keyboard_adds_exact_type_once");

    const juce::DragAndDropTarget::SourceDetails drag_details(
        audioalgo::editor::make_library_drag_description(target_type_id), entry, {320, 220});
    CHECK(canvas.isInterestedInDragSource(drag_details));
    canvas.itemDragEnter(drag_details);
    canvas.itemDropped(drag_details);
    REQUIRE(dropped_node_ids.size() == 1);
    CHECK(dropped_node_ids.back() == drag_node_id);
    CHECK(drop_statuses.back() == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(activated_type_ids.size() == 2);
    CHECK(controller.project().nodes.size() == initial_project.nodes.size() + 3);
    CHECK(controller.project().nodes.back().id == drag_node_id);
    CHECK(controller.project().nodes.back().type_id == target_type_id);
    CHECK(observed_drop_epoch == 19);
    CHECK(observed_drop_revision == initial_revision + 2);
    CHECK(controller.revision() == initial_revision + 3);
    CHECK(controller.history().undo_size() == 3);
    record_state("filtered_entry_drag_drop_adds_exact_type_once");

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-04.c");
    receipt->setProperty("observations", observations);
    juce::Array<juce::var> activated;
    for (const auto& type_id : activated_type_ids)
        activated.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
    receipt->setProperty("activated_type_ids", activated);
    juce::Array<juce::var> created;
    for (const auto& node_id : added_node_ids)
        created.add(juce::String::fromUTF8(node_id.data(), static_cast<int>(node_id.size())));
    for (const auto& node_id : dropped_node_ids)
        created.add(juce::String::fromUTF8(node_id.data(), static_cast<int>(node_id.size())));
    receipt->setProperty("created_node_ids", created);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc04c/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC04C_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    const auto receipt_path = artifact_root / "component-keyboard-safety-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-05.a production library combines trusted source language and search filters",
          "[NLC-05.a][node_library][component][provenance]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto provenance = load_provenance_expectation();
    const auto expectation = load_library_filters_expectation();
    REQUIRE(provenance.isObject());
    REQUIRE(expectation.isObject());
    CHECK(static_cast<int>(expectation.getProperty("schema_version", {})) == 1);
    const auto* expected_nodes = provenance.getProperty("nodes", {}).getArray();
    REQUIRE(expected_nodes != nullptr);

    audioalgo::NodeRegistry registry;
    const auto register_fixture_node = [&](const juce::var& item) {
        audioalgo::NodeDescriptor descriptor{
            item.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
            item.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
        const auto browser = item.getProperty("browser", {});
        if (browser.isObject()) {
            descriptor.browser_classification = audioalgo::NodeBrowserClassification{
                browser.getProperty("category_id", {}).toString().toStdString(),
                browser.getProperty("subcategory_id", {}).toString().toStdString()};
            descriptor.browser_tags = string_array(browser.getProperty("tags", {}));
        }
        const auto trusted = item.getProperty("trusted_registration", {});
        const auto trusted_source = trusted.getProperty("source_id", {}).toString().toStdString();
        if (trusted_source != "builtin") {
            descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
                trusted.getProperty("plugin_id", {}).toString().toStdString(), "1.0.0", "",
                "fixture-manifest-sha256", trusted.getProperty("runtime", {}).toString().toStdString()};
            descriptor.plugin_origin->managed_content_sha256 =
                trusted.getProperty("managed_content_sha256", {}).toString().toStdString();
        }
        return registry.register_descriptor(std::move(descriptor));
    };
    for (const auto& node : *expected_nodes)
        REQUIRE(register_fixture_node(node));
    REQUIRE(register_fixture_node(expectation.getProperty("unreported_runtime_node", {})));

    const auto entries = audioalgo::editor::build_node_library_entries(registry);
    CHECK(entries.size() == static_cast<std::size_t>(
        static_cast<int>(expectation.getProperty("all_type_ids", {}).getArray()->size())));
    for (const auto& expected_node : *expected_nodes) {
        const auto expected_type_id = expected_node.getProperty("type_id", {}).toString().toStdString();
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
            return candidate.type_id == expected_type_id;
        });
        REQUIRE(entry != entries.end());
        const auto trusted = expected_node.getProperty("trusted_registration", {});
        CHECK(entry->source_id == trusted.getProperty("source_id", {}).toString().toStdString());
        CHECK(entry->language_id == trusted.getProperty("language_id", {}).toString().toStdString());
        const auto browser = expected_node.getProperty("browser", {});
        if (browser.isObject()) {
            CHECK(browser.getProperty("source_id", {}).toString().toStdString() != entry->source_id);
            CHECK(browser.getProperty("language_id", {}).toString().toStdString() != entry->language_id);
        }
    }
    const auto unknown_type_id = expectation.getProperty("unreported_runtime_node", {}).getProperty(
        "type_id", {}).toString().toStdString();
    const auto unknown_runtime_entry = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.type_id == unknown_type_id;
    });
    REQUIRE(unknown_runtime_entry != entries.end());
    CHECK(unknown_runtime_entry->source_id == "third_party");
    CHECK(unknown_runtime_entry->language_id.empty());

    std::vector<std::string> activated_type_ids;
    const auto add_from_library = [&](std::string type_id) {
        activated_type_ids.push_back(type_id);
    };
    audioalgo::editor::NodeLibrary library(registry, add_from_library, [] { return std::uint64_t{41}; });
    library.setSize(360, 820);

    auto* source_filter = dynamic_cast<juce::ComboBox*>(find_component(
        library, expectation.getProperty("source_filter_component_id", {}).toString()));
    auto* language_filter = dynamic_cast<juce::ComboBox*>(find_component(
        library, expectation.getProperty("language_filter_component_id", {}).toString()));
    auto* search = dynamic_cast<juce::TextEditor*>(find_component(
        library, expectation.getProperty("search_component_id", {}).toString()));
    REQUIRE(source_filter != nullptr);
    REQUIRE(language_filter != nullptr);
    REQUIRE(search != nullptr);

    const auto check_options = [&](juce::ComboBox& combo, const juce::var& expected_options) {
        const auto* options = expected_options.getArray();
        REQUIRE(options != nullptr);
        CHECK(combo.getNumItems() == static_cast<int>(options->size()));
        if (combo.getNumItems() != static_cast<int>(options->size()))
            return;
        for (int index = 0; index < combo.getNumItems(); ++index) {
            const auto& option = options->getReference(static_cast<std::size_t>(index));
            CHECK(combo.getItemId(index) == static_cast<int>(option.getProperty("id", {})));
            CHECK(combo.getItemText(index) == option.getProperty("label", {}).toString());
        }
    };
    const auto options = expectation.getProperty("options", {});
    check_options(*source_filter, options.getProperty("source", {}));
    check_options(*language_filter, options.getProperty("language", {}));

    const auto expected_all = string_array(expectation.getProperty("all_type_ids", {}));
    const auto visible_entry_ids = [&] {
        std::vector<std::string> visible;
        for (const auto& type_id : expected_all) {
            const auto component_id = "library." + type_id;
            auto* button = dynamic_cast<juce::Button*>(find_component(
                library, juce::String::fromUTF8(component_id.data(), static_cast<int>(component_id.size()))));
            if (button != nullptr && button->isVisible())
                visible.push_back(type_id);
        }
        std::sort(visible.begin(), visible.end());
        return visible;
    };
    juce::Array<juce::var> observed_states;
    const auto* cases = expectation.getProperty("cases", {}).getArray();
    REQUIRE(cases != nullptr);
    for (const auto& state : *cases) {
        const auto state_id = state.getProperty("id", {}).toString().toStdString();
        INFO("filter state: " << state_id);
        source_filter->setSelectedId(static_cast<int>(state.getProperty("source_id", {})),
                                     juce::sendNotificationSync);
        language_filter->setSelectedId(static_cast<int>(state.getProperty("language_id", {})),
                                       juce::sendNotificationSync);
        search->setText(state.getProperty("query", {}).toString(), true);
        if (auto* manager = juce::MessageManager::getInstance())
            manager->runDispatchLoopUntil(10);
        const auto expected_ids = string_array(state.getProperty("expected_type_ids", {}));
        auto sorted_expected = expected_ids;
        std::sort(sorted_expected.begin(), sorted_expected.end());
        CHECK(visible_entry_ids() == sorted_expected);

        juce::DynamicObject::Ptr observation = new juce::DynamicObject;
        observation->setProperty("state_id", state.getProperty("id", {}));
        observation->setProperty("source_id", state.getProperty("source_id", {}));
        observation->setProperty("language_id", state.getProperty("language_id", {}));
        observation->setProperty("query", state.getProperty("query", {}));
        juce::Array<juce::var> visible;
        for (const auto& type_id : visible_entry_ids())
            visible.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
        observation->setProperty("visible_type_ids", visible);
        observed_states.add(juce::var(observation.get()));
    }

    const auto activation_type_id = expectation.getProperty("activation_type_id", {}).toString().toStdString();
    const auto activation_component_id = "library." + activation_type_id;
    auto* activation = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(find_component(
        library, juce::String::fromUTF8(activation_component_id.data(), static_cast<int>(activation_component_id.size()))));
    REQUIRE(activation != nullptr);
    REQUIRE(activation->isVisible());
    const auto click_time = juce::Time::getCurrentTime();
    const juce::Point<float> click_position(8.0f, 10.0f);
    const juce::MouseEvent click_event(juce::Desktop::getInstance().getMainMouseSource(), click_position,
        juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        activation, activation, click_time, click_position, click_time, 1, false);
    activation->mouseDown(click_event);
    activation->mouseUp(click_event);
    auto* message_manager = juce::MessageManager::getInstance();
    REQUIRE(message_manager != nullptr);
    const auto click_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (activated_type_ids.empty() && std::chrono::steady_clock::now() < click_deadline)
        message_manager->runDispatchLoopUntil(10);
    REQUIRE(activated_type_ids.size() == 1);
    CHECK(activated_type_ids.front() == activation_type_id);

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-05.a");
    receipt->setProperty("observations", observed_states);
    juce::Array<juce::var> activated_types;
    for (const auto& type_id : activated_type_ids)
        activated_types.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
    receipt->setProperty("activated_type_ids", activated_types);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc05a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC05A_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    const auto receipt_path = artifact_root / "component-filter-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-05.b clear resets every library filter while preserving canvas and project state",
          "[NLC-05.b][node_library][component][filters]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto provenance = load_provenance_expectation();
    const auto filters = load_library_filters_expectation();
    REQUIRE(provenance.isObject());
    REQUIRE(filters.isObject());
    const auto fixture_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_browser/library_filter_clear_v1.json";
    std::ifstream fixture_file(fixture_path, std::ios::binary);
    REQUIRE(fixture_file.good());
    const std::string fixture_bytes{
        std::istreambuf_iterator<char>(fixture_file), std::istreambuf_iterator<char>()};
    const auto fixture = juce::JSON::parse(
        juce::String::fromUTF8(fixture_bytes.data(), static_cast<int>(fixture_bytes.size())));
    REQUIRE(fixture.isObject());
    CHECK(static_cast<int>(fixture.getProperty("schema_version", {})) == 1);
    CHECK(fixture.getProperty("filter_fixture", {}).toString() ==
          "tests/fixtures/node_browser/library_filters_v1.json");

    audioalgo::NodeRegistry registry;
    const auto register_fixture_node = [&](const juce::var& item) {
        audioalgo::NodeDescriptor descriptor{
            item.getProperty("type_id", {}).toString().toStdString(), "1.0.0",
            item.getProperty("display_name", {}).toString().toStdString(),
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {{"gain_db", audioalgo::ParameterKind::floating, 0.0, -60.0, 12.0, "dB"}},
            audioalgo::NodeExecutionKind::block};
        const auto browser = item.getProperty("browser", {});
        if (browser.isObject()) {
            descriptor.browser_classification = audioalgo::NodeBrowserClassification{
                browser.getProperty("category_id", {}).toString().toStdString(),
                browser.getProperty("subcategory_id", {}).toString().toStdString()};
            descriptor.browser_tags = string_array(browser.getProperty("tags", {}));
        }
        const auto trusted = item.getProperty("trusted_registration", {});
        const auto trusted_source = trusted.getProperty("source_id", {}).toString().toStdString();
        if (trusted_source != "builtin") {
            descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
                trusted.getProperty("plugin_id", {}).toString().toStdString(), "1.0.0", "",
                "fixture-manifest-sha256", trusted.getProperty("runtime", {}).toString().toStdString()};
            descriptor.plugin_origin->managed_content_sha256 =
                trusted.getProperty("managed_content_sha256", {}).toString().toStdString();
        }
        return registry.register_descriptor(std::move(descriptor));
    };
    const auto* provenance_nodes = provenance.getProperty("nodes", {}).getArray();
    REQUIRE(provenance_nodes != nullptr);
    for (const auto& node : *provenance_nodes)
        REQUIRE(register_fixture_node(node));
    REQUIRE(register_fixture_node(filters.getProperty("unreported_runtime_node", {})));

    audioalgo::ProjectModel project;
    project.project_id = "nlc05b-clear-filters-view-only";
    project.name = "Clearing library filters must preserve this graph";
    project.revision = 17;
    project.serialized_extensions = {{"vendor_project", R"json({"preserve":true})json"}};
    audioalgo::NodeInstance selected_node;
    selected_node.id = fixture.getProperty("selected_node_id", {}).toString().toStdString();
    selected_node.type_id = "builtin.gain";
    selected_node.parameters = {{"gain_db", 0.0}};
    selected_node.x = -23.75;
    selected_node.y = 84.5;
    selected_node.opaque_parameters = {{"gain_future", R"json({"curve":[1,2,3]})json"}};
    selected_node.serialized_extensions = {{"vendor_node", R"json({"keep":"exact"})json"}};
    project.nodes.push_back(selected_node);
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    controller.mark_saved();
    controller.mark_result_current();
    REQUIRE(controller.set_parameter(selected_node.id, "gain_db", 2.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    REQUIRE(controller.set_parameter(selected_node.id, "gain_db", 3.0).status ==
            audioalgo::editor::ParameterEditStatus::applied);
    controller.mark_saved();
    controller.mark_result_current();
    REQUIRE(controller.undo());
    REQUIRE(controller.dirty());
    REQUIRE(controller.results_stale());
    REQUIRE(controller.history().undo_size() == 1);
    REQUIRE(controller.history().redo_size() == 1);

    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::GraphCanvas canvas(controller.project(), registry, selection, controller,
        [](std::string) {}, [] {}, [](std::string) {},
        [](std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
           std::optional<audioalgo::Connection>) {},
        [](juce::Component*) -> std::optional<std::uint64_t> { return std::nullopt; });
    canvas.select(selected_node.id);
    REQUIRE(canvas.selected_node_id() == selected_node.id);
    REQUIRE((selection.selected_node_ids() == std::vector<std::string>{selected_node.id}));

    std::vector<std::string> focus_requests;
    audioalgo::editor::NodeLibrary library(registry, [](std::string) {}, [] { return std::uint64_t{41}; },
        {}, {}, {}, {}, {}, [&] { focus_requests.emplace_back("library.search"); });
    library.setSize(360, 420);
    auto* search = dynamic_cast<juce::TextEditor*>(find_component(
        library, fixture.getProperty("search_component_id", {}).toString()));
    auto* source_filter = dynamic_cast<juce::ComboBox*>(find_component(
        library, fixture.getProperty("source_filter_component_id", {}).toString()));
    auto* language_filter = dynamic_cast<juce::ComboBox*>(find_component(
        library, fixture.getProperty("language_filter_component_id", {}).toString()));
    auto* clear_filters = dynamic_cast<juce::Button*>(find_component(
        library, fixture.getProperty("clear_component_id", {}).toString()));
    auto* content = find_component(library, "library.content");
    REQUIRE(search != nullptr);
    REQUIRE(source_filter != nullptr);
    REQUIRE(language_filter != nullptr);
    REQUIRE(clear_filters != nullptr);
    REQUIRE(content != nullptr);
    juce::Viewport* viewport = nullptr;
    const auto locate_viewport = [&](auto&& self, juce::Component& parent) -> void {
        if (auto* candidate = dynamic_cast<juce::Viewport*>(&parent)) {
            const auto* viewed = candidate->getViewedComponent();
            if (viewed != nullptr && viewed->getComponentID() == "library.content") {
                viewport = candidate;
                return;
            }
        }
        for (int index = 0; index < parent.getNumChildComponents() && viewport == nullptr; ++index)
            self(self, *parent.getChildComponent(index));
    };
    locate_viewport(locate_viewport, library);
    REQUIRE(viewport != nullptr);
    CHECK(viewport->getViewedComponent() == content);
    const auto expected_all = string_array(fixture.getProperty("all_type_ids", {}));
    auto sorted_all = expected_all;
    std::sort(sorted_all.begin(), sorted_all.end());
    const auto visible_entry_ids = [&] {
        std::vector<std::string> visible;
        for (const auto& type_id : expected_all) {
            const auto component_id = "library." + type_id;
            auto* button = dynamic_cast<juce::Button*>(find_component(
                library, juce::String::fromUTF8(component_id.data(), static_cast<int>(component_id.size()))));
            if (button != nullptr && button->isVisible())
                visible.push_back(type_id);
        }
        std::sort(visible.begin(), visible.end());
        return visible;
    };
    const auto same_project = [](const audioalgo::ProjectModel& actual,
                                 const audioalgo::ProjectModel& expected) {
        if (actual.schema_version != expected.schema_version || actual.project_id != expected.project_id ||
            actual.name != expected.name || actual.revision != expected.revision ||
            actual.selected_output_node_id != expected.selected_output_node_id ||
            actual.selected_output_port_id != expected.selected_output_port_id ||
            actual.serialized_extensions != expected.serialized_extensions ||
            actual.nodes.size() != expected.nodes.size() || actual.connections.size() != expected.connections.size())
            return false;
        for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
            const auto& left = actual.nodes[index];
            const auto& right = expected.nodes[index];
            if (left.id != right.id || left.type_id != right.type_id || left.parameters != right.parameters ||
                left.x != right.x || left.y != right.y || left.opaque_parameters != right.opaque_parameters ||
                left.serialized_extensions != right.serialized_extensions || left.managed_version != right.managed_version)
                return false;
        }
        for (std::size_t index = 0; index < actual.connections.size(); ++index) {
            const auto& left = actual.connections[index];
            const auto& right = expected.connections[index];
            if (left.from_node != right.from_node || left.from_port != right.from_port ||
                left.to_node != right.to_node || left.to_port != right.to_port ||
                left.serialized_extensions != right.serialized_extensions)
                return false;
        }
        return true;
    };
    juce::Array<juce::var> observations;
    const auto record_state = [&](const juce::String& state_id) {
        juce::DynamicObject::Ptr state = new juce::DynamicObject;
        state->setProperty("state_id", state_id);
        state->setProperty("query", search->getText());
        state->setProperty("source_id", source_filter->getSelectedId());
        state->setProperty("language_id", language_filter->getSelectedId());
        juce::Array<juce::var> visible;
        for (const auto& type_id : visible_entry_ids())
            visible.add(juce::String::fromUTF8(type_id.data(), static_cast<int>(type_id.size())));
        state->setProperty("visible_type_ids", visible);
        state->setProperty("view_y", viewport->getViewPositionY());
        state->setProperty("content_height", content->getHeight());
        state->setProperty("viewport_height", viewport->getHeight());
        state->setProperty("clear_enabled", clear_filters->isEnabled());
        state->setProperty("search_is_focusable", search->getWantsKeyboardFocus());
        state->setProperty("focus_request_count", static_cast<int>(focus_requests.size()));
        state->setProperty("focus_request_target", focus_requests.empty() ? juce::String{} : focus_requests.back());
        state->setProperty("selected_node_id", juce::String::fromUTF8(
            canvas.selected_node_id().data(), static_cast<int>(canvas.selected_node_id().size())));
        state->setProperty("revision", static_cast<juce::int64>(controller.revision()));
        state->setProperty("dirty", controller.dirty());
        state->setProperty("results_stale", controller.results_stale());
        state->setProperty("undo_count", static_cast<juce::int64>(controller.history().undo_size()));
        state->setProperty("redo_count", static_cast<juce::int64>(controller.history().redo_size()));
        observations.add(juce::var(state.get()));
    };
    const auto project_before_filters = controller.project();
    const auto revision_before_filters = controller.revision();
    const auto dirty_before_filters = controller.dirty();
    const auto stale_before_filters = controller.results_stale();
    const auto undo_before_filters = controller.history().undo_size();
    const auto redo_before_filters = controller.history().redo_size();
    const auto selection_before_filters = selection.selected_node_ids();
    const auto require_editor_state_unchanged = [&] {
        CHECK(same_project(controller.project(), project_before_filters));
        CHECK(controller.revision() == revision_before_filters);
        CHECK(controller.dirty() == dirty_before_filters);
        CHECK(controller.results_stale() == stale_before_filters);
        CHECK(controller.history().undo_size() == undo_before_filters);
        CHECK(controller.history().redo_size() == redo_before_filters);
        CHECK(canvas.selected_node_id() == selected_node.id);
        CHECK(selection.selected_node_ids() == selection_before_filters);
    };
    const auto filter_state = fixture.getProperty("filtered_state", {});
    REQUIRE(filter_state.isObject());
    CHECK(visible_entry_ids() == sorted_all);
    const auto full_scroll_max = std::max(0, content->getHeight() - viewport->getHeight());
    CHECK(full_scroll_max >= static_cast<int>(fixture.getProperty("minimum_initial_scroll_range", {})));
    viewport->setViewPosition(0, full_scroll_max + 500);
    CHECK(viewport->getViewPositionY() == full_scroll_max);

    source_filter->setSelectedId(static_cast<int>(filter_state.getProperty("source_id", {})),
                                 juce::sendNotificationSync);
    language_filter->setSelectedId(static_cast<int>(filter_state.getProperty("language_id", {})),
                                   juce::sendNotificationSync);
    search->setText(filter_state.getProperty("query", {}).toString(), true);
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    auto expected_filtered = string_array(filter_state.getProperty("expected_type_ids", {}));
    std::sort(expected_filtered.begin(), expected_filtered.end());
    CHECK((visible_entry_ids() == expected_filtered));
    CHECK(clear_filters->isEnabled());
    const auto filtered_max = std::max(0, content->getHeight() - viewport->getHeight());
    CHECK(viewport->getViewPositionY() >= 0);
    CHECK(viewport->getViewPositionY() <= filtered_max);
    record_state("combined_filter_before_clear");
    require_editor_state_unchanged();

    clear_filters->triggerClick();
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    CHECK(search->getText().isEmpty());
    CHECK(source_filter->getSelectedId() == static_cast<int>(fixture.getProperty("all_source_id", {})));
    CHECK(language_filter->getSelectedId() == static_cast<int>(fixture.getProperty("all_language_id", {})));
    CHECK(visible_entry_ids() == sorted_all);
    CHECK_FALSE(clear_filters->isEnabled());
    CHECK(focus_requests.size() == 1);
    CHECK(focus_requests.back() == "library.search");
    CHECK(search->getWantsKeyboardFocus());
    const auto restored_max = std::max(0, content->getHeight() - viewport->getHeight());
    CHECK(viewport->getViewPositionY() >= 0);
    CHECK(viewport->getViewPositionY() <= restored_max);
    record_state("clear_combined_filter_restores_all");
    require_editor_state_unchanged();

    const auto source_only_state = fixture.getProperty("source_only_state", {});
    REQUIRE(source_only_state.isObject());
    source_filter->setSelectedId(static_cast<int>(source_only_state.getProperty("source_id", {})),
                                 juce::sendNotificationSync);
    auto expected_source_only = string_array(source_only_state.getProperty("expected_type_ids", {}));
    std::sort(expected_source_only.begin(), expected_source_only.end());
    CHECK((visible_entry_ids() == expected_source_only));
    CHECK(clear_filters->isEnabled());
    record_state("source_only_filter_before_clear");
    require_editor_state_unchanged();
    clear_filters->triggerClick();
    if (auto* manager = juce::MessageManager::getInstance())
        manager->runDispatchLoopUntil(10);
    CHECK(search->getText().isEmpty());
    CHECK(source_filter->getSelectedId() == static_cast<int>(fixture.getProperty("all_source_id", {})));
    CHECK(language_filter->getSelectedId() == static_cast<int>(fixture.getProperty("all_language_id", {})));
    CHECK(visible_entry_ids() == sorted_all);
    CHECK(focus_requests.size() == 2);
    CHECK(focus_requests.back() == "library.search");
    CHECK(search->getWantsKeyboardFocus());
    const auto source_reset_max = std::max(0, content->getHeight() - viewport->getHeight());
    CHECK(viewport->getViewPositionY() >= 0);
    CHECK(viewport->getViewPositionY() <= source_reset_max);
    record_state("clear_source_only_filter_restores_all");
    require_editor_state_unchanged();

    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("behavior_id", "NLC-05.b");
    receipt->setProperty("observations", observations);
    const auto artifact_root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc05b/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(artifact_root);
    std::cout << "NLC05B_ARTIFACT_ROOT=" << artifact_root.generic_string() << '\n';
    const auto receipt_path = artifact_root / "component-filter-clear-observation.json";
    const auto receipt_text = juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    std::ofstream receipt_file(receipt_path, std::ios::binary);
    receipt_file.write(receipt_text.data(), static_cast<std::streamsize>(receipt_text.size()));
    receipt_file.close();
    REQUIRE(receipt_file.good());
}

TEST_CASE("NLC-05.c filtered library pointer gestures cancel outside and add the exact type once",
          "[NLC-05.c][node_library][component][drag]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto fixture_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_browser/library_filtered_gestures_v1.json";
    std::ifstream input(fixture_path, std::ios::binary);
    REQUIRE(input.good());
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto fixture = juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
    REQUIRE(fixture.isObject());
    const auto type_id = fixture["target_type_id"].toString().toStdString();
    const auto epoch = static_cast<std::uint64_t>(fixture["epoch"].toString().getLargeIntValue());
    const auto revision = static_cast<std::uint64_t>(fixture["initial_revision"].toString().getLargeIntValue());
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    audioalgo::NodeDescriptor descriptor{type_id, "1.0.0", "Filtered gesture fixture",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    descriptor.browser_classification = audioalgo::NodeBrowserClassification{"utility", "inspection"};
    descriptor.plugin_origin = audioalgo::NodeDescriptor::PluginOrigin{
        "user.filtered_gesture", "1.0.0", "", "fixture-manifest", "python"};
    descriptor.plugin_origin->managed_content_sha256 = std::string(64, 'a');
    descriptor.plugin_origin->managed_package_path = "packages/user.filtered_gesture/" + std::string(64, 'a');
    descriptor.plugin_origin->managed_wrapper_version = "function-1";
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::ProjectModel project;
    project.project_id = "nlc05c-filtered-gestures";
    project.name = "Preserve original node during library gestures";
    project.revision = revision;
    project.serialized_extensions = {{"vendor_project", "{\"keep\":true}"}};
    audioalgo::NodeInstance original;
    original.id = "original"; original.type_id = "builtin.gain";
    original.parameters = {{"gain_db", 3.0}};
    original.x = -36.5; original.y = 41.25;
    original.serialized_extensions = {{"vendor_node", "true"}};
    project.nodes.push_back(original);
    audioalgo::editor::EditorController controller(registry);
    controller.adopt_project(project);
    controller.mark_saved(); controller.mark_result_current();
    audioalgo::editor::NodeSelectionState selection;
    std::vector<std::string> clicked_types, dropped_types;
    audioalgo::editor::GraphCanvas canvas(controller.project(), registry, selection, controller,
        [](std::string) {}, [] {}, [](std::string) {},
        [&](std::string type, audioalgo::editor::EditorPoint point, std::uint64_t source_epoch,
            std::uint64_t source_revision, std::optional<audioalgo::Connection> edge) {
            CHECK(source_epoch == epoch);
            CHECK(source_revision == controller.revision());
            CHECK_FALSE(edge.has_value());
            dropped_types.push_back(type);
            CHECK(controller.add_node(type, "dropped", point.x, point.y).status ==
                  audioalgo::editor::ProjectEditStatus::applied);
        },
        [](juce::Component* source) -> std::optional<std::uint64_t> {
            const auto* button = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(source);
            return button ? std::optional<std::uint64_t>{button->gesture_project_epoch()} : std::nullopt;
        });
    canvas.setSize(800, 520); canvas.setVisible(true); canvas.set_project_epoch(epoch); canvas.select(original.id);
    audioalgo::editor::NodeLibrary library(registry, [&](std::string type) {
        clicked_types.push_back(type);
        CHECK(controller.add_node(type, "clicked", 210, 85).status ==
              audioalgo::editor::ProjectEditStatus::applied);
    }, [=] { return epoch; });
    library.setSize(360, 700);
    library.setVisible(true);
    const auto mouse = [](juce::Component& component, juce::Point<float> position,
                          int modifiers, bool dragged = false) {
        const auto time = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
            juce::ModifierKeys(modifiers), 1.f, 0.f, 0.f, 0.f, 0.f, &component, &component,
            time, {8.f, 10.f}, time, 1, dragged);
    };
    const auto pump = [] {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(25);
    };
    auto* search = dynamic_cast<juce::TextEditor*>(find_component(library, "library.search"));
    auto* source = dynamic_cast<juce::ComboBox*>(find_component(library, "library.filter.source"));
    auto* language = dynamic_cast<juce::ComboBox*>(find_component(library, "library.filter.language"));
    auto* entry = dynamic_cast<audioalgo::editor::NodeLibraryButton*>(find_component(
        library, "library." + juce::String(type_id)));
    auto* category = find_component(library, fixture["category_component_id"].toString());
    auto* subcategory = find_component(library, fixture["subcategory_component_id"].toString());
    REQUIRE(search); REQUIRE(source); REQUIRE(language); REQUIRE(entry); REQUIRE(category); REQUIRE(subcategory);
    // A heading click folds its own entries and never dispatches a node action.
    category->mouseDown(mouse(*category, {8, 10}, juce::ModifierKeys::leftButtonModifier));
    category->mouseUp(mouse(*category, {8, 10}, 0));
    CHECK_FALSE(entry->isVisible());
    CHECK(clicked_types.empty());
    juce::Array<juce::var> observations;
    const auto record = [&](const char* state) {
        juce::DynamicObject::Ptr item = new juce::DynamicObject;
        item->setProperty("id", state);
        item->setProperty("created", static_cast<int>(controller.project().nodes.size() - 1));
        item->setProperty("revision", static_cast<juce::int64>(controller.revision()));
        item->setProperty("click_count", static_cast<int>(clicked_types.size()));
        item->setProperty("drop_count", static_cast<int>(dropped_types.size()));
        observations.add(juce::var(item.get()));
    };
    record("category_fold");
    source->setSelectedId(static_cast<int>(fixture["source_filter_id"]), juce::sendNotificationSync);
    language->setSelectedId(static_cast<int>(fixture["language_filter_id"]), juce::sendNotificationSync);
    search->setText(fixture["query"].toString(), true); pump();
    CHECK(entry->isVisible()); CHECK(category->isVisible()); CHECK(subcategory->isVisible());
    // Hit resolution observes the actual view tree, including Viewport clipping and overlays.
    const auto library_point = library.getLocalPoint(entry, juce::Point<int>{8, 10});
    CHECK(library.getComponentAt(library_point) == entry);
    CHECK_FALSE(search->getBounds().contains(library_point));
    CHECK_FALSE(source->getBounds().contains(library_point));
    CHECK_FALSE(language->getBounds().contains(library_point));
    CHECK_FALSE(library.getLocalArea(category, category->getLocalBounds()).contains(library_point));
    CHECK_FALSE(library.getLocalArea(subcategory, subcategory->getLocalBounds()).contains(library_point));
    entry->mouseDown(mouse(*entry, {8, 10}, juce::ModifierKeys::leftButtonModifier));
    entry->mouseUp(mouse(*entry, {8, 10}, 0)); pump();
    CHECK(clicked_types == std::vector<std::string>{type_id});
    CHECK(controller.project().nodes.back().type_id == type_id);
    CHECK(controller.revision() == revision + 1);
    CHECK(controller.history().undo_size() == 1);
    record("filtered_entry_click");
    const auto down = [&] {
        entry->mouseDown(mouse(*entry, {8, 10}, juce::ModifierKeys::leftButtonModifier));
        // Without an OS capture, no JUCE drag image is started; the production
        // button still owns threshold and post-drag click suppression.
        entry->mouseDrag(mouse(*entry, {15, 10}, juce::ModifierKeys::leftButtonModifier, true));
        CHECK(entry->gesture_project_epoch() == epoch);
    };
    const juce::Point<int> drop_point{static_cast<int>(fixture["drop_position"][0]),
                                     static_cast<int>(fixture["drop_position"][1])};
    const juce::DragAndDropTarget::SourceDetails details(
        audioalgo::editor::make_library_drag_description(type_id), entry, drop_point);
    down();
    CHECK(canvas.isInterestedInDragSource(details));
    canvas.itemDragEnter(details); canvas.itemDragMove(details); canvas.itemDragExit(details);
    canvas.finish_library_drag(epoch);
    entry->mouseUp(mouse(*entry, {-12, 10}, 0, true)); pump();
    CHECK(dropped_types.empty());
    CHECK(clicked_types.size() == 1);
    CHECK(controller.project().nodes.size() == 2);
    CHECK(controller.revision() == revision + 1);
    CHECK(controller.history().undo_size() == 1);
    CHECK(canvas.selected_node_id() == original.id);
    record("drag_outside_cancel");
    down(); canvas.itemDragEnter(details); canvas.itemDragMove(details); canvas.itemDropped(details);
    // Repeated delivery and a release back over the source must not add again.
    canvas.itemDropped(details); canvas.finish_library_drag(epoch);
    entry->mouseUp(mouse(*entry, {8, 10}, 0, true)); pump();
    CHECK(clicked_types == std::vector<std::string>{type_id});
    CHECK(dropped_types == std::vector<std::string>{type_id});
    CHECK(controller.project().nodes.size() == 3);
    CHECK(controller.project().nodes.back().id == "dropped");
    CHECK(controller.project().nodes.back().type_id == type_id);
    CHECK(controller.revision() == revision + 2);
    CHECK(controller.history().undo_size() == 2);
    CHECK(controller.history().redo_size() == 0);
    record("drop_once");
    const auto& kept = controller.project().nodes.front();
    CHECK(kept.id == original.id); CHECK(kept.type_id == original.type_id);
    CHECK(kept.parameters == original.parameters); CHECK(kept.x == original.x); CHECK(kept.y == original.y);
    CHECK(kept.opaque_parameters == original.opaque_parameters);
    CHECK(kept.serialized_extensions == original.serialized_extensions);
    CHECK(kept.managed_version == original.managed_version);
    CHECK(controller.project().connections.empty());
    CHECK(controller.project().serialized_extensions == project.serialized_extensions);
    CHECK(controller.project().selected_output_node_id == project.selected_output_node_id);
    CHECK(controller.project().selected_output_port_id == project.selected_output_port_id);
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("behavior_id", "NLC-05.c"); receipt->setProperty("observations", observations);
    receipt->setProperty("clicked_type_id", juce::String(clicked_types.empty() ? "" : clicked_types.front()));
    receipt->setProperty("dropped_type_id", juce::String(dropped_types.empty() ? "" : dropped_types.front()));
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc05c/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout << "NLC05C_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    std::ofstream output(root / "observation.json", std::ios::binary);
    output << juce::JSON::toString(juce::var(receipt.get()), true).toStdString();
    output.close(); REQUIRE(output.good());
}
