#include "audio_node_sdk/NodeSDK.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>
#include <type_traits>

namespace audioalgo {

AudioStreamOutcome AudioNodeProcessor::process_stream(const AudioStreamBlockView&, AudioStreamProgress) {
    return Diagnostic{"stream_processing_unsupported", "Node does not implement the streaming frame protocol"};
}

namespace {

NodeRegistrationDiagnostic invalid(std::string code, std::string field, std::string message) {
    return {std::move(code), std::move(field), std::move(message)};
}

std::optional<NodeRegistrationDiagnostic> validate_descriptor(const NodeDescriptor& descriptor) {
    if (descriptor.type_id.empty() || descriptor.version.empty() || descriptor.display_name.empty())
        return invalid("descriptor_required", "type_id/version/display_name", "Type ID, version, and display name are required");

    std::set<std::string> port_ids;
    std::size_t input_ports = 0;
    std::size_t output_ports = 0;
    for (std::size_t index = 0; index < descriptor.ports.size(); ++index) {
        const auto& port = descriptor.ports[index];
        const auto prefix = "ports[" + std::to_string(index) + "]";
        if (port.id.empty())
            return invalid("port_id_empty", prefix + ".id", "Port ID must not be empty");
        if (port.type_id.empty())
            return invalid("port_type_empty", prefix + ".type_id", "Port type ID must not be empty");
        if (!port_ids.insert(port.id).second)
            return invalid("port_id_duplicate", prefix + ".id", "Port IDs must be unique within a node");
        switch (port.direction) {
        case PortDirection::input: ++input_ports; break;
        case PortDirection::output: ++output_ports; break;
        default: return invalid("port_direction_invalid", prefix + ".direction", "Port direction is unsupported");
        }
    }

    std::set<std::string> parameter_ids;
    for (std::size_t index = 0; index < descriptor.parameters.size(); ++index) {
        const auto& parameter = descriptor.parameters[index];
        const auto prefix = "parameters[" + std::to_string(index) + "]";
        if (parameter.id.empty() || !parameter_ids.insert(parameter.id).second)
            return invalid("parameter_id_invalid", prefix + ".id", "Parameter ID must be non-empty and unique");

        const auto field = "parameters." + parameter.id;
        if (parameter.input_resource && parameter.kind != ParameterKind::path)
            return invalid("parameter_resource_kind", field, "Input resources require a path parameter");
        bool default_matches = false;
        bool numeric = false;
        switch (parameter.kind) {
        case ParameterKind::boolean:
            default_matches = std::holds_alternative<bool>(parameter.default_value);
            break;
        case ParameterKind::integer:
            default_matches = std::holds_alternative<std::int64_t>(parameter.default_value);
            numeric = true;
            break;
        case ParameterKind::floating:
            default_matches = std::holds_alternative<double>(parameter.default_value);
            numeric = true;
            break;
        case ParameterKind::enumeration:
        case ParameterKind::string:
        case ParameterKind::path:
            default_matches = std::holds_alternative<std::string>(parameter.default_value);
            break;
        default:
            return invalid("parameter_kind_unsupported", field + ".kind", "Parameter kind is unsupported");
        }
        if (!default_matches)
            return invalid("parameter_default_type", field + ".default_value", "Default value does not match the declared parameter kind");

        if (parameter.minimum && !std::isfinite(*parameter.minimum))
            return invalid("parameter_bound_non_finite", field + ".minimum", "Minimum must be finite");
        if (parameter.maximum && !std::isfinite(*parameter.maximum))
            return invalid("parameter_bound_non_finite", field + ".maximum", "Maximum must be finite");
        if (!numeric && (parameter.minimum || parameter.maximum))
            return invalid("parameter_range_kind", field + ".minimum/maximum", "Only integer and floating parameters may declare numeric bounds");
        if (parameter.step && !numeric)
            return invalid("parameter_step_kind", field + ".step", "Only integer and floating parameters may declare a step");
        if (parameter.step && (!std::isfinite(*parameter.step) || *parameter.step <= 0.0))
            return invalid("parameter_step_invalid", field + ".step", "Parameter step must be finite and greater than zero");
        if (parameter.minimum && parameter.maximum && *parameter.minimum > *parameter.maximum)
            return invalid("parameter_range_invalid", field + ".minimum", "Minimum must not exceed maximum");
        if (parameter.kind == ParameterKind::integer) {
            if ((parameter.minimum && std::trunc(*parameter.minimum) != *parameter.minimum) ||
                (parameter.maximum && std::trunc(*parameter.maximum) != *parameter.maximum))
                return invalid("parameter_integer_bound", field + ".minimum/maximum", "Integer parameter bounds must be whole numbers");
            if (parameter.step && std::trunc(*parameter.step) != *parameter.step)
                return invalid("parameter_integer_step", field + ".step", "Integer parameter step must be a whole number");
        }

        if (parameter.kind == ParameterKind::enumeration) {
            std::set<std::string> choice_values;
            for (std::size_t choice_index = 0; choice_index < parameter.choices.size(); ++choice_index) {
                const auto& choice = parameter.choices[choice_index];
                const auto choice_field = field + ".choices[" + std::to_string(choice_index) + "]";
                if (choice.value.empty() || !choice_values.insert(choice.value).second)
                    return invalid("enum_value_invalid", choice_field + ".value", "Enum stable values must be non-empty and unique");
                if (choice.label.empty())
                    return invalid("enum_label_empty", choice_field + ".label", "Enum display labels must not be empty");
            }
            if (parameter.choices.empty())
                return invalid("enum_choices_empty", field + ".choices", "Enum parameters require at least one stable choice");
        } else if (!parameter.choices.empty()) {
            return invalid("parameter_choices_kind", field + ".choices", "Only enum parameters may declare choices");
        }

        // Range and enum membership rules live in one shared helper; only the field/code naming
        // differs between a descriptor default and a live instance value.
        if (const auto value_error = validate_parameter_value(parameter, parameter.default_value)) {
            auto remapped = *value_error;
            remapped.field = field + ".default_value";
            if (remapped.code == "parameter_type")
                remapped.code = "parameter_default_type";
            else if (remapped.code == "parameter_non_finite")
                remapped.code = "parameter_default_non_finite";
            else if (remapped.code == "parameter_out_of_range")
                remapped.code = "parameter_default_out_of_range";
            else if (remapped.code == "parameter_enum_unknown")
                remapped.code = "enum_default_unknown";
            return remapped;
        }
    }

    for (const auto& parameter : descriptor.parameters) {
        for (const auto& [name, condition] : {
                 std::pair{"visible_if", &parameter.visible_if},
                 std::pair{"enabled_if", &parameter.enabled_if}}) {
            if (!*condition)
                continue;
            const auto field = "parameters." + parameter.id + "." + name;
            const auto source = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                [&](const auto& candidate) { return candidate.id == (*condition)->parameter_id; });
            if (source == descriptor.parameters.end())
                return invalid("parameter_condition_reference", field, "Condition must reference a declared parameter");
            if (const auto error = validate_parameter_value(*source, (*condition)->equals))
                return invalid("parameter_condition_value", field + ".equals", error->message);
        }
    }

