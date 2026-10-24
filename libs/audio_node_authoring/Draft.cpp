#include "Draft.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace audioalgo::authoring {
namespace {
constexpr std::size_t maximum_document_bytes = 8 * 1024 * 1024;
bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}
std::filesystem::path logical_path(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto text = path.wstring();
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + text.substr(8);
    if (text.rfind(L"\\\\?\\", 0) == 0) return text.substr(4);
#endif
    return path;
}
std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto s = std::filesystem::absolute(logical_path(path)).lexically_normal().wstring();
    if (s.rfind(L"\\\\?\\", 0) != 0) s = s.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + s.substr(2) : L"\\\\?\\" + s;
    return s;
#else
    return path;
#endif
}
std::filesystem::path checked(const std::filesystem::path& root, const std::string& id) {
    if (!valid_id(id)) throw std::runtime_error("Invalid draft ID");
    const auto base = std::filesystem::absolute(logical_path(root)).lexically_normal();
    // Reject symlink/junction ancestors as well as the target. User-selected
    // external sources are copied separately and never used as a storage path.
    auto current = base.root_path();
    for (const auto& part : base.relative_path()) {
        current /= part;
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(io_path(current), ec)))
            throw std::runtime_error("Linked draft roots are not supported");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(io_path(current).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Reparse points are not supported in draft storage");
#endif
    }
    const auto target = base / (id + ".aadraft");
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(io_path(target), ec)))
        throw std::runtime_error("Linked draft files are not supported");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(io_path(target).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Linked draft files are not supported");
#endif
    return io_path(target);
}
void append(std::string& output, const std::string& field) {
    output += std::to_string(field.size()) + "\n" + field;
}
std::string encode(const Draft& d) {
    std::string out = "AudioAlgoDraft/1\n";
    for (const auto& field : {d.id, std::to_string(d.generation), d.language, d.declaration, d.source}) append(out, field);
    if (out.size() > maximum_document_bytes) throw std::runtime_error("Draft exceeds the 8 MiB storage budget");
    return out;
}
std::uint64_t number(const std::string& s) {
    std::uint64_t n = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || s.empty()) throw std::runtime_error("Invalid draft length/generation");
    return n;
}
Draft decode(const std::string& bytes) {
    const std::string magic = "AudioAlgoDraft/1\n";
    if (!bytes.starts_with(magic) || bytes.size() > maximum_document_bytes) throw std::runtime_error("Invalid draft format/budget");
    std::size_t offset = magic.size();
    auto field = [&] {
        const auto end = bytes.find('\n', offset);
        if (end == std::string::npos) throw std::runtime_error("Truncated draft");
        const auto size = number(bytes.substr(offset, end - offset));
        offset = end + 1;
        if (size > bytes.size() - offset) throw std::runtime_error("Truncated draft field");
        auto result = bytes.substr(offset, static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return result;
    };
    Draft d;
    d.id = field(); d.generation = number(field()); d.language = field(); d.declaration = field(); d.source = field();
    if (offset != bytes.size() || !valid_id(d.id) || d.generation == 0 || (d.language != "python" && d.language != "cpp"))
        throw std::runtime_error("Invalid draft identity/language/trailing data");
    return d;
}
std::string read(const std::filesystem::path& path, std::size_t limit = maximum_document_bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not read draft");
    std::string bytes(limit + 1, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.bad() || static_cast<std::size_t>(stream.gcount()) > limit) throw std::runtime_error("Draft read failed or exceeds budget");
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    return bytes;
}
std::filesystem::path document_path(const std::filesystem::path&);
}
FileDraftStorage::FileDraftStorage(std::filesystem::path root, std::function<bool(SavePhase)> allow)
    : root_(std::move(root)), allow_(std::move(allow)) {}
