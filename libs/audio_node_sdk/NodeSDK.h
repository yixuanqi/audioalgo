#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <functional>
#include <map>
#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace audioalgo {

enum class PortDirection { input, output };
enum class ParameterKind { boolean, integer, floating, enumeration, string, path };
enum class NodeExecutionKind { source, block, sink };

using ParameterValue = std::variant<bool, std::int64_t, double, std::string>;
using NodeParameters = std::map<std::string, ParameterValue, std::less<>>;

struct ParameterCondition {
    std::string parameter_id;
    ParameterValue equals;
};

struct PortDescriptor {
    std::string id;
    std::string type_id;
    PortDirection direction;
    bool required = false;
};

struct ParameterDescriptor {
    std::string id;
    ParameterKind kind;
    ParameterValue default_value;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::string unit;
    struct Choice {
        std::string value;
        std::string label;
    };
    std::vector<Choice> choices;
    std::optional<double> step;
    std::string display_name;
    std::string help;
    // Explicit input resource semantics; ordinary path parameters may also name outputs.
    bool input_resource = false;
    // A single typed equality is deliberately the only supported presentation condition.
    std::optional<ParameterCondition> visible_if;
    std::optional<ParameterCondition> enabled_if;
};

struct NodeAudioFormatConstraint {
    // Empty lists mean that the node accepts any value for that dimension.
    std::vector<std::uint32_t> sample_rates_hz;
    std::vector<std::size_t> channel_counts;
};

struct NodeExecutionCapabilities {
    // An unset maximum means "derive it from the ports the descriptor actually declares".
    // Explicit maximums are still enforced, so a node can declare a smaller range than its ports.
    std::size_t minimum_audio_inputs = 0;
    std::optional<std::size_t> maximum_audio_inputs;
    std::size_t minimum_audio_outputs = 0;
    std::optional<std::size_t> maximum_audio_outputs;
    bool supports_variable_block_size = true;
    std::size_t fixed_block_frames = 0;
    bool supports_named_audio_ports = false;
};

struct NodeDescriptor {
    std::string type_id;
    std::string version;
    std::string display_name;
    std::vector<PortDescriptor> ports;
    std::vector<ParameterDescriptor> parameters;
    NodeExecutionKind execution_kind = NodeExecutionKind::block;
    NodeAudioFormatConstraint audio_format;
    NodeExecutionCapabilities capabilities;
};

struct NodeRegistrationDiagnostic {
    std::string code;
    std::string field;
    std::string message;
};

struct NodeRegistrationResult {
    bool registered = false;
    std::optional<NodeRegistrationDiagnostic> diagnostic;
};

// Single source of truth for "is this parameter value acceptable for this descriptor?".
// Descriptor registration, project validation, controller commands and the runtime all share it.
// The returned diagnostic uses the graph-facing codes parameter_type, parameter_non_finite,
// parameter_out_of_range and parameter_enum_unknown, with field "parameters.<id>".
[[nodiscard]] std::optional<NodeRegistrationDiagnostic> validate_parameter_value(
    const ParameterDescriptor& descriptor, const ParameterValue& value);

using AudioPorts = std::map<std::string, AudioBuffer, std::less<>>;

struct NodeExecutionContext {
    std::string project_file;
    std::string output_directory;
};

struct NodeProcessRequest {
    const NodeParameters& parameters;
    const AudioPorts& inputs;
    const NodeExecutionContext& context;
};

using NodeProcessOutcome = std::variant<AudioPorts, Diagnostic>;

struct NodePrepareContext {
    AudioFormat format{};
    std::size_t maximum_block_frames = 0;
    // Validated parameter snapshot owned by the caller; null when the caller has no parameters.
    const NodeParameters* parameters = nullptr;
};

// Borrowed, non-owning block views. The channel pointers and their samples stay valid only for
// the duration of the process_block() call; a node must never retain them.
struct AudioBlockView {
    std::uint32_t sample_rate_hz = 0;
    std::size_t frame_count = 0;
    std::size_t channel_count = 0;
    const float* const* input_channels = nullptr;
    float* const* output_channels = nullptr;
};

struct AudioBlockResult {
    std::size_t processed_frames = 0;
};

using AudioBlockOutcome = std::variant<AudioBlockResult, Diagnostic>;

struct AudioInputPortBlock {
    std::string_view id;
    std::span<const float* const> channels;
};