    const auto validate_positive_unique = [](const auto& values, const std::string& field)
        -> std::optional<NodeRegistrationDiagnostic> {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        std::set<Value> seen;
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (values[index] == 0)
                return invalid("format_value_invalid", field + "[" + std::to_string(index) + "]", "Format values must be positive");
            if (!seen.insert(values[index]).second)
                return invalid("format_value_duplicate", field + "[" + std::to_string(index) + "]", "Format values must be unique");
        }
        return std::nullopt;
    };
    if (auto error = validate_positive_unique(descriptor.audio_format.sample_rates_hz, "audio_format.sample_rates_hz"))
        return error;
    if (auto error = validate_positive_unique(descriptor.audio_format.channel_counts, "audio_format.channel_counts"))
        return error;

    const auto& capabilities = descriptor.capabilities;
    if (capabilities.uses_streaming_frame_api && (capabilities.supports_variable_block_size ||
        capabilities.fixed_block_frames == 0 ||
        capabilities.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad))
        return invalid("stream_frame_profile_required", "capabilities.uses_streaming_frame_api", "Streaming frame API requires an explicit zero-padded fixed frame");
    if (capabilities.uses_streaming_frame_api && (capabilities.maximum_no_progress_calls == 0 || capabilities.maximum_no_progress_calls > 1024))
        return invalid("stream_progress_budget_invalid", "capabilities.maximum_no_progress_calls", "No-progress budget must be 1..1024");
    if (capabilities.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::reject_partial &&
        capabilities.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad)
        return invalid("capability_eos_policy_invalid", "capabilities.fixed_frame_end_policy", "Unknown fixed-frame EOS policy");
    if (capabilities.supports_variable_block_size &&
        capabilities.fixed_frame_end_policy != NodeExecutionCapabilities::FixedFrameEndPolicy::reject_partial)
        return invalid("capability_eos_policy_conflict", "capabilities.fixed_frame_end_policy", "Padding policy requires an explicitly fixed frame size");
    if (capabilities.maximum_audio_inputs && capabilities.minimum_audio_inputs > *capabilities.maximum_audio_inputs)
        return invalid("capability_range_invalid", "capabilities.minimum_audio_inputs", "Minimum input count must not exceed maximum input count");
    if (capabilities.maximum_audio_outputs && capabilities.minimum_audio_outputs > *capabilities.maximum_audio_outputs)
        return invalid("capability_range_invalid", "capabilities.minimum_audio_outputs", "Minimum output count must not exceed maximum output count");
    if (input_ports < capabilities.minimum_audio_inputs ||
        (capabilities.maximum_audio_inputs && input_ports > *capabilities.maximum_audio_inputs))
        return invalid("capability_port_count", "capabilities.maximum_audio_inputs", "Declared input ports do not fit the input capability range");
    if (output_ports < capabilities.minimum_audio_outputs ||
        (capabilities.maximum_audio_outputs && output_ports > *capabilities.maximum_audio_outputs))
        return invalid("capability_port_count", "capabilities.maximum_audio_outputs", "Declared output ports do not fit the output capability range");
    if (capabilities.supports_variable_block_size && capabilities.fixed_block_frames != 0)
        return invalid("capability_block_size_conflict", "capabilities.fixed_block_frames", "Variable-block capability cannot also require a fixed frame count");
    if (!capabilities.supports_variable_block_size && capabilities.fixed_block_frames == 0)
        return invalid("capability_fixed_block_missing", "capabilities.fixed_block_frames", "Fixed-block capability requires a positive frame count");

