#pragma once
#include "audio_run_api/NodeLogEvent.h"

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"
#include "audio_run_api/RunIdentity.h"

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
    // Opt in only if processing a zero-padded final frame is algorithmically valid.
    // The wrapper declares its buffering latency and clips that delay once at region output.
    enum class FixedFrameEndPolicy { reject_partial, zero_pad };
    FixedFrameEndPolicy fixed_frame_end_policy = FixedFrameEndPolicy::reject_partial;
    // Algorithm delay, separate from the wrapper's F-1 FIFO buffering delay.
    // nullopt means unknown: such a node cannot enter a synchronous native region.
    std::optional<std::size_t> latency_frames = 0;
    // Internal bounded-frame streaming profile (distinct from the published C ABI v1).
    // The wrapper retains each port's unconsumed input until the complete frame is done.
    bool uses_streaming_frame_api = false;
    std::size_t maximum_no_progress_calls = 8;
    // Finite post-input signal to preserve, independent of deterministic latency.
    // Unknown tails and algorithms requiring a flush API cannot enter this profile.
    enum class TailProcessing { none, zero_padding, requires_flush };
    TailProcessing tail_processing = TailProcessing::none;
    std::optional<std::size_t> tail_frames = 0;
    // Additional input timeline after EOS; includes latency recovery and frame padding.
    // Required for zero_padding tails. No calls to a fictitious algorithm flush API.
    std::optional<std::size_t> maximum_zero_padding_frames;
    // Owned task boundary; never admitted to a native audio callback region.
    bool requires_offline_task = false;
    // Internal named padded fixed-frame profile, including bounded streaming
    // EOS: complete output-port overrides. The scalar tail remains their maximum
    // for padding admission. Published C ABI v1 does not expose these overrides.
    std::map<std::string, std::size_t, std::less<>> output_tail_frames;
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
    struct PluginOrigin {
        std::string plugin_id, version, library_sha256, manifest_sha256;
        std::string runtime = "native";
        // Set only by the managed package resolver after content verification.
        std::string managed_content_sha256, managed_wrapper_version, managed_package_path;
    };
    std::optional<PluginOrigin> plugin_origin;
    std::string backend_metadata_json;
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
    std::string node_id;
    RunIdentity identity;
    std::function<bool()> is_cancelled;
    // Worker-only value transfer; no UI observer or borrowed sample pointer escapes.
    std::function<void(std::string,std::string)> record_task_artifact;
    std::function<void(NodeLogEvent)> on_log;
    std::string diagnostic_directory;
    int maximum_block_frames=512;
    std::size_t maximum_task_payload_bytes=64*1024*1024;
    // One worker-owned cache shared by all task instances in this run. Backends
    // use opaque package keys and immutable snapshot directories, never live UI state.
    std::shared_ptr<std::map<std::string,std::string>> frozen_task_packages;
    // Native adapter sets this only for its owned worker render path, never for
    // a UI/device callback. Backends must still keep process/reset free of I/O.
    bool worker_log_io_allowed=false;
    // One native render task/region shares its complete-log budget across all
    // instances. Only the owned worker drains/updates it, never process/reset.
    std::shared_ptr<std::size_t> native_task_log_bytes;
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

struct AudioStreamInputPortBlock {
    std::string_view id;
    std::span<const float* const> channels;
    std::size_t frame_count = 0;
    // Prefix belonging to the finite input timeline; remaining samples are declared zero padding.
    // EOS means this offered span reaches the end, not that nothing was consumed.
    std::size_t frames_before_eos = 0;
    bool end_of_stream = false;
};
struct AudioStreamOutputPortBlock {
    std::string_view id;
    std::span<float* const> channels;
    std::size_t frame_capacity = 0;
    std::size_t frames_before_eos = 0;
    bool end_of_stream = false;
};
struct AudioStreamBlockView {
    std::uint32_t sample_rate_hz = 0;
    std::span<const AudioStreamInputPortBlock> inputs;
    std::span<const AudioStreamOutputPortBlock> outputs;
};
// Caller-owned, zeroed before each call. Counts are independent for every port;
// a node may consume/produce only part of the offered data. Views are borrowed
// for this call only. Normal processing may not allocate or perform I/O.
struct AudioStreamProgress {
    std::span<std::size_t> consumed;
    std::span<std::size_t> produced;
    // Independent output EOS acknowledgement. Values must be 0 or 1, only after
    // producing the planned logical prefix. A completed output is never offered again.
    std::span<std::uint8_t> finished;
};
enum class AudioStreamStatus { progress, need_more_input };
using AudioStreamOutcome = std::variant<AudioStreamStatus, Diagnostic>;

class AudioNodeProcessor {
public:
    virtual ~AudioNodeProcessor() = default;
    // Owned-block entry point used by tasks that need whole ports (source/sink, file I/O, geometry).
    [[nodiscard]] virtual NodeProcessOutcome process(const NodeProcessRequest& request) = 0;
    // Called for every owned task before any execution task. Native block nodes
    // need no package snapshot; code-owning backends override this worker-only hook.
    [[nodiscard]] virtual std::optional<Diagnostic> freeze_task(const NodeExecutionContext&) { return {}; }
    // Worker-only log transfer, outside prepare/process/reset and device callbacks.
    // final=true follows reset and must detach borrowed sinks before destruction.
    // Default nodes have no log queue; the C DLL v1 ABI is unchanged.
    [[nodiscard]] virtual std::optional<Diagnostic> drain_logs(const NodeExecutionContext&, bool /*final*/) { return {}; }

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
    [[nodiscard]] virtual bool supports_stream_processing() const noexcept { return false; }
    [[nodiscard]] virtual AudioStreamOutcome process_stream(const AudioStreamBlockView&, AudioStreamProgress);
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
    void swap(NodeRegistry& other) noexcept { descriptors_.swap(other.descriptors_); factories_.swap(other.factories_); }
    [[nodiscard]] NodeRegistry subset(const std::function<bool(const NodeDescriptor&)>& include) const {
        auto selected = *this;
        for (auto it = selected.descriptors_.begin(); it != selected.descriptors_.end();) {
            if (include(it->second)) { ++it; continue; }
            selected.factories_.erase(it->first);
            it = selected.descriptors_.erase(it);
        }
        return selected;
    }
    // Compose complete descriptor/factory pairs without changing either input on conflict.
    [[nodiscard]] bool merge_disjoint(const NodeRegistry& other) {
        for (const auto& [type, descriptor] : other.descriptors_)
            if (descriptors_.contains(type)) return false;
        auto combined = *this;
        combined.descriptors_.insert(other.descriptors_.begin(), other.descriptors_.end());
        combined.factories_.insert(other.factories_.begin(), other.factories_.end());
        swap(combined);
        return true;
    }
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