std::filesystem::path draft_parent_directory(const std::filesystem::path& path) {
    const auto logical = logical_path(path);
    if (logical.empty() || !logical.is_absolute() || logical.filename().empty())
        throw std::runtime_error("Choose an absolute draft file path");
    auto parent = logical.lexically_normal().parent_path();
#ifdef _WIN32
    // MSVC parses an extended drive root as a root-name without its separator.
    // Compute ancestry before adding the Win32 prefix. UNC share roots also
    // need a trailing separator (MSVC treats only the server as root_name).
    if (parent.native().rfind(L"\\\\", 0) == 0) {
        const auto relative = parent.relative_path();
        if (!relative.empty() && relative.parent_path().empty()) parent /= L"";
    }
#endif
    return io_path(parent);
}
std::filesystem::path FileDraftStorage::file_path(const std::string& id) const { return checked(root_, id); }
SaveResult FileDraftStorage::save(const Draft& draft) const {
    std::filesystem::path temporary;
    try {
        const auto path = checked(root_, draft.id);
        const auto bytes = encode(draft);
        std::filesystem::create_directories(draft_parent_directory(path));
        static std::atomic<std::uint64_t> counter{0};
        temporary = path; temporary += ".tmp-" + std::to_string(++counter);
        auto allow = [&](SavePhase phase) { if (allow_ && !allow_(phase)) throw std::runtime_error("Injected draft storage failure"); };
        allow(SavePhase::write);
        { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); stream.flush();
          if (!stream) throw std::runtime_error("Could not write draft temporary file"); }
        allow(SavePhase::verify);
        if (read(temporary) != bytes || decode(read(temporary)) != draft) throw std::runtime_error("Draft verification failed");
        allow(SavePhase::replace);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not atomically replace draft: " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(temporary, path);
#endif
        return {};
    } catch (const std::exception& e) {
        if (!temporary.empty()) { std::error_code ec; std::filesystem::remove(temporary, ec); }
        return Diagnostic{"draft_save_failed", e.what()};
    }
}
LoadResult FileDraftStorage::load(const std::string& id) const {
    try { auto d = decode(read(checked(root_, id))); if (d.id != id) throw std::runtime_error("Draft identity mismatch"); return d; }
    catch (const std::exception& e) { return Diagnostic{"draft_load_failed", e.what()}; }
}
ListResult FileDraftStorage::list() const {
    try {
        auto probe = checked(root_, "storage-probe");
        std::vector<std::string> ids;
        if (!std::filesystem::exists(draft_parent_directory(probe))) return ids;
        for (const auto& item : std::filesystem::directory_iterator(draft_parent_directory(probe)))
            if (item.path().extension() == ".aadraft") { auto id = item.path().stem().string(); (void)checked(root_, id); ids.push_back(std::move(id)); }
        std::sort(ids.begin(), ids.end()); return ids;
    } catch (const std::exception& e) { return Diagnostic{"draft_list_failed", e.what()}; }
}
SaveResult FileDraftStorage::save_recovery(const Draft& draft, const std::filesystem::path& original, const std::optional<std::string>& expected) const {
    if (auto error = save(draft)) return error;
    std::filesystem::path temporary;
    try {
        auto target = checked(root_, draft.id); target += ".binding"; target = document_path(target);
        std::string payload = "AudioAlgoRecoveryBinding/1\n";
        const auto name = original.u8string();
        if (name.size() > 128 * 1024) throw std::runtime_error("Recovery file location is too long");
        append(payload, draft.id); append(payload, std::to_string(draft.generation));
        append(payload, std::string(reinterpret_cast<const char*>(name.data()), name.size()));
        append(payload, expected.value_or(std::string{}));
        temporary = target; temporary += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
        { std::ofstream out(temporary, std::ios::binary); out.write(payload.data(), static_cast<std::streamsize>(payload.size())); out.close(); if (!out) throw std::runtime_error("Recovery location write failed"); }
        if (read(temporary, maximum_document_bytes + 256 * 1024) != payload) throw std::runtime_error("Recovery location verification failed");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Recovery location replacement failed");
#else
        std::filesystem::rename(temporary, target);
#endif
        return {};
    } catch (const std::exception& error) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
        return Diagnostic{"draft_recovery_binding_failed", error.what()};
    }
}
RecoveryOutcome FileDraftStorage::load_recovery(const std::string& id) const {
    auto loaded = load(id);
    if (auto* error = std::get_if<Diagnostic>(&loaded)) return *error;
    DraftRecovery result{std::get<Draft>(std::move(loaded)), {}, {}, {}};
    result.recovery_file = file_path(id);
    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(result.recovery_file, time_error);
    if (!time_error) {
        const auto system_time = std::chrono::system_clock::now()
            + (modified - std::filesystem::file_time_type::clock::now());
        result.updated_unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
    }
    try {
        auto target = checked(root_, id); target += ".binding"; target = document_path(target);
        if (!std::filesystem::exists(target)) return result;
        const auto bytes = read(target, maximum_document_bytes + 256 * 1024);
        const std::string header = "AudioAlgoRecoveryBinding/1\n";
        if (!bytes.starts_with(header)) throw std::runtime_error("Unknown recovery location format");
        std::size_t cursor = header.size();
        auto field = [&] {
            const auto end = bytes.find('\n', cursor);
            if (end == std::string::npos) throw std::runtime_error("Incomplete recovery location");
            const auto length = number(bytes.substr(cursor, end - cursor)); cursor = end + 1;
            if (length > bytes.size() - cursor) throw std::runtime_error("Invalid recovery location length");
            auto value = bytes.substr(cursor, static_cast<std::size_t>(length)); cursor += static_cast<std::size_t>(length); return value;
        };
        const auto bound_id = field(), generation = field(), path = field(), expected = field();
        if (bound_id != id || number(generation) != result.document.generation || cursor != bytes.size()) throw std::runtime_error("Recovery location belongs to another document generation");
        if (!path.empty()) {
            const auto original = std::filesystem::u8path(path);
            if (!original.is_absolute() || expected.empty() || decode(expected).id != id) throw std::runtime_error("Recovery original-file binding is invalid");
            result.original_file = original; result.expected_bytes = expected;
        }
    } catch (const std::exception& error) { result.original_file.clear(); result.expected_bytes.reset(); result.warning = error.what(); }
    return result;
}
namespace {
std::filesystem::path document_path(const std::filesystem::path& path) {
    const auto logical = logical_path(path);
    if (logical.empty() || !logical.is_absolute() || logical.filename().empty()) throw std::runtime_error("Choose an absolute draft file path");
    const auto parent = draft_parent_directory(checked(draft_parent_directory(path), "path-check"));
    const auto target = parent / logical.filename();
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(target, ec))) throw std::runtime_error("Linked draft files are not supported");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Linked draft files are not supported");
#endif
    return target;
}
}
DraftFileOutcome DraftFiles::load(const std::filesystem::path& path) const {
    try { auto bytes = read(document_path(path)); return DraftFile{decode(bytes), std::move(bytes)}; }
    catch (const std::exception& e) { return Diagnostic{"draft_open_failed", e.what()}; }
}
DraftTargetOutcome DraftFiles::inspect_target(const std::filesystem::path& selected) const {
    try {
        const auto path = document_path(selected);
        if (!std::filesystem::exists(path)) return std::monostate{};
        auto bytes = read(path); return DraftFile{decode(bytes), std::move(bytes)};
    } catch (const std::exception& error) { return Diagnostic{"draft_target_unavailable", error.what()}; }
}
SaveResult probe_authoring_directory(const std::filesystem::path& directory) {
    std::filesystem::path first, second;
    try {
        static std::atomic<std::uint64_t> serial{0};
        const auto name = "write-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
        first = checked(directory, name); second = first; second += ".replaced";
        std::filesystem::create_directories(draft_parent_directory(first));
        { std::ofstream stream(first, std::ios::binary); stream << name; stream.close(); if (!stream) throw std::runtime_error("Cannot write directory probe"); }
        if (read(first) != name) throw std::runtime_error("Directory probe could not be read back");
        std::filesystem::rename(first, second); first.clear();
        if (read(second) != name) throw std::runtime_error("Directory probe rename failed");
        std::filesystem::remove(second); return {};
    } catch (const std::exception& e) {
        std::error_code ignored;
        if (!first.empty()) std::filesystem::remove(first, ignored);
        if (!second.empty()) std::filesystem::remove(second, ignored);
        return Diagnostic{"authoring_storage_unavailable", e.what()};
    }
}
DraftFileOutcome DraftFiles::save(const std::filesystem::path& selected, const Draft& draft, const std::optional<std::string>& expected) const {
    std::filesystem::path temporary;
    try {
        const auto path = document_path(selected);
        const auto bytes = encode(draft);
        auto unchanged = [&] { return expected ? std::filesystem::exists(path) && read(path) == *expected : !std::filesystem::exists(path); };
        if (!unchanged()) return Diagnostic{"draft_file_conflict", "The selected file changed outside this editor. Choose another name to preserve both versions."};
        std::filesystem::create_directories(draft_parent_directory(path));
        static std::atomic<std::uint64_t> serial{0};
        temporary = path; temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
        auto phase = [&](SavePhase p) { if (allow_ && !allow_(p)) throw std::runtime_error("Injected draft file storage failure"); };
        phase(SavePhase::write);
        { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); stream.close();
          if (!stream) throw std::runtime_error("Could not write draft temporary file"); }
        phase(SavePhase::verify);
        if (read(temporary) != bytes || decode(read(temporary)) != draft) throw std::runtime_error("Draft verification failed");
        phase(SavePhase::replace);
        if (!unchanged()) { std::filesystem::remove(temporary); return Diagnostic{"draft_file_conflict", "The selected file changed during saving; both versions were preserved."}; }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), (expected ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace draft file, Windows error " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(temporary, path);
#endif
        return DraftFile{draft, bytes};
    } catch (const std::exception& e) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
        return Diagnostic{"draft_save_failed", e.what()};
    }
}
DraftSession::DraftSession(Draft draft, bool saved) : draft_(std::move(draft)), saved_generation_(saved ? draft_.generation : 0), recovered_generation_(saved_generation_) {}
void DraftSession::edit(std::string source, std::string declaration, std::uint64_t now) {
    if (source == draft_.source && declaration == draft_.declaration) return;
    draft_.source = std::move(source); draft_.declaration = std::move(declaration); ++draft_.generation; changed_at_ = now;
}
bool DraftSession::save_due(std::uint64_t now) const { return dirty() && now >= changed_at_ && now - changed_at_ >= 500; }
void DraftSession::saved(std::uint64_t generation) { if (generation == draft_.generation) saved_generation_ = generation; }
bool DraftSession::recovery_due(std::uint64_t now) const { return !recovery_current() && now >= changed_at_ && now - changed_at_ >= 500; }
void DraftSession::recovered(std::uint64_t generation) { if (generation == draft_.generation) recovered_generation_ = generation; }
Draft python_draft(std::string id, PythonTemplate kind) {
    Draft d; d.id = std::move(id);
    const bool stateful = kind == PythonTemplate::stateful_ramp;
    const bool named = kind == PythonTemplate::named_router;
    d.declaration = R"({
  "type_id": "user.custom_node", "version": "1.0.0", "display_name": "Custom Python node",
)";
    d.declaration += named ? R"(  "inputs": [{"id":"primary","type":"audio.pcm@1","required":true},{"id":"reference","type":"audio.pcm@1","required":true}],
  "outputs": [{"id":"main","type":"audio.pcm@1"},{"id":"side","type":"audio.pcm@1"}],
)" : R"(
  "inputs": [{"id":"audio_in","type":"audio.pcm@1","required":true}],
  "outputs": [{"id":"audio_out","type":"audio.pcm@1"}],
)";
    d.declaration += R"(
  "parameters": [{"id":"gain","type":"float","default":0.5,"minimum":-2,"maximum":2})";
    if (stateful) d.declaration += R"(,{"id":"step","type":"float","default":0.00001,"minimum":0,"maximum":0.001,"step":0.000001})";
    d.declaration += R"(],
  "format": {"sample_rates_hz":[16000,48000],"channels":[1,2,4]},
  "execution": {"kind":"block","stateful":)";
    d.declaration += stateful ? "true" : "false";
    d.declaration += R"(,"realtime_verified":false}, "authoring_template":{"id":")";
    d.declaration += named ? "named-router" : stateful ? "stateful-ramp" : "stateless-gain";
    d.declaration += R"(","version":1}})";
    d.source = "# inputs: named, read-only float32 PCM arrays [channels, samples]\n"
               "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain']}\n";
    if (stateful) d.source = R"PY(import numpy as np

def initialize(params, state):
    state['frame'] = 0

def process(inputs, params, state):
    audio = inputs['audio_in']
    first = state['frame']
    ramp = np.arange(first, first + audio.shape[1], dtype=np.float32) * np.float32(params['step'])
    state['frame'] += audio.shape[1]
    return {'audio_out': audio * np.float32(params['gain']) + ramp[None, :]}

def reset(state, reason):
    state.clear()
)PY";
    if (named) d.source = R"PY(def process(inputs, params, state):
    primary = inputs['primary']
    reference = inputs['reference']
    return {'main': (2 * primary + 3 * reference) * params['gain'],
            'side': primary - reference}
)PY";
    return d;
}
}