struct AudioOutputPortBlock {
    std::string_view id;
    std::span<float* const> channels;
};

// Every group has frame_count samples per channel at the common sample rate.
// Names, channel-pointer spans and samples are borrowed only for this call.
// A full-block result covers every declared output group. No views may escape.
struct NamedAudioBlockView {
    std::uint32_t sample_rate_hz = 0;
    std::size_t frame_count = 0;
    std::span<const AudioInputPortBlock> inputs;
    std::span<const AudioOutputPortBlock> outputs;
};

class AudioNodeProcessor {
public:
    virtual ~AudioNodeProcessor() = default;
    // Owned-block entry point used by tasks that need whole ports (source/sink, file I/O, geometry).
    [[nodiscard]] virtual NodeProcessOutcome process(const NodeProcessRequest& request) = 0;

    // True only when process_block() runs without allocating, copying whole buffers or doing I/O.
    [[nodiscard]] virtual bool supports_block_processing() const noexcept { return false; }

    // True when process_block() may receive identical input and output storage. Node contracts that
    // forbid aliasing (the default) make the wrapper stage inputs in preallocated scratch memory.
    [[nodiscard]] virtual bool supports_in_place_block_processing() const noexcept { return false; }

    // Allocation-free block path used by the native wrapper. The default reports the capability as
    // unsupported so the wrapper can fall back to process() instead of silently passing audio through.
    [[nodiscard]] virtual AudioBlockOutcome process_block(const AudioBlockView& block);

    [[nodiscard]] virtual bool supports_named_block_processing() const noexcept { return false; }
    [[nodiscard]] virtual AudioBlockOutcome process_named_block(const NamedAudioBlockView& block);

    // Plan-time preparation. Allocation is allowed here and never inside process_block().
    // Called once per prepared run with the validated format and the run's maximum block length.
    [[nodiscard]] virtual std::optional<Diagnostic> prepare(const NodePrepareContext& context);

    // Released between runs (new run, discontinuity, parameter change). Must not allocate.
    virtual void reset() noexcept {}
};

using NodeFactory = std::function<std::unique_ptr<AudioNodeProcessor>()>;

struct NodeFactoryDiagnostic {
    std::string code;
    std::string node_id;
    std::string message;
};

struct NodeProcessorCreationResult {
    std::unique_ptr<AudioNodeProcessor> processor;
    std::optional<NodeFactoryDiagnostic> diagnostic;
};

struct NodeProcessorRequest {
    std::string node_id;
    std::string type_id;
    NodeParameters parameters;
};

struct PlannedNodeProcessor {
    std::string node_id;
    std::string type_id;
    NodeParameters parameters;
    std::unique_ptr<AudioNodeProcessor> processor;
};

struct NodeProcessorPlan {
    std::vector<PlannedNodeProcessor> processors;
};

using NodeProcessorPlanOutcome = std::variant<NodeProcessorPlan, NodeFactoryDiagnostic>;

class NodeCatalog {
public:
    virtual ~NodeCatalog() = default;
    [[nodiscard]] virtual const NodeDescriptor* find(const std::string& type_id) const noexcept = 0;
    [[nodiscard]] virtual const std::map<std::string, NodeDescriptor, std::less<>>& descriptors() const noexcept = 0;
};

class NodeRegistry : public NodeCatalog {
public:
    [[nodiscard]] NodeRegistrationResult register_node(NodeDescriptor descriptor, NodeFactory factory);
    [[nodiscard]] bool register_descriptor(NodeDescriptor descriptor);
    [[nodiscard]] bool register_factory(std::string type_id, NodeFactory factory);
    [[nodiscard]] const NodeDescriptor* find(const std::string& type_id) const noexcept override;
    [[nodiscard]] NodeProcessorCreationResult create_processor(const std::string& type_id,
                                                                 const std::string& node_id) const;
    [[nodiscard]] NodeProcessorPlanOutcome create_plan(const std::vector<NodeProcessorRequest>& requests) const;
    [[nodiscard]] const std::map<std::string, NodeDescriptor, std::less<>>& descriptors() const noexcept override { return descriptors_; }

private:
    std::map<std::string, NodeDescriptor, std::less<>> descriptors_;
    std::map<std::string, NodeFactory, std::less<>> factories_;
};

} // namespace audioalgo
