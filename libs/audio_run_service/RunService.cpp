#include "audio_run_service/RunService.h"

#include "audio_io/WavFile.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

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

std::variant<std::string, Diagnostic> sha256_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Diagnostic{"input_hash_failed", "Could not open audio file for hashing"};
    Sha256 digest;
    std::array<std::uint8_t, 64 * 1024> block{};
    while (stream) {
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
    explicit OutputDirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~OutputDirectoryGuard() {
        if (!committed_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

} // namespace

RunService::RunService(const NodeRegistry& descriptors, const JuceProcessorRegistry& processors)
    : descriptors_(descriptors), processors_(processors) {}

RunOutcome RunService::run(const RunRequest& request) const {
    if (!std::filesystem::is_regular_file(request.project_file))
        return Diagnostic{"project_file_missing", "Project file does not exist"};
    if (request.output_directory.empty() || std::filesystem::exists(request.output_directory))
        return Diagnostic{"run_directory_exists", "Run output directory must be a new path"};

    const auto graph_diagnostics = validate_project(request.project, descriptors_);
    if (!graph_diagnostics.empty())
        return Diagnostic{graph_diagnostics.front().code, graph_diagnostics.front().message};

    const NodeInstance* source_node = nullptr;
    for (const auto& node : request.project.nodes) {
        const auto* descriptor = descriptors_.find(node.type_id);
        if (descriptor != nullptr && descriptor->execution_kind == NodeExecutionKind::source) {
            if (source_node != nullptr)
                return Diagnostic{"graph_shape_unsupported", "A G1 run requires exactly one audio source"};
            source_node = &node;
        }
    }
    if (source_node == nullptr)
        return Diagnostic{"source_node_missing", "Project has no registered audio source node"};
    const auto path_parameter = source_node->parameters.find("path");
    if (path_parameter == source_node->parameters.end() || !std::holds_alternative<std::string>(path_parameter->second))
        return Diagnostic{"source_path_missing", "File source node requires a path parameter"};
    const auto utf8_path = std::get<std::string>(path_parameter->second);
    if (utf8_path.empty())
        return Diagnostic{"source_path_missing", "File source path must not be empty"};

    auto input_path = std::filesystem::u8path(utf8_path);
    if (input_path.is_relative())
        input_path = request.project_file.parent_path() / input_path;
    input_path = input_path.lexically_normal();
    const auto input_hash_before = sha256_file(input_path);
    if (const auto* error = std::get_if<Diagnostic>(&input_hash_before))
        return *error;
    const auto audio_result = read_wav_file(input_path);
    if (const auto* error = std::get_if<Diagnostic>(&audio_result))
        return *error;
    const auto rendered_result = render_audio_graph(request.project, descriptors_, processors_, std::get<AudioBuffer>(audio_result), request.block_size);
    if (const auto* error = std::get_if<Diagnostic>(&rendered_result))
        return *error;
    const auto input_hash_after = sha256_file(input_path);
    if (const auto* error = std::get_if<Diagnostic>(&input_hash_after))
        return *error;
    const auto input_hash = std::get<std::string>(input_hash_before);
    if (input_hash != std::get<std::string>(input_hash_after))
        return Diagnostic{"source_modified_during_run", "Input audio changed while the run was executing"};

    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(request.output_directory, filesystem_error))
        return Diagnostic{"run_directory_create_failed", filesystem_error ? filesystem_error.message() : "Could not reserve a new run directory"};
    OutputDirectoryGuard output_guard(request.output_directory);

    const auto run_id = juce::Uuid().toString().toStdString();
    const auto output_file = request.output_directory / "output.wav";
    const auto write_result = write_wav_file(output_file, std::get<AudioBuffer>(rendered_result), WavEncoding::float32);
    if (const auto* error = std::get_if<Diagnostic>(&write_result))
        return *error;
    const auto output_hash_result = sha256_file(output_file);
    if (const auto* error = std::get_if<Diagnostic>(&output_hash_result))
        return *error;

    juce::DynamicObject::Ptr manifest = new juce::DynamicObject();
    manifest->setProperty("schema_version", 1);
    manifest->setProperty("run_id", json_string(run_id));
    manifest->setProperty("project_id", json_string(request.project.project_id));
    manifest->setProperty("project_revision", static_cast<juce::int64>(request.project.revision));
    manifest->setProperty("sample_rate_hz", static_cast<int>(std::get<AudioBuffer>(rendered_result).sample_rate_hz()));
    manifest->setProperty("channels", static_cast<int>(std::get<AudioBuffer>(rendered_result).channel_count()));
    manifest->setProperty("frames", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).frame_count()));
    manifest->setProperty("valid_start_frame", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).valid_interval().first_frame));
    manifest->setProperty("valid_frame_count", static_cast<juce::int64>(std::get<AudioBuffer>(rendered_result).valid_interval().frame_count));
    manifest->setProperty("block_size", request.block_size);
    manifest->setProperty("input_sha256", json_string(input_hash));
    manifest->setProperty("output_sha256", json_string(std::get<std::string>(output_hash_result)));
    const auto input_filename = input_path.filename().u8string();
    const std::string input_filename_utf8(reinterpret_cast<const char*>(input_filename.data()), input_filename.size());
    manifest->setProperty("input_file", json_string(input_filename_utf8));
    manifest->setProperty("output_file", "output.wav");
    if (!write_json_atomically(request.output_directory / "run.json", juce::var(manifest.get())))
        return Diagnostic{"run_manifest_write_failed", "Could not publish the run manifest"};

    juce::DynamicObject::Ptr completion = new juce::DynamicObject();
    completion->setProperty("schema_version", 1);
    completion->setProperty("run_id", json_string(run_id));
    completion->setProperty("manifest", "run.json");
    completion->setProperty("output_sha256", json_string(std::get<std::string>(output_hash_result)));
    if (!write_json_atomically(request.output_directory / "complete.json", juce::var(completion.get())))
        return Diagnostic{"run_complete_write_failed", "Could not publish the completion marker"};
    output_guard.commit();

    return RunSuccess{run_id, output_file, request.output_directory / "run.json", request.output_directory / "complete.json",
                      std::get<AudioBuffer>(rendered_result).frame_count(), input_hash,
                      std::get<std::string>(output_hash_result)};
}

} // namespace audioalgo