    switch (descriptor.execution_kind) {
    case NodeExecutionKind::source:
    case NodeExecutionKind::block:
    case NodeExecutionKind::sink:
        break;
    default:
        return invalid("execution_kind_unsupported", "execution_kind", "Execution role is unsupported");
    }
    return std::nullopt;
}

} // namespace

std::optional<NodeRegistrationDiagnostic> validate_parameter_value(const ParameterDescriptor& parameter,
                                                                  const ParameterValue& value) {
    const auto field = "parameters." + parameter.id;
    bool matches = false;
    bool numeric = false;
    switch (parameter.kind) {
    case ParameterKind::boolean:
        matches = std::holds_alternative<bool>(value);
        break;
    case ParameterKind::integer:
        matches = std::holds_alternative<std::int64_t>(value);
        numeric = true;
        break;
    case ParameterKind::floating:
        matches = std::holds_alternative<double>(value);
        numeric = true;
        break;
    case ParameterKind::enumeration:
    case ParameterKind::string:
    case ParameterKind::path:
        matches = std::holds_alternative<std::string>(value);
        break;
    default:
        return invalid("parameter_kind_unsupported", field + ".kind", "Parameter kind is unsupported");
    }
    if (!matches)
        return invalid("parameter_type", field, "Parameter value does not match the declared parameter kind");

    if (numeric) {
        const auto number = parameter.kind == ParameterKind::floating
            ? std::get<double>(value)
            : static_cast<double>(std::get<std::int64_t>(value));
        if (!std::isfinite(number))
            return invalid("parameter_non_finite", field, "Parameter must be finite");
        if ((parameter.minimum && number < *parameter.minimum) ||
            (parameter.maximum && number > *parameter.maximum))
            return invalid("parameter_out_of_range", field, "Parameter is outside its described range");
    }

    if (parameter.kind == ParameterKind::enumeration) {
        const auto& text = std::get<std::string>(value);
        const auto found = std::find_if(parameter.choices.begin(), parameter.choices.end(),
                                        [&](const ParameterDescriptor::Choice& choice) {
                                            return choice.value == text;
                                        });
        if (found == parameter.choices.end())
            return invalid("parameter_enum_unknown", field,
                           "Parameter value is not one of the declared stable choices");
    }
    return std::nullopt;
}

