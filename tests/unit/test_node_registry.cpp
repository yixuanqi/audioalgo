#include "audio_node_sdk/NodeSDK.h"
#include "audio_project/ProjectModel.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

TEST_CASE("AR-12.a ChannelSwap joins the shared catalog through plugin composition", "[AR-12.a][R4]") {
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    const auto* descriptor = registry.find("example.cpp_channel_swap");
    REQUIRE(descriptor != nullptr);
    CHECK(descriptor->display_name == "Channel Swap");
    CHECK(descriptor->audio_format.channel_counts == std::vector<std::size_t>{2});
    REQUIRE(descriptor->parameters.size() == 1);
    CHECK(descriptor->parameters.front().id == "enabled");
    CHECK(descriptor->parameters.front().default_value == audioalgo::ParameterValue{true});
    const auto processor = registry.create_processor(descriptor->type_id, "new-swap-instance");
    CHECK(processor.processor != nullptr);
    CHECK_FALSE(processor.diagnostic.has_value());
}

namespace {

class RegistryProbeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::AudioPorts{};
    }
};

class StatefulRegistryProbeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        ++processed_blocks;
        return audioalgo::AudioPorts{};
    }

    std::size_t processed_blocks = 0;
};

class DestructionProbeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit DestructionProbeProcessor(std::shared_ptr<std::size_t> destruction_count)
        : destruction_count_(std::move(destruction_count)) {}
    ~DestructionProbeProcessor() override { ++*destruction_count_; }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::AudioPorts{};
    }

private:
    std::shared_ptr<std::size_t> destruction_count_;
};

