#include "audio_run_service/RunService.h"

#include "audio_io/WavFile.h"
#include "audio_runtime/NativePlan.h"
#include "audio_run_service/MixedExecution.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace audioalgo {
namespace {

juce::String to_juce_string(const std::string& value) {
    return juce::String::fromUTF8(value.c_str());
}

juce::File to_juce_file(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return juce::File(to_juce_string(utf8));
}

bool write_json_atomically(const std::filesystem::path& path, const juce::var& value) {
    const auto json = juce::JSON::toString(value, true);
    const auto utf8 = json.toUTF8();
    return to_juce_file(path).replaceWithData(utf8.getAddress(), static_cast<std::size_t>(utf8.sizeInBytes() - 1));
}

juce::var json_string(const std::string& value) {
    return juce::var(to_juce_string(value));
}

// Small streaming SHA-256 implementation used only for reproducibility metadata.
class Sha256 {
public:
    void update(const std::uint8_t* bytes, std::size_t size) {
        total_bytes_ += size;
        while (size != 0) {
            const auto count = std::min(size, buffer_.size() - buffered_);
            std::copy_n(bytes, count, buffer_.data() + buffered_);
            bytes += count;
            size -= count;
            buffered_ += count;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    std::string finish() {
        const auto bit_count = static_cast<std::uint64_t>(total_bytes_) * 8;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, 0);
        for (int index = 0; index < 8; ++index)
            buffer_[63 - index] = static_cast<std::uint8_t>((bit_count >> (index * 8)) & 0xff);
        transform(buffer_.data());

        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto word : state_) {
            for (int byte = 3; byte >= 0; --byte) {
                const auto value = static_cast<std::uint8_t>((word >> (byte * 8)) & 0xff);
                result.push_back(digits[value >> 4]);
                result.push_back(digits[value & 0x0f]);
            }
        }
        return result;
    }

private:
    void transform(const std::uint8_t* block) {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                           (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                           (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                            static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto x = words[index - 15];
            const auto y = words[index - 2];
            const auto s0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
            const auto s1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temp1 = h + sum1 + choose + constants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::size_t total_bytes_ = 0;
};

std::variant<std::string, Diagnostic> sha256_file(const std::filesystem::path& path,
                                                const std::function<bool()>& cancelled = {}) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Diagnostic{"input_hash_failed", "Could not open audio file for hashing"};
    Sha256 digest;
    std::array<std::uint8_t, 64 * 1024> block{};
    while (stream) {
        if (cancelled && cancelled()) return Diagnostic{"run_cancelled", "File hashing was cancelled"};
        stream.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        const auto count = stream.gcount();
        if (count > 0)
            digest.update(block.data(), static_cast<std::size_t>(count));
    }
    if (!stream.eof())
        return Diagnostic{"input_hash_failed", "Could not read audio file completely for hashing"};
    return digest.finish();
}

class OutputDirectoryGuard {
public:
    explicit OutputDirectoryGuard(std::filesystem::path path) : path_(std::filesystem::absolute(path).lexically_normal()) {}
    ~OutputDirectoryGuard() {
        if (!committed_) {
            std::error_code ignored;
            // This guard owns only the new directory reserved by this request.
            // Extended Windows paths are needed for deeply nested runner payloads.
#ifdef _WIN32
            const auto native=path_.wstring();
            const auto target=native.rfind(L"\\\\?\\",0)==0?native:native.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+native.substr(2):L"\\\\?\\"+native;
            std::filesystem::remove_all(std::filesystem::path(target), ignored);
#else
            std::filesystem::remove_all(path_, ignored);
#endif
        }
    }
    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

} // namespace

RunService::RunService(const NodeRegistry& nodes, std::shared_ptr<const ProjectResourceService> resources)
    : nodes_(&nodes), resources_(resources ? std::move(resources) : std::make_shared<ProjectResourceService>()) {}

RunService::RunService(std::shared_ptr<const NodeRegistry> nodes,
                       std::shared_ptr<const ProjectResourceService> resources)
    : nodes_owner_(std::move(nodes)), nodes_(nodes_owner_.get()),
      resources_(resources ? std::move(resources) : std::make_shared<ProjectResourceService>()) {}

RunOutcome RunService::run(const RunRequest& request) const {
    const auto cancelled = [&] { return request.is_cancelled && request.is_cancelled(); };
    const auto cancellation = [] { return Diagnostic{"run_cancelled", "Offline run cancelled; no successful result was published"}; };
    const auto file_hash = [&](const std::filesystem::path& path) { return sha256_file(path, request.is_cancelled); };
    if (cancelled()) return cancellation();
    if (nodes_ == nullptr)
        return Diagnostic{"node_registry_missing", "The run service has no node registry"};
    auto identity = request.identity;
    if (identity.request_id.empty()) {
        identity.project_id = request.project.project_id;
        identity.session_epoch = 0;
        identity.request_id = juce::Uuid().toString().toStdString();
        identity.revision = request.project.revision;
    }
    if (identity.project_id != request.project.project_id || identity.revision != request.project.revision)
        return Diagnostic{"run_identity_mismatch", "Run identity does not belong to the requested project snapshot"};
    if (!std::filesystem::is_regular_file(request.project_file))
        return Diagnostic{"project_file_missing", "Project file does not exist"};
    if (request.output_directory.empty() || std::filesystem::exists(request.output_directory))
        return Diagnostic{"run_directory_exists", "Run output directory must be a new path"};

    const auto graph_diagnostics = validate_project(request.project, *nodes_);
    if (!graph_diagnostics.empty())
        return Diagnostic{graph_diagnostics.front().code, graph_diagnostics.front().message,
                          graph_diagnostics.front().node_id, graph_diagnostics.front().port_id};

    // Only sources inside the selected output's reverse dependency closure are materialized;
    // unselected branches create no processors and no resources.
    const auto selection_outcome = evaluate_plan_selection(request.project, *nodes_);
    if (const auto* selection_error = std::get_if<Diagnostic>(&selection_outcome))
        return *selection_error;
    const auto& selection = std::get<PlanSelection>(selection_outcome);
    std::vector<const NodeInstance*> source_nodes;
    for (const auto& node : request.project.nodes) {
        const auto* descriptor = nodes_->find(node.type_id);
        if (descriptor != nullptr && descriptor->execution_kind == NodeExecutionKind::source &&
            selection.closure.contains(node.id))
            source_nodes.push_back(&node);
    }
    if (source_nodes.empty())
        return Diagnostic{"source_node_missing", "Project has no registered audio source node"};
    struct SourceInput {
        const NodeInstance* node;
        std::filesystem::path path;
    };
    std::vector<SourceInput> source_inputs;
    for (const auto* node : source_nodes) {
        const auto path_parameter = node->parameters.find("path");
        if (path_parameter == node->parameters.end() || !std::holds_alternative<std::string>(path_parameter->second))
            return Diagnostic{"source_path_missing", "File source node requires a path parameter", node->id, "path"};
        const auto utf8_path = std::get<std::string>(path_parameter->second);
        if (utf8_path.empty())
            return Diagnostic{"source_path_missing", "File source path must not be empty", node->id, "path"};
        const auto resource_path = resources_->resolve(utf8_path, request.project_file);
        if (const auto* error = std::get_if<Diagnostic>(&resource_path)) {
            auto contextual = *error;
            if (contextual.code == "source_resource_missing") {
                contextual.node_id = node->id;
                contextual.port_id = "path";
            }
            return contextual;
        }
        source_inputs.push_back({node, std::get<std::filesystem::path>(resource_path)});
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(request.output_directory, filesystem_error))
        return Diagnostic{"run_directory_create_failed", filesystem_error ? filesystem_error.message() : "Could not reserve a new run directory"};
    OutputDirectoryGuard output_guard(request.output_directory);

    struct ResourceRecord {
        std::string node_id, parameter_id;
        std::filesystem::path path, snapshot_path;
        std::string sha256;
    };
    std::vector<ResourceRecord> resource_records;
    const auto capture_resource = [&](const std::filesystem::path& path, const std::string& node_id,
                                      const std::string& parameter_id) -> std::variant<ResourceRecord, Diagnostic> {
        const auto error = [&](std::string code, std::string message) {
            return Diagnostic{std::move(code), std::move(message), node_id, parameter_id};
        };
        const auto before = file_hash(path);
        if (const auto* failure = std::get_if<Diagnostic>(&before))
            return error(failure->code, failure->message);
        const auto relative = std::filesystem::path("resources") /
                              std::to_string(resource_records.size()) / path.filename();
        const auto destination_path = request.output_directory / relative;
        std::error_code directory_error;
        if (!std::filesystem::create_directories(destination_path.parent_path(), directory_error))
            return error("resource_snapshot_create_failed", directory_error ? directory_error.message() : "Could not reserve resource snapshot storage");
        // Fresh files deliberately do not inherit read-only attributes from user inputs.
        // Every stream is closed before a processor can observe the captured version.
        {
            std::ifstream source(path, std::ios::binary);
            std::ofstream destination(destination_path, std::ios::binary);
            if (!source || !destination)
                return error("resource_snapshot_copy_failed", "Could not open resource snapshot streams");
            std::array<char, 64 * 1024> bytes{};
            while (source) {
                if (cancelled()) return error("run_cancelled", "Resource snapshot copy was cancelled");
                source.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (source.gcount() > 0)
                    destination.write(bytes.data(), source.gcount());
                if (!destination)
                    return error("resource_snapshot_copy_failed", "Could not write complete resource snapshot");
            }
            destination.close();
            if (!source.eof() || source.bad() || !destination)
                return error("resource_snapshot_copy_failed", "Could not close complete resource snapshot");
        }
        const auto copied = file_hash(destination_path);
        if (const auto* diagnostic = std::get_if<Diagnostic>(&copied)) return error(diagnostic->code, diagnostic->message);
        if (std::get<std::string>(copied) != std::get<std::string>(before))
            return error("resource_modified_during_snapshot", "Resource capture did not match the fixed input version");
        return ResourceRecord{node_id, parameter_id, path, relative, std::get<std::string>(copied)};
    };
    // Every source adapter receives a materialized, independently captured file, exactly like
    // any explicitly declared native input resource; no later read uses the caller's mutable
    // disk paths, and no buffer is ever shared between two sources.
    for (const auto& input : source_inputs) {
        auto captured = capture_resource(input.path, input.node->id, "path");
        if (const auto* error = std::get_if<Diagnostic>(&captured))
            return *error;
        resource_records.push_back(std::move(std::get<ResourceRecord>(captured)));
    }
    const auto& input_path = source_inputs.front().path;
    const auto input_hash = resource_records.front().sha256;
    std::vector<AudioSourceBinding> source_bindings;
    for (std::size_t index = 0; index < source_inputs.size(); ++index) {
        const auto captured_input = request.output_directory / resource_records[index].snapshot_path;
        const auto audio_result = read_wav_file(captured_input);
        if (const auto* error = std::get_if<Diagnostic>(&audio_result))
            return *error;
        source_bindings.push_back({source_inputs[index].node->id, std::move(std::get<AudioBuffer>(audio_result))});
    }
    auto executed_project = request.project;
    for (auto& node : executed_project.nodes)
        if (const auto* descriptor = nodes_->find(node.type_id))
            for (const auto& parameter : descriptor->parameters)
                node.parameters.try_emplace(parameter.id, parameter.default_value);
    auto rendering_project = executed_project;
    std::map<std::string, std::filesystem::path, std::less<>> captured_source_copies;
    for (std::size_t index = 0; index < source_inputs.size(); ++index)
        captured_source_copies.emplace(source_inputs[index].node->id,
                                       request.output_directory / resource_records[index].snapshot_path);
    for (auto& node : rendering_project.nodes) {
        // Unselected branches are not rendered: their resources are never resolved or captured.
        if (!selection.closure.contains(node.id))
            continue;
        const auto* descriptor = nodes_->find(node.type_id);
        const auto source_path = captured_source_copies.find(node.id);
        if (source_path != captured_source_copies.end())
            node.parameters["path"] = to_juce_file(source_path->second).getFullPathName().toStdString();
        for (const auto& parameter : descriptor->parameters) {
            if (!parameter.input_resource ||
                (source_path != captured_source_copies.end() && parameter.id == "path"))
                continue;
            const auto& reference = std::get<std::string>(node.parameters.at(parameter.id));
            if (reference.empty())
                return Diagnostic{"resource_path_missing", "Input resource path must not be empty", node.id, parameter.id};
            const auto resolved = resources_->resolve(reference, request.project_file);
            if (const auto* error = std::get_if<Diagnostic>(&resolved))
                return Diagnostic{"parameter_resource_missing", error->message, node.id, parameter.id};
            auto captured = capture_resource(std::get<std::filesystem::path>(resolved), node.id, parameter.id);
            if (const auto* error = std::get_if<Diagnostic>(&captured))
                return *error;
            resource_records.push_back(std::move(std::get<ResourceRecord>(captured)));
            node.parameters[parameter.id] = to_juce_file(request.output_directory /
                resource_records.back().snapshot_path).getFullPathName().toStdString();
        }
    }
    // Reject changes while capture is in progress. Once this boundary is crossed,
    // original paths may change or disappear; execution and publication use copies.
    for (const auto& resource : resource_records) {
        const auto digest = file_hash(resource.path);
        if (const auto* error = std::get_if<Diagnostic>(&digest)) return *error;
        if (!std::holds_alternative<std::string>(digest) || std::get<std::string>(digest) != resource.sha256)
            return Diagnostic{"resource_modified_during_snapshot", "Input resource changed before capture completed",
                              resource.node_id, resource.parameter_id};
    }
    NativeRenderStatistics render_statistics;
    std::string task_manifest_json;juce::Array<juce::var> task_artifacts;
    const bool mixed=std::any_of(rendering_project.nodes.begin(),rendering_project.nodes.end(),[&](const auto& n){return selection.closure.contains(n.id) && nodes_->find(n.type_id)->capabilities.requires_offline_task;});
    NodeExecutionContext context;context.project_file=to_juce_file(request.project_file).getFullPathName().toStdString();
    context.output_directory=to_juce_file(request.output_directory).getFullPathName().toStdString();context.identity=identity;context.is_cancelled=request.is_cancelled;
    context.maximum_task_payload_bytes=request.maximum_task_payload_bytes;
    context.record_task_artifact=[&](std::string directory,std::string provenance){juce::DynamicObject::Ptr item=new juce::DynamicObject;
        item->setProperty("directory",to_juce_file(std::filesystem::u8path(directory)).getRelativePathFrom(to_juce_file(request.output_directory)).replaceCharacter('\\','/'));item->setProperty("path_base","run_root");
        item->setProperty("provenance",juce::JSON::parse(to_juce_string(provenance)));task_artifacts.add(juce::var(item.get()));};
    const auto rendered_result = mixed?render_prepared_audio_graph(rendering_project,*nodes_,std::move(source_bindings),request.block_size,std::move(context),&render_statistics,&task_manifest_json):
        render_audio_graph(rendering_project, *nodes_, std::move(source_bindings), request.block_size,{{}, &render_statistics, true, request.is_cancelled});
    if (const auto* error = std::get_if<Diagnostic>(&rendered_result))
        return *error;
    if (cancelled()) return cancellation();
    for (const auto& resource : resource_records) {
        const auto digest = file_hash(request.output_directory / resource.snapshot_path);
        if (const auto* error = std::get_if<Diagnostic>(&digest)) return *error;
        if (!std::holds_alternative<std::string>(digest) || std::get<std::string>(digest) != resource.sha256)
            return Diagnostic{"resource_snapshot_modified", "Captured resource changed while the run was executing",
                              resource.node_id, resource.parameter_id};
    }

    const auto run_id = identity.request_id;
    const auto output_file = request.output_directory / "output.wav";
    const auto write_result = write_wav_file(output_file, std::get<AudioBuffer>(rendered_result), WavEncoding::float32);
    if (const auto* error = std::get_if<Diagnostic>(&write_result))
        return *error;
    const auto output_hash_result = file_hash(output_file);
    if (const auto* error = std::get_if<Diagnostic>(&output_hash_result))
        return *error;
    std::string raw_output_name = "output.wav";
    std::string raw_output_hash = std::get<std::string>(output_hash_result);
    if (render_statistics.raw_audio) {
        raw_output_name = "output.raw.wav";
        const auto raw_file = request.output_directory / raw_output_name;
        const auto written = write_wav_file(raw_file, *render_statistics.raw_audio, WavEncoding::float32);
        if (const auto* error = std::get_if<Diagnostic>(&written)) return *error;
        const auto digest = file_hash(raw_file);
        if (const auto* error = std::get_if<Diagnostic>(&digest)) return *error;
        raw_output_hash = std::get<std::string>(digest);
    }

    // Keep the executed parameter values with each run, including descriptor defaults.
    // The caller's editable project and original project file remain untouched.
    const auto snapshot_file = request.output_directory / "project.aaproj";
    const auto snapshot_result = save_project_file(snapshot_file, executed_project);
    if (const auto* error = std::get_if<Diagnostic>(&snapshot_result))
        return *error;
    const auto snapshot_hash = file_hash(snapshot_file);
    if (const auto* error = std::get_if<Diagnostic>(&snapshot_hash))
        return *error;

    auto replay_project = executed_project;
    const auto relative_utf8 = [](const std::filesystem::path& path) {
        const auto bytes = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    for (auto& node : replay_project.nodes)
        for (const auto& resource : resource_records)
            if (node.id == resource.node_id)
                node.parameters[resource.parameter_id] = relative_utf8(resource.snapshot_path);
    const auto replay_file = request.output_directory / "replay.aaproj";
    const auto replay_saved = save_project_file(replay_file, replay_project);
    if (const auto* error = std::get_if<Diagnostic>(&replay_saved))
        return *error;
    const auto replay_hash = file_hash(replay_file);
    if (const auto* error = std::get_if<Diagnostic>(&replay_hash))
        return *error;

    juce::DynamicObject::Ptr manifest = new juce::DynamicObject();
    manifest->setProperty("schema_version", 1);
    manifest->setProperty("run_id", json_string(run_id));
    manifest->setProperty("project_id", json_string(request.project.project_id));
    manifest->setProperty("project_revision", static_cast<juce::int64>(request.project.revision));
    manifest->setProperty("project_snapshot", "project.aaproj");
    manifest->setProperty("project_snapshot_sha256", json_string(std::get<std::string>(snapshot_hash)));
    manifest->setProperty("replay_project", "replay.aaproj");
    manifest->setProperty("replay_project_sha256", json_string(std::get<std::string>(replay_hash)));
    juce::Array<juce::var> resource_manifest;
    for (const auto& resource : resource_records) {
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty("node_id", json_string(resource.node_id));
        item->setProperty("parameter_id", json_string(resource.parameter_id));
        item->setProperty("path", to_juce_file(resource.path).getFullPathName());
        item->setProperty("sha256", json_string(resource.sha256));
        item->setProperty("snapshot_path", json_string(relative_utf8(resource.snapshot_path)));
        item->setProperty("snapshot_sha256", json_string(resource.sha256));
        resource_manifest.add(juce::var(item.get()));
    }
    manifest->setProperty("resources", resource_manifest);
    if(mixed){manifest->setProperty("tasks",juce::JSON::parse(to_juce_string(task_manifest_json)));manifest->setProperty("task_artifacts",task_artifacts);
        juce::Array<juce::var> packages;std::set<std::string> seen;
        for(const auto& artifact:task_artifacts){const auto path=artifact["provenance"]["frozen_package_root"].toString();if(seen.insert(path.toStdString()).second)packages.add(path);}
        manifest->setProperty("frozen_python_packages",packages);
    }
    juce::Array<juce::var> plugin_manifest;
    juce::Array<juce::var> node_versions;
    for(const auto& node:executed_project.nodes) {
        if(!selection.closure.contains(node.id)) continue;
        const auto* descriptor=nodes_->find(node.type_id);
        if(!descriptor) continue;
        juce::DynamicObject::Ptr version=new juce::DynamicObject;
        version->setProperty("node_id",json_string(node.id));
        version->setProperty("type_id",json_string(node.type_id));
        version->setProperty("version",json_string(descriptor->version));
        node_versions.add(juce::var(version.get()));
        if(!descriptor->plugin_origin || descriptor->plugin_origin->runtime!="native") continue;
        const auto& origin=*descriptor->plugin_origin;
        juce::DynamicObject::Ptr item=new juce::DynamicObject;
        item->setProperty("node_id",json_string(node.id));
        item->setProperty("type_id",json_string(node.type_id));
        item->setProperty("node_version",json_string(descriptor->version));
        item->setProperty("plugin_id",json_string(origin.plugin_id));
        item->setProperty("plugin_version",json_string(origin.version));
        item->setProperty("library_sha256",json_string(origin.library_sha256));
        item->setProperty("manifest_sha256",json_string(origin.manifest_sha256));
        plugin_manifest.add(juce::var(item.get()));
    }
    manifest->setProperty("plugins",plugin_manifest);
    manifest->setProperty("node_versions",node_versions);
    manifest->setProperty("session_epoch", static_cast<juce::int64>(identity.session_epoch));
    manifest->setProperty("request_id", json_string(identity.request_id));
    manifest->setProperty("sample_rate_hz", static_cast<int>(std::get<AudioBuffer>(rendered_result).sample_rate_hz()));
    manifest->setProperty("channels", static_cast<int>(std::get<AudioBuffer>(rendered_result).channel_count()));
    manifest->setProperty("frames", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).frame_count()));
    manifest->setProperty("valid_start_frame", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).valid_interval().first_frame));
    manifest->setProperty("valid_frame_count", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).valid_interval().frame_count));
    manifest->setProperty("block_size", request.block_size);
    manifest->setProperty("maximum_task_payload_bytes",static_cast<juce::int64>(request.maximum_task_payload_bytes));
    manifest->setProperty("raw_frames", static_cast<juce::int64>(render_statistics.raw_frames));
    manifest->setProperty("raw_duration_ns", static_cast<juce::int64>(render_statistics.raw_duration_ns));
    manifest->setProperty("aligned_duration_ns", static_cast<juce::int64>(render_statistics.aligned_duration_ns));
    manifest->setProperty("alignment_offset_ns", static_cast<juce::int64>(render_statistics.alignment_offset_ns));
    manifest->setProperty("time_rounding", "floor_absolute_sample_position");
    manifest->setProperty("aligned_frames", static_cast<juce::int64>(render_statistics.aligned_frames));
    manifest->setProperty("source_frames", static_cast<juce::int64>(render_statistics.source_frames));
    manifest->setProperty("retained_tail_frames", static_cast<juce::int64>(render_statistics.retained_tail_frames));
    manifest->setProperty("zero_padding_frames", static_cast<juce::int64>(render_statistics.zero_padding_frames));
    manifest->setProperty("drain_padding_frames", static_cast<juce::int64>(render_statistics.drain_padding_frames));
    manifest->setProperty("raw_valid_frame_count", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).valid_interval().frame_count));
    manifest->setProperty("alignment_offset_frames", static_cast<juce::int64>(render_statistics.alignment_offset_frames));
    manifest->setProperty("raw_output_file", json_string(raw_output_name));
    manifest->setProperty("raw_output_sha256", json_string(raw_output_hash));
    manifest->setProperty("raw_valid_start_frame", static_cast<juce::int64>(render_statistics.alignment_offset_frames +
        std::get<AudioBuffer>(rendered_result).valid_interval().first_frame));
    manifest->setProperty("input_sha256", json_string(input_hash));
    manifest->setProperty("output_sha256", json_string(std::get<std::string>(output_hash_result)));
    const auto input_filename = input_path.filename().u8string();
    const std::string input_filename_utf8(reinterpret_cast<const char*>(input_filename.data()), input_filename.size());
    manifest->setProperty("input_file", json_string(input_filename_utf8));
    manifest->setProperty("output_file", "output.wav");
    if (cancelled()) return cancellation();
    if (!write_json_atomically(request.output_directory / "run.json", juce::var(manifest.get())))
        return Diagnostic{"run_manifest_write_failed", "Could not publish the run manifest"};

    juce::DynamicObject::Ptr completion = new juce::DynamicObject();
    completion->setProperty("schema_version", 1);
    completion->setProperty("run_id", json_string(run_id));
    completion->setProperty("manifest", "run.json");
    completion->setProperty("output_sha256", json_string(std::get<std::string>(output_hash_result)));
    // Last cooperative cancellation/commit boundary. Once this final check has
    // passed, publishing the completion marker is the terminal successful action.
    if (cancelled()) return cancellation();
    if (!write_json_atomically(request.output_directory / "complete.json", juce::var(completion.get())))
        return Diagnostic{"run_complete_write_failed", "Could not publish the completion marker"};
    output_guard.commit();

    return RunSuccess{run_id, output_file, request.output_directory / "run.json", request.output_directory / "complete.json",
                      std::get<AudioBuffer>(rendered_result).frame_count(), input_hash,
                      std::get<std::string>(output_hash_result), identity};
}

} // namespace audioalgo
