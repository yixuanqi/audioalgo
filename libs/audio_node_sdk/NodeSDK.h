#pragma once

#include "audio_core/AudioBuffer.h"
#include "audio_core/Diagnostic.h"

#include <functional>
#include <map>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace audioalgo {

enum class PortDirection { input, output };
enum class ParameterKind { boolean, integer, floating, string, path };
enum class NodeExecutionKind { source, block, sink };

using ParameterValue = std::variant<bool, std::int64_t, double, std::string>;
using NodeParameters = std::map<std::string, ParameterValue, std::less<>>;

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
};

struct NodeDescriptor {
    std::string type_id;
    std::string version;
    std::string display_name;
    std::vector<PortDescriptor> ports;
    std::vector<ParameterDescriptor> parameters;
    NodeExecutionKind execution_kind = NodeExecutionKind::block;
};

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

class AudioNodeProcessor {
public:
    virtual ~AudioNodeProcessor() = default;
    [[nodiscard]] virtual NodeProcessOutcome process(const NodeProcessRequest& request) = 0;
};

using NodeFactory = std::function<std::unique_ptr<AudioNodeProcessor>()>;

class NodeRegistry {
public:
    [[nodiscard]] bool register_descriptor(NodeDescriptor descriptor);
    [[nodiscard]] bool register_factory(std::string type_id, NodeFactory factory);
    [[nodiscard]] const NodeDescriptor* find(const std::string& type_id) const noexcept;
    [[nodiscard]] std::unique_ptr<AudioNodeProcessor> create_processor(const std::string& type_id) const;
    [[nodiscard]] const std::map<std::string, NodeDescriptor, std::less<>>& descriptors() const noexcept { return descriptors_; }

private:
    std::map<std::string, NodeDescriptor, std::less<>> descriptors_;
    std::map<std::string, NodeFactory, std::less<>> factories_;
};

void register_builtin_node_descriptors(NodeRegistry& registry);

} // namespace audioalgo