audioalgo::NodeDescriptor probe_descriptor(std::string type_id = "test.atomic_node",
                                           std::string version = "1.0.0") {
    return {std::move(type_id), std::move(version), "Atomic node",
            {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
             {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
}

audioalgo::NodeFactory probe_factory() {
    return [] { return std::make_unique<RegistryProbeProcessor>(); };
}

} // namespace

namespace {
template <typename T>
concept CanCreateNodeProcessors = requires(const T& catalog) {
    catalog.create_processor(std::string{}, std::string{});
};

static_assert(!CanCreateNodeProcessors<audioalgo::NodeCatalog>);
} // namespace

TEST_CASE("AR-06.a failed paired registration leaves the descriptor and factory catalog unchanged", "[R1][AR-06.a]") {
    audioalgo::NodeRegistry registry;
    const auto descriptor = probe_descriptor();

    REQUIRE(registry.register_node(descriptor, probe_factory()).registered);
    REQUIRE(registry.descriptors().size() == 1);
    REQUIRE(registry.find(descriptor.type_id) != nullptr);
    REQUIRE(registry.create_processor(descriptor.type_id, "node-atomic").processor != nullptr);

    auto conflicting_version = probe_descriptor(descriptor.type_id, "2.0.0");
    const auto conflict = registry.register_node(std::move(conflicting_version), probe_factory());
    CHECK_FALSE(conflict.registered);
    REQUIRE(conflict.diagnostic.has_value());
    CHECK(conflict.diagnostic->field == "type_id");
    CHECK(registry.descriptors().size() == 1);
    CHECK(registry.find(descriptor.type_id)->version == "1.0.0");
    CHECK(registry.create_processor(descriptor.type_id, "node-atomic").processor != nullptr);

    auto empty_factory_descriptor = probe_descriptor("test.empty_factory");
    const auto empty_factory = registry.register_node(empty_factory_descriptor, {});
    CHECK_FALSE(empty_factory.registered);
    REQUIRE(empty_factory.diagnostic.has_value());
    CHECK(empty_factory.diagnostic->field == "factory");
    CHECK(registry.find(empty_factory_descriptor.type_id) == nullptr);
    CHECK(registry.create_processor(empty_factory_descriptor.type_id, "node-empty-factory").processor == nullptr);
    CHECK(registry.descriptors().size() == 1);

    auto invalid_ports = probe_descriptor("test.invalid_ports");
    invalid_ports.ports[1].id = invalid_ports.ports[0].id;
    const auto duplicate_port = registry.register_node(invalid_ports, probe_factory());
    CHECK_FALSE(duplicate_port.registered);
    REQUIRE(duplicate_port.diagnostic.has_value());
    CHECK(duplicate_port.diagnostic->field == "ports[1].id");
    CHECK(registry.find(invalid_ports.type_id) == nullptr);
    CHECK(registry.create_processor(invalid_ports.type_id, "node-invalid-port").processor == nullptr);
    CHECK(registry.descriptors().size() == 1);
    CHECK(registry.create_processor(descriptor.type_id, "node-atomic").processor != nullptr);
}

TEST_CASE("AR-06.b descriptor validation rejects invalid schema and names its field", "[R1][AR-06.b]") {
    const auto rejects = [](audioalgo::NodeDescriptor descriptor, const std::string& expected_field) {
        audioalgo::NodeRegistry registry;
        const auto result = registry.register_node(std::move(descriptor), probe_factory());
        CHECK_FALSE(result.registered);
        REQUIRE(result.diagnostic.has_value());
        CHECK(result.diagnostic->field == expected_field);
        CHECK(registry.descriptors().empty());
        CHECK(registry.create_processor("test.invalid", "node-invalid").processor == nullptr);
    };

    auto wrong_default = probe_descriptor("test.invalid");
    wrong_default.parameters.push_back({"amount", audioalgo::ParameterKind::floating,
                                       std::int64_t{1}, 0.0, 2.0, "units"});
    rejects(std::move(wrong_default), "parameters.amount.default_value");

    auto reversed_range = probe_descriptor("test.invalid");
    reversed_range.parameters.push_back({"amount", audioalgo::ParameterKind::floating,
                                         1.0, 2.0, -2.0, "units"});
    rejects(std::move(reversed_range), "parameters.amount.minimum");

    auto default_out_of_range = probe_descriptor("test.invalid");
    default_out_of_range.parameters.push_back({"amount", audioalgo::ParameterKind::integer,
                                               std::int64_t{3}, 0.0, 2.0, "units"});
    rejects(std::move(default_out_of_range), "parameters.amount.default_value");

    auto zero_parameter_step = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor zero_step{"amount", audioalgo::ParameterKind::floating,
                                              0.5, 0.0, 1.0, "units"};
    zero_step.step = 0.0;
    zero_parameter_step.parameters.push_back(std::move(zero_step));
    rejects(std::move(zero_parameter_step), "parameters.amount.step");

    auto non_finite_parameter_step = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor non_finite_step{"amount", audioalgo::ParameterKind::floating,
                                                    0.5, 0.0, 1.0, "units"};
    non_finite_step.step = std::numeric_limits<double>::infinity();
    non_finite_parameter_step.parameters.push_back(std::move(non_finite_step));
    rejects(std::move(non_finite_parameter_step), "parameters.amount.step");

    auto fractional_integer_step = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor integer_step{"amount", audioalgo::ParameterKind::integer,
                                                 std::int64_t{1}, 0.0, 4.0, "units"};
    integer_step.step = 0.5;
    fractional_integer_step.parameters.push_back(std::move(integer_step));
    rejects(std::move(fractional_integer_step), "parameters.amount.step");

    auto non_numeric_parameter_step = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor string_step{"amount", audioalgo::ParameterKind::string,
                                                std::string("value"), {}, {}, {}};
    string_step.step = 1.0;
    non_numeric_parameter_step.parameters.push_back(std::move(string_step));
    rejects(std::move(non_numeric_parameter_step), "parameters.amount.step");

    auto duplicate_parameter = probe_descriptor("test.invalid");
    duplicate_parameter.parameters = {
        {"amount", audioalgo::ParameterKind::floating, 0.0, {}, {}, "units"},
        {"amount", audioalgo::ParameterKind::integer, std::int64_t{0}, {}, {}, "units"}};
    rejects(std::move(duplicate_parameter), "parameters[1].id");

    auto unknown_enum_default = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor mode{"mode", audioalgo::ParameterKind::enumeration,
                                        std::string("invert"), {}, {}, {}};
    mode.choices = {{"normal", "Normal"}, {"invert", "Invert"}};
    unknown_enum_default.parameters.push_back(std::move(mode));
    std::get<std::string>(unknown_enum_default.parameters.back().default_value) = "other";
    rejects(std::move(unknown_enum_default), "parameters.mode.default_value");

    auto duplicate_enum_values = probe_descriptor("test.invalid");
    audioalgo::ParameterDescriptor polarity{"polarity", audioalgo::ParameterKind::enumeration,
                                            std::string("normal"), {}, {}, {}};
    polarity.choices = {{"normal", "Normal"}, {"normal", "Same stable value"}};
    duplicate_enum_values.parameters.push_back(std::move(polarity));
    rejects(std::move(duplicate_enum_values), "parameters.polarity.choices[1].value");

    auto zero_sample_rate = probe_descriptor("test.invalid");
    zero_sample_rate.audio_format.sample_rates_hz = {48000, 0};
    rejects(std::move(zero_sample_rate), "audio_format.sample_rates_hz[1]");

    auto zero_channel_count = probe_descriptor("test.invalid");
    zero_channel_count.audio_format.channel_counts = {2, 0};
    rejects(std::move(zero_channel_count), "audio_format.channel_counts[1]");

    auto inconsistent_port_capability = probe_descriptor("test.invalid");
    inconsistent_port_capability.capabilities.minimum_audio_inputs = 2;
    inconsistent_port_capability.capabilities.maximum_audio_inputs = 1;
    rejects(std::move(inconsistent_port_capability), "capabilities.minimum_audio_inputs");

    auto port_outside_capability = probe_descriptor("test.invalid");
    port_outside_capability.capabilities.maximum_audio_outputs = 0;
    rejects(std::move(port_outside_capability), "capabilities.maximum_audio_outputs");

    auto conflicting_block_contract = probe_descriptor("test.invalid");
    conflicting_block_contract.capabilities.fixed_block_frames = 512;
    rejects(std::move(conflicting_block_contract), "capabilities.fixed_block_frames");

    auto missing_fixed_block = probe_descriptor("test.invalid");
    missing_fixed_block.capabilities.supports_variable_block_size = false;
    rejects(std::move(missing_fixed_block), "capabilities.fixed_block_frames");

    auto valid = probe_descriptor("test.valid_schema");
    audioalgo::ParameterDescriptor valid_amount{"amount", audioalgo::ParameterKind::floating,
                                                 0.5, 0.0, 1.0, "units"};
    valid_amount.step = 0.25;
    valid.parameters.push_back(std::move(valid_amount));
    audioalgo::ParameterDescriptor valid_polarity{"polarity", audioalgo::ParameterKind::enumeration,
                                                  std::string("normal"), {}, {}, {}};
    valid_polarity.choices = {{"normal", "Normal"}, {"invert", "Invert"}};
    valid.parameters.push_back(std::move(valid_polarity));
    valid.audio_format.sample_rates_hz = {16000, 48000};
    valid.audio_format.channel_counts = {1, 2, 4};
    valid.capabilities.supports_variable_block_size = false;
    valid.capabilities.fixed_block_frames = 512;
    audioalgo::NodeRegistry valid_registry;
    const auto result = valid_registry.register_node(std::move(valid), probe_factory());
    CHECK(result.registered);
    CHECK_FALSE(result.diagnostic.has_value());
    CHECK(valid_registry.descriptors().size() == 1);
}

TEST_CASE("AR-06.c node instances keep state separate and factory failures stay inside the SDK", "[R1][AR-06.c]") {
    audioalgo::NodeRegistry registry;
    const auto descriptor = probe_descriptor("test.stateful_node");
    REQUIRE(registry.register_node(descriptor, [] {
        return std::make_unique<StatefulRegistryProbeProcessor>();
    }).registered);

    auto first_result = registry.create_processor(descriptor.type_id, "node-first");
    auto second_result = registry.create_processor(descriptor.type_id, "node-second");
    REQUIRE_FALSE(first_result.diagnostic.has_value());
    REQUIRE_FALSE(second_result.diagnostic.has_value());
    auto& first = first_result.processor;
    auto& second = second_result.processor;
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    auto* first_state = dynamic_cast<StatefulRegistryProbeProcessor*>(first.get());
    auto* second_state = dynamic_cast<StatefulRegistryProbeProcessor*>(second.get());
    REQUIRE(first_state != nullptr);
    REQUIRE(second_state != nullptr);
    CHECK(first_state != second_state);

    audioalgo::NodeParameters parameters;
    audioalgo::AudioPorts ports;
    audioalgo::NodeExecutionContext context;
    const audioalgo::NodeProcessRequest request{parameters, ports, context};
    (void)first->process(request);
    (void)first->process(request);
    CHECK(first_state->processed_blocks == 2);
    CHECK(second_state->processed_blocks == 0);

    audioalgo::NodeRegistry throwing_registry;
    auto destroyed = std::make_shared<std::size_t>(0);
    const auto good_descriptor = probe_descriptor("test.plan_good");
    REQUIRE(throwing_registry.register_node(good_descriptor, [destroyed] {
        return std::make_unique<DestructionProbeProcessor>(destroyed);
    }).registered);
    const auto throwing_descriptor = probe_descriptor("test.throwing_node");
    REQUIRE(throwing_registry.register_node(throwing_descriptor, []() -> std::unique_ptr<audioalgo::AudioNodeProcessor> {
        throw std::runtime_error("test factory failure");
    }).registered);
    const auto outcome = throwing_registry.create_plan({
        {"node-created-before-failure", good_descriptor.type_id},
        {"node-throwing", throwing_descriptor.type_id}});
    REQUIRE(std::holds_alternative<audioalgo::NodeFactoryDiagnostic>(outcome));
    const auto& diagnostic = std::get<audioalgo::NodeFactoryDiagnostic>(outcome);
    CHECK(diagnostic.code == "node_factory_threw");
    CHECK(diagnostic.node_id == "node-throwing");
    CHECK(diagnostic.message.find("test factory failure") != std::string::npos);
    CHECK(*destroyed == 1);
}

TEST_CASE("AR-06.d plugin composition uses one catalog and exposes metadata read-only", "[R1][AR-06.d]") {
    audioalgo::NodeRegistry editor_registry;
    audioalgo::NodeRegistry cli_registry;
    REQUIRE(audioalgo::register_builtin_nodes(editor_registry));
    REQUIRE(audioalgo::register_builtin_nodes(cli_registry));

    const audioalgo::NodeCatalog& editor_catalog = editor_registry;
    const audioalgo::NodeCatalog& cli_catalog = cli_registry;
    // 5 descriptors since A2 added the production builtin.mixer to the builtin catalog.
    REQUIRE(editor_catalog.descriptors().size() == 5);
    REQUIRE(cli_catalog.descriptors().size() == 5);
    CHECK(editor_catalog.descriptors().size() == cli_catalog.descriptors().size());

    auto editor_descriptor = editor_catalog.descriptors().begin();
    auto cli_descriptor = cli_catalog.descriptors().begin();
    for (; editor_descriptor != editor_catalog.descriptors().end(); ++editor_descriptor, ++cli_descriptor) {
        CHECK(editor_descriptor->first == cli_descriptor->first);
        CHECK(editor_descriptor->second.version == cli_descriptor->second.version);
        CHECK(editor_descriptor->second.display_name == cli_descriptor->second.display_name);
        CHECK(editor_catalog.find(editor_descriptor->first) != nullptr);
        CHECK(cli_catalog.find(editor_descriptor->first) != nullptr);
    }
    CHECK(editor_catalog.find("builtin.file_source") != nullptr);
    CHECK(editor_catalog.find("builtin.gain") != nullptr);
    CHECK(editor_catalog.find("builtin.mixer") != nullptr);
    CHECK(editor_catalog.find("builtin.output") != nullptr);
    CHECK(editor_catalog.find("example.cpp_channel_swap") != nullptr);
    CHECK(cli_catalog.find("example.cpp_channel_swap") != nullptr);

    auto editor_gain = editor_registry.create_processor("builtin.gain", "editor-gain");
    auto cli_gain = cli_registry.create_processor("builtin.gain", "cli-gain");
    REQUIRE_FALSE(editor_gain.diagnostic.has_value());
    REQUIRE_FALSE(cli_gain.diagnostic.has_value());
    REQUIRE(editor_gain.processor != nullptr);
    REQUIRE(cli_gain.processor != nullptr);
    CHECK(editor_gain.processor.get() != cli_gain.processor.get());
}

TEST_CASE("AR-06.b enum parameter instances must use a declared stable choice", "[R1][AR-06.b]") {
    audioalgo::NodeRegistry registry;
    audioalgo::ParameterDescriptor mode{"mode", audioalgo::ParameterKind::enumeration,
                                        std::string("normal"), {}, {}, {}};
    mode.choices = {{"normal", "Normal"}, {"invert", "Invert"}};
    const audioalgo::NodeDescriptor descriptor{"test.enum_node", "1.0.0", "Enum node", {},
                                               {mode}, audioalgo::NodeExecutionKind::block};
    REQUIRE(registry.register_node(descriptor, probe_factory()).registered);

    // One shared value-schema helper is used by descriptor registration, project validation and runtime.
    CHECK_FALSE(audioalgo::validate_parameter_value(mode, std::string("normal")).has_value());
    CHECK_FALSE(audioalgo::validate_parameter_value(mode, std::string("invert")).has_value());
    const auto rejected_value = audioalgo::validate_parameter_value(mode, std::string("INVALID"));
    REQUIRE(rejected_value.has_value());
    CHECK(rejected_value->code == "parameter_enum_unknown");
    CHECK(rejected_value->field == "parameters.mode");
    CHECK(audioalgo::validate_parameter_value(mode, std::int64_t{0})->code == "parameter_type");

    const auto project_with = [&](audioalgo::ParameterValue value) {
        audioalgo::ProjectModel project;
        project.project_id = "enum-regression";
        project.name = "enum regression";
        audioalgo::NodeInstance node{"enum-node", descriptor.type_id, {}, 0.0, 0.0};
        node.parameters.emplace("mode", std::move(value));
        project.nodes.push_back(std::move(node));
        return project;
    };

    const auto invalid = audioalgo::validate_project(project_with(std::string("INVALID")), registry);
    REQUIRE(invalid.size() == 1);
    CHECK(invalid.front().code == "parameter_enum_unknown");
    CHECK(invalid.front().node_id == "enum-node");
    CHECK(invalid.front().port_id == "mode");

    CHECK(audioalgo::validate_project(project_with(std::string("invert")), registry).empty());
    CHECK(audioalgo::validate_project(project_with(std::string("normal")), registry).empty());

    const auto wrong_type = audioalgo::validate_project(project_with(std::int64_t{1}), registry);
    REQUIRE(wrong_type.size() == 1);
    CHECK(wrong_type.front().code == "parameter_type");

    audioalgo::ProjectModel without_value;
    without_value.project_id = "enum-regression";
    without_value.name = "enum regression";
    without_value.nodes.push_back({"enum-node", descriptor.type_id, {}, 0.0, 0.0});
    CHECK(audioalgo::validate_project(without_value, registry).empty());
}