AudioBlockOutcome AudioNodeProcessor::process_block(const AudioBlockView& block) {
    (void)block;
    return Diagnostic{"block_processing_unsupported",
                      "This node does not implement the allocation-free block interface"};
}

AudioBlockOutcome AudioNodeProcessor::process_named_block(const NamedAudioBlockView& block) {
    (void)block;
    return Diagnostic{"named_block_processing_unsupported",
                      "This node does not implement the borrowed named audio port interface"};
}

std::optional<Diagnostic> AudioNodeProcessor::prepare(const NodePrepareContext& context) {
    (void)context;
    return std::nullopt;
}

NodeRegistrationResult NodeRegistry::register_node(NodeDescriptor descriptor, NodeFactory factory) {
    if (!factory)
        return {false, invalid("factory_missing", "factory", "Node factory must be provided")};
    if (auto error = validate_descriptor(descriptor))
        return {false, std::move(error)};

    const auto type_id = descriptor.type_id;
    if (descriptors_.contains(type_id) || factories_.contains(type_id))
        return {false, invalid("node_type_duplicate", "type_id", "Node type is already registered")};

    const auto [descriptor_it, descriptor_inserted] = descriptors_.emplace(type_id, std::move(descriptor));
    if (!descriptor_inserted)
        return {false, invalid("node_type_duplicate", "type_id", "Node type is already registered")};
    try {
        const auto [factory_it, factory_inserted] = factories_.emplace(type_id, std::move(factory));
        (void)factory_it;
        if (!factory_inserted) {
            descriptors_.erase(descriptor_it);
            return {false, invalid("factory_duplicate", "factory", "Node factory is already registered")};
        }
    } catch (...) {
        descriptors_.erase(descriptor_it);
        return {false, invalid("registration_failed", "factory", "Node factory could not be registered")};
    }
    return {true, std::nullopt};
}

bool NodeRegistry::register_descriptor(NodeDescriptor descriptor) {
    if (validate_descriptor(descriptor) || descriptors_.contains(descriptor.type_id))
        return false;
    descriptors_.emplace(descriptor.type_id, std::move(descriptor));
    return true;
}

bool NodeRegistry::register_factory(std::string type_id, NodeFactory factory) {
    if (type_id.empty() || !factory || !descriptors_.contains(type_id) || factories_.contains(type_id))
        return false;
    factories_.emplace(std::move(type_id), std::move(factory));
    return true;
}

const NodeDescriptor* NodeRegistry::find(const std::string& type_id) const noexcept {
    const auto it = descriptors_.find(type_id);
    return it == descriptors_.end() ? nullptr : &it->second;
}

NodeProcessorCreationResult NodeRegistry::create_processor(const std::string& type_id,
                                                             const std::string& node_id) const {
    if (node_id.empty())
        return {{}, NodeFactoryDiagnostic{"node_id_empty", {}, "Processor creation requires a node ID"}};
    const auto it = factories_.find(type_id);
    if (it == factories_.end())
        return {{}, NodeFactoryDiagnostic{"node_factory_missing", node_id,
                                         "Node " + node_id + " has no registered processor factory for " + type_id}};
    try {
        auto processor = it->second();
        if (!processor)
            return {{}, NodeFactoryDiagnostic{"node_factory_returned_null", node_id,
                                             "Node " + node_id + " processor factory returned no instance"}};
        return {std::move(processor), std::nullopt};
    } catch (const std::exception& error) {
        return {{}, NodeFactoryDiagnostic{"node_factory_threw", node_id,
                                         "Node " + node_id + " processor factory failed: " + error.what()}};
    } catch (...) {
        return {{}, NodeFactoryDiagnostic{"node_factory_threw", node_id,
                                         "Node " + node_id + " processor factory failed with an unknown exception"}};
    }
}

NodeProcessorPlanOutcome NodeRegistry::create_plan(const std::vector<NodeProcessorRequest>& requests) const {
    NodeProcessorPlan plan;
    plan.processors.reserve(requests.size());
    for (const auto& request : requests) {
        auto result = create_processor(request.type_id, request.node_id);
        if (result.diagnostic)
            return std::move(*result.diagnostic);
        plan.processors.push_back({request.node_id, request.type_id, request.parameters, std::move(result.processor)});
    }
    return plan;
}

} // namespace audioalgo
