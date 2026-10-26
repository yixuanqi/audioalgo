#include "Workspace.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace audioalgo::authoring {
namespace {
// ---------------------------------------------------------------------------
// Frozen contract constants (ADR0040 §5/§7)
// ---------------------------------------------------------------------------
constexpr const char* kManifestMagic = "AudioAlgoNodeWorkspace/1\n";
constexpr std::uint64_t kMaximumNodeJsonBytes = 1024 * 1024;
constexpr std::uint64_t kMaximumSourceBytes = 8 * 1024 * 1024 - 16 * 1024;
constexpr std::size_t kMaximumSourceFiles = 128;
constexpr std::uint64_t kMaximumResourceBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumResourceFiles = 64;
constexpr std::size_t kMaximumRelativePathBytes = 512;
constexpr std::uint64_t kMaximumDraftFileBytes = 8 * 1024 * 1024;

struct WorkspaceFailure {
    std::string code, message;
};
[[noreturn]] void fail(std::string code, std::string message) {
    throw WorkspaceFailure{std::move(code), std::move(message)};
}
Diagnostic diagnostic_of(const WorkspaceFailure& failure) { return Diagnostic{failure.code, failure.message}; }

// ---------------------------------------------------------------------------
// SHA-256 (self-contained; no JUCE or third party dependency)
// ---------------------------------------------------------------------------
class Sha256 {
public:
    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        length_ += size;
        while (size > 0) {
            const auto take = std::min<std::size_t>(size, 64 - fill_);
            std::copy(bytes, bytes + take, block_ + fill_);
            fill_ += take; bytes += take; size -= take;
            if (fill_ == 64) { compress(block_); fill_ = 0; }
        }
    }
    std::string finish() {
        const std::uint64_t bits = length_ * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (fill_ != 56) update(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<unsigned char>((bits >> (56 - 8 * i)) & 0xff);
        update(tail, sizeof tail);
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (auto word : state_)
            for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 0xf]);
        return out;
    }
private:
    static std::uint32_t rotr(std::uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }
    void compress(const unsigned char* block) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
                   (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
    std::uint32_t state_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t length_ = 0;
    unsigned char block_[64] = {};
    std::size_t fill_ = 0;
};

std::string sha256_of(std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish();
}

// ---------------------------------------------------------------------------
// Paths and files
// ---------------------------------------------------------------------------
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
    auto text = std::filesystem::absolute(logical_path(path)).lexically_normal().wstring();
    if (text.rfind(L"\\\\?\\", 0) != 0)
        text = text.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + text.substr(2) : L"\\\\?\\" + text;
    return text;
#else
    return path;
#endif
}
bool is_linked(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(io_path(path), error);
    if (!error && std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(io_path(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
std::string path_to_utf8_generic(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string read_bytes(const std::filesystem::path& path, std::uint64_t limit, const char* missing_code) {
    std::ifstream stream(io_path(path), std::ios::binary);
    if (!stream) fail(missing_code, "Could not read " + path_to_utf8_generic(path));
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(limit) + 1);
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto read = static_cast<std::uint64_t>(stream.gcount());
    if (stream.bad()) fail("workspace_io_failed", "Read failed for " + path_to_utf8_generic(path));
    if (read > limit) fail("workspace_budget_exceeded", "File exceeds the frozen budget: " + path_to_utf8_generic(path));
    bytes.resize(static_cast<std::size_t>(read));
    return bytes;
}
void write_atomic(const std::filesystem::path& path, std::string_view bytes,
                  const std::function<bool(WorkspacePhase)>& allow) {
    auto hook = [&allow](WorkspacePhase phase, const std::string& name) {
        if (allow && !allow(phase))
            fail("workspace_write_failed", "Injected workspace failure at phase '" + name + "'");
    };
    const auto io = io_path(path);
    const auto parent = io.parent_path();
    std::error_code error;
    if (!std::filesystem::is_directory(parent)) std::filesystem::create_directories(parent, error);
    if (error) fail("workspace_io_failed", "Could not create directory " + path_to_utf8_generic(parent) + ": " + error.message());
    static std::atomic<std::uint64_t> serial{0};
    auto temporary = io;
    temporary += L".tmp-" + std::to_wstring(++serial);
    try {
        hook(WorkspacePhase::write, "write");
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream) fail("workspace_io_failed", "Could not write " + path_to_utf8_generic(path));
        }
        hook(WorkspacePhase::verify, "verify");
        if (read_bytes(temporary, bytes.size() + 1, "workspace_io_failed") != bytes)
            fail("workspace_io_failed", "Verification failed for " + path_to_utf8_generic(path));
        hook(WorkspacePhase::replace, "replace");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), io.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            fail("workspace_io_failed", "Could not replace " + path_to_utf8_generic(path) + ", Windows error " +
                                           std::to_string(GetLastError()));
#else
        std::error_code error_code;
        std::filesystem::rename(temporary, io, error_code);
        if (error_code) fail("workspace_io_failed", "Could not replace " + path_to_utf8_generic(path));
#endif
    } catch (...) {
        // A failed phase never leaves its own temporary file behind.
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

// ---------------------------------------------------------------------------
// Token preserving JSON reader and canonical writer (ADR0040 §3)
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Kind { null_value, boolean, number, string, array, object };
    Kind kind = Kind::null_value;
    bool boolean = false;
    std::string text;  // number token, or UTF-8 string content
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;
    const JsonValue* find(std::string_view key) const {
        for (const auto& member : members)
            if (member.first == key) return &member.second;
        return nullptr;
    }
    JsonValue* find(std::string_view key) {
        for (auto& member : members)
            if (member.first == key) return &member.second;
        return nullptr;
    }
};
void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}
class JsonParser {
public:
    explicit JsonParser(std::string_view document) : document_(document) {}
    JsonValue parse() {
        if (document_.substr(0, 3) == "\xef\xbb\xbf") fail("workspace_json_invalid", "UTF-8 BOM is not accepted");
        skip_space();
        auto value = parse_value();
        skip_space();
        if (position_ != document_.size()) fail("workspace_json_invalid", "Trailing bytes after the JSON document");
        return value;
    }
private:
    void skip_space() {
        while (position_ < document_.size()) {
            const auto c = document_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }
    char peek() const { return position_ < document_.size() ? document_[position_] : '\0'; }
    char take() {
        if (position_ >= document_.size()) fail("workspace_json_invalid", "Unexpected end of JSON");
        return document_[position_++];
    }
    void expect(char wanted) {
        if (take() != wanted) fail("workspace_json_invalid", std::string("Expected '") + wanted + "'");
    }
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    JsonValue parse_value() {
        skip_space();
        switch (peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': { JsonValue value; value.kind = JsonValue::Kind::string; value.text = parse_string(); return value; }
        case 't': case 'f': return parse_boolean();
        case 'n': return parse_null();
        default: return parse_number();
        }
    }
    JsonValue parse_object() {
        expect('{');
        JsonValue value; value.kind = JsonValue::Kind::object;
        skip_space();
        if (peek() == '}') { take(); return value; }
        for (;;) {
            skip_space();
            if (peek() != '"') fail("workspace_json_invalid", "Object keys must be strings");
            auto key = parse_string();
            if (value.find(key) != nullptr) fail("workspace_json_duplicate_key", "Duplicate object key: " + key);
            skip_space();
            expect(':');
            value.members.emplace_back(std::move(key), parse_value());
            skip_space();
            const auto separator = take();
            if (separator == '}') break;
            if (separator != ',') fail("workspace_json_invalid", "Expected ',' or '}' in an object");
        }
        return value;
    }
    JsonValue parse_array() {
        expect('[');
        JsonValue value; value.kind = JsonValue::Kind::array;
        skip_space();
        if (peek() == ']') { take(); return value; }
        for (;;) {
            value.items.push_back(parse_value());
            skip_space();
            const auto separator = take();
            if (separator == ']') break;
            if (separator != ',') fail("workspace_json_invalid", "Expected ',' or ']' in an array");
        }
        return value;
    }
    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            const auto c = take();
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("workspace_json_invalid", "Raw control character in a string");
            if (c != '\\') { out.push_back(c); continue; }
            const auto escape = take();
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                const auto first = parse_hex4();
                std::uint32_t code = first;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (take() != '\\' || take() != 'u') fail("workspace_json_invalid", "Missing low surrogate");
                    const auto second = parse_hex4();
                    if (second < 0xdc00 || second > 0xdfff) fail("workspace_json_invalid", "Invalid low surrogate");
                    code = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
                } else if (first >= 0xdc00 && first <= 0xdfff) {
                    fail("workspace_json_invalid", "Unpaired low surrogate");
                }
                append_utf8(out, code);
                break;
            }
            default: fail("workspace_json_invalid", "Unknown escape sequence");
            }
        }
    }
    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto c = take();
            value <<= 4;
            if (c >= '0' && c <= '9') value |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') value |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= std::uint32_t(c - 'A' + 10);
            else fail("workspace_json_invalid", "Invalid \\u hex digit");
        }
        return value;
    }
    JsonValue parse_number() {
        const auto start = position_;
        if (peek() == '-') ++position_;
        if (peek() == '0') ++position_;
        else {
            if (!is_digit(peek())) fail("workspace_json_invalid", "Invalid JSON value");
            while (is_digit(peek())) ++position_;
        }
        if (peek() == '.') {
            ++position_;
            if (!is_digit(peek())) fail("workspace_json_invalid", "Invalid fraction");
            while (is_digit(peek())) ++position_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') ++position_;
            if (!is_digit(peek())) fail("workspace_json_invalid", "Invalid exponent");
            while (is_digit(peek())) ++position_;
        }
        JsonValue value; value.kind = JsonValue::Kind::number;
        value.text = std::string(document_.substr(start, position_ - start));
        return value;
    }
    JsonValue parse_boolean() {
        JsonValue value; value.kind = JsonValue::Kind::boolean;
        if (document_.substr(position_, 4) == "true") { value.boolean = true; position_ += 4; }
        else if (document_.substr(position_, 5) == "false") { value.boolean = false; position_ += 5; }
        else fail("workspace_json_invalid", "Invalid literal");
        return value;
    }
    JsonValue parse_null() {
        if (document_.substr(position_, 4) != "null") fail("workspace_json_invalid", "Invalid literal");
        position_ += 4;
        return {};
    }
    std::string_view document_;
    std::size_t position_ = 0;
};
void dump_string(std::string& out, std::string_view text) {
    static const char* hex = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xf]); }
            else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}
void dump_value(std::string& out, const JsonValue& value) {
    switch (value.kind) {
    case JsonValue::Kind::null_value: out += "null"; break;
    case JsonValue::Kind::boolean: out += value.boolean ? "true" : "false"; break;
    case JsonValue::Kind::number: out += value.text; break;
    case JsonValue::Kind::string: dump_string(out, value.text); break;
    case JsonValue::Kind::array:
        out.push_back('[');
        for (std::size_t index = 0; index < value.items.size(); ++index) {
            if (index) out.push_back(',');
            dump_value(out, value.items[index]);
        }
        out.push_back(']');
        break;
    case JsonValue::Kind::object:
        out.push_back('{');
        for (std::size_t index = 0; index < value.members.size(); ++index) {
            if (index) out.push_back(',');
            dump_string(out, value.members[index].first);
            out.push_back(':');
            dump_value(out, value.members[index].second);
        }
        out.push_back('}');
        break;
    }
}
JsonValue json_string(std::string text) { JsonValue value; value.kind = JsonValue::Kind::string; value.text = std::move(text); return value; }
JsonValue json_number(std::string token) { JsonValue value; value.kind = JsonValue::Kind::number; value.text = std::move(token); return value; }
JsonValue json_array() { JsonValue value; value.kind = JsonValue::Kind::array; return value; }
JsonValue json_object() { JsonValue value; value.kind = JsonValue::Kind::object; return value; }

// ---------------------------------------------------------------------------
// Frozen path, manifest and budget rules (ADR0040 §4/§5/§6/§7)
// ---------------------------------------------------------------------------
bool is_excluded(const std::string& relative) {
    static const std::set<std::string> excluded_segments{
        "tests", ".git", ".vscode", ".idea", "__pycache__", "build", ".build", "staging", "dist", "out",
        "node_modules", ".pytest_cache", ".mypy_cache"};
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = relative.find('/', start);
        parts.push_back(relative.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    for (std::size_t index = 0; index + 1 < parts.size(); ++index)
        if (excluded_segments.contains(parts[index])) return true;
    for (const auto& part : parts)
        if (!part.empty() && part.front() == '.') return true;
    const auto& last = parts.back();
    return last.size() >= 4 && (last.compare(last.size() - 4, 4, ".pyc") == 0 || last.compare(last.size() - 4, 4, ".pyo") == 0);
}
std::string normalize_relative(const std::string& path, const char* code) {
    if (path.empty()) fail(code, "Empty relative path");
    if (path.size() > kMaximumRelativePathBytes) fail(code, "Relative path is longer than the frozen limit: " + path);
    if (path.find('\\') != std::string::npos) fail(code, "Relative paths must use '/': " + path);
    if (path.front() == '/' || path.rfind("./", 0) == 0) fail(code, "Rooted relative path: " + path);
    std::size_t start = 0;
    bool first = true;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") fail(code, "Empty or dot segment: " + path);
        if (part.back() == '.' || part.back() == ' ') fail(code, "Trailing dot/space segment: " + path);
        if (part.find(':') != std::string::npos || (first && part.size() >= 2 && part[1] == ':'))
            fail(code, "Drive or alternate data stream segment: " + path);
        first = false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return path;
}
std::string ascii_lower(std::string text) {
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
void require_utf8_path_bytes_stable(const std::vector<std::string>& paths) {
    std::map<std::string, std::string> seen;
    for (const auto& path : paths) {
        const auto key = ascii_lower(path);
        const auto existing = seen.find(key);
        if (existing != seen.end() && existing->second != path)
            fail("workspace_path_case_conflict", "Paths differ only by case: " + existing->second + " / " + path);
        seen.emplace(key, path);
    }
}
void collect_directory(const std::filesystem::path& root, const std::string& relative,
                       std::vector<std::string>& files) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(io_path(root / relative), error), end;
    if (error) fail("workspace_io_failed", "Could not enumerate " + relative + ": " + error.message());
    for (; iterator != end; iterator.increment(error)) {
        if (error) fail("workspace_io_failed", "Could not enumerate " + relative + ": " + error.message());
        const auto name = iterator->path().filename();
        const auto entry_relative = relative + "/" + path_to_utf8_generic(name);
        if (is_excluded(entry_relative)) continue;
        if (is_linked(iterator->path())) fail("workspace_path_invalid", "Linked or reparse entry: " + entry_relative);
        if (iterator->is_directory(error)) { collect_directory(root, entry_relative, files); continue; }
        if (!iterator->is_regular_file(error)) fail("workspace_path_invalid", "Not a regular file: " + entry_relative);
        files.push_back(entry_relative);
    }
}
struct WorkspaceDocument {
    std::string language;
    std::string entrypoint;
    std::string source_root = "src";
    std::vector<std::string> declared_sources;
    std::vector<std::string> include_dirs;
    std::vector<std::string> resources;
    JsonValue node;
};
WorkspaceDocument parse_document(const JsonValue& document) {
    if (document.kind != JsonValue::Kind::object) fail("workspace_contract_invalid", "node.json must be an object");
    const auto* schema = document.find("workspace_schema");
    if (!schema || schema->kind != JsonValue::Kind::number || schema->text != "1")
        fail("workspace_schema_unsupported", "workspace_schema must be the integer 1");
    const auto* language = document.find("language");
    if (!language || language->kind != JsonValue::Kind::string) fail("workspace_contract_invalid", "language is required");
    if (language->text != "python" && language->text != "cpp")
        fail("workspace_language_unsupported", "language must be python or cpp");
    WorkspaceDocument result;
    result.language = language->text;
    const auto* node = document.find("node");
    if (!node || node->kind != JsonValue::Kind::object) fail("workspace_contract_invalid", "node must be an object");
    const auto* type_id = node->find("type_id");
    if (!type_id || type_id->kind != JsonValue::Kind::string || type_id->text.empty())
        fail("workspace_contract_invalid", "node.type_id is required");
    result.node = *node;
    if (result.language == "python") {
        const auto* section = document.find("python");
        if (!section || section->kind != JsonValue::Kind::object)
            fail("workspace_contract_invalid", "python section is required for a python workspace");
        const auto* root = section->find("source_root");
        if (!root || root->kind != JsonValue::Kind::string || root->text != "src")
            fail("workspace_contract_invalid", "python.source_root must be src");
        const auto* entrypoint = section->find("entrypoint");
        if (!entrypoint || entrypoint->kind != JsonValue::Kind::string)
            fail("workspace_contract_invalid", "python.entrypoint is required");
        result.entrypoint = entrypoint->text;
    } else {
        const auto* section = document.find("native");
        if (!section || section->kind != JsonValue::Kind::object)
            fail("workspace_contract_invalid", "native section is required for a cpp workspace");
        const auto* sources = section->find("sources");
        if (!sources || sources->kind != JsonValue::Kind::array || sources->items.empty())
            fail("workspace_contract_invalid", "native.sources must be a non-empty array");
        for (const auto& item : sources->items) {
            if (item.kind != JsonValue::Kind::string) fail("workspace_contract_invalid", "native.sources must contain strings");
            result.declared_sources.push_back(normalize_relative(item.text, "workspace_path_invalid"));
        }
        if (const auto* includes = section->find("include_dirs")) {
            if (includes->kind != JsonValue::Kind::array)
                fail("workspace_contract_invalid", "native.include_dirs must be an array");
            for (const auto& item : includes->items) {
                if (item.kind != JsonValue::Kind::string)
                    fail("workspace_contract_invalid", "native.include_dirs must contain strings");
                result.include_dirs.push_back(normalize_relative(item.text, "workspace_path_invalid"));
            }
        }
    }
    if (const auto* resources = document.find("resources")) {
        if (resources->kind != JsonValue::Kind::array)
            fail("workspace_contract_invalid", "resources must be an array");
        for (const auto& item : resources->items) {
            if (item.kind != JsonValue::Kind::string) fail("workspace_contract_invalid", "resources must contain strings");
            result.resources.push_back(normalize_relative(item.text, "workspace_path_invalid"));
        }
    }
    return result;
}
std::string python_entry_path(const std::string& entrypoint) {
    const auto separator = entrypoint.find(':');
    if (separator == std::string::npos) fail("workspace_contract_invalid", "python.entrypoint must be module:function");
    const auto module = entrypoint.substr(0, separator);
    const auto function = entrypoint.substr(separator + 1);
    if (module.empty() || function.empty()) fail("workspace_contract_invalid", "python.entrypoint must be module:function");
    std::string path = "src/";
    for (std::size_t index = 0; index < module.size(); ++index) {
        const auto c = module[index];
        if (c == '.') {
            if (index == 0 || index + 1 == module.size() || module[index - 1] == '.' || module[index + 1] == '.')
                fail("workspace_entrypoint_invalid", "Entrypoint module has an empty segment: " + entrypoint);
            path.push_back('/');
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            path.push_back(c);
        } else {
            fail("workspace_entrypoint_invalid", "Entrypoint module contains an unsupported character: " + entrypoint);
        }
    }
    for (const auto character : function)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
            fail("workspace_entrypoint_invalid", "Entrypoint function contains an unsupported character: " + entrypoint);
    if (!std::isalpha(static_cast<unsigned char>(function.front())) && function.front() != '_')
        fail("workspace_entrypoint_invalid", "Entrypoint function must be an identifier: " + entrypoint);
    path += ".py";
    normalize_relative(path, "workspace_entrypoint_invalid");
    return path;
}
bool valid_draft_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}
std::string manifest_text(const std::vector<WorkspaceFile>& files) {
    std::string text = kManifestMagic;
    for (const auto& file : files) {
        text += file.path; text.push_back('\n');
        text += std::to_string(file.bytes); text.push_back('\n');
        text += file.sha256; text.push_back('\n');
    }
    return text;
}
Workspace open_workspace(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(io_path(directory), error))
        fail("workspace_directory_missing", "Node project directory does not exist: " + path_to_utf8_generic(directory));
    const auto node_json = read_bytes(directory / "node.json", kMaximumNodeJsonBytes, "workspace_node_json_missing");
    const auto document = JsonParser(node_json).parse();
    const auto parsed = parse_document(document);
    // Paths are validated before any existence/enumeration work (ADR0040 §2).
    std::string python_entry;
    if (parsed.language == "python") python_entry = python_entry_path(parsed.entrypoint);
    std::vector<std::string> collected;
    for (const auto* subdirectory : {"src", "include"}) {
        if (std::filesystem::is_directory(io_path(directory / subdirectory), error))
            collect_directory(directory, subdirectory, collected);
    }
    std::vector<WorkspaceFile> files;
    files.push_back({"node.json", node_json.size(), sha256_of(node_json)});
    std::uint64_t source_bytes = node_json.size();
    std::size_t source_files = 1;
    for (const auto& relative : collected) {
        const auto bytes = read_bytes(directory / relative, kMaximumSourceBytes, "workspace_source_missing");
        files.push_back({relative, bytes.size(), sha256_of(bytes)});
        source_bytes += bytes.size();
        ++source_files;
    }
    std::uint64_t resource_bytes = 0;
    for (const auto& relative : parsed.resources) {
        if (is_excluded(relative)) fail("workspace_contract_invalid", "A declared resource is excluded from runtime content: " + relative);
        if (is_linked(directory / relative)) fail("workspace_path_invalid", "Linked or reparse resource: " + relative);
        const auto bytes = read_bytes(directory / relative, kMaximumResourceBytes, "workspace_resource_missing");
        files.push_back({relative, bytes.size(), sha256_of(bytes)});
        resource_bytes += bytes.size();
    }
    std::sort(files.begin(), files.end(), [](const WorkspaceFile& left, const WorkspaceFile& right) {
        return left.path < right.path;
    });
    for (std::size_t index = 1; index < files.size(); ++index)
        if (files[index].path == files[index - 1].path) fail("workspace_path_invalid", "Duplicate runtime path: " + files[index].path);
    std::vector<std::string> all_paths;
    for (const auto& file : files) all_paths.push_back(file.path);
    for (const auto& source : parsed.declared_sources) all_paths.push_back(source);
    for (const auto& resource : parsed.resources) all_paths.push_back(resource);
    require_utf8_path_bytes_stable(all_paths);
    if (source_files > kMaximumSourceFiles) fail("workspace_budget_exceeded", "Source file count exceeds the frozen budget");
    if (source_bytes > kMaximumSourceBytes) fail("workspace_budget_exceeded", "Source bytes exceed the frozen budget");
    if (parsed.resources.size() > kMaximumResourceFiles) fail("workspace_budget_exceeded", "Resource file count exceeds the frozen budget");
    if (resource_bytes > kMaximumResourceBytes) fail("workspace_budget_exceeded", "Resource bytes exceed the frozen budget");
    const auto contains = [&files](const std::string& path) {
        return std::any_of(files.begin(), files.end(), [&path](const WorkspaceFile& file) { return file.path == path; });
    };
    for (const auto& source : parsed.declared_sources)
        if (!contains(source)) fail("workspace_contract_invalid", "A declared native source is not part of the runtime content: " + source);
    for (const auto& resource : parsed.resources)
        if (!contains(resource)) fail("workspace_contract_invalid", "A declared resource is not part of the runtime content: " + resource);
    // The entry source is read while the manifest vector is still intact.
    std::string entry_source;
    if (parsed.language == "python") {
        if (!contains(python_entry))
            fail("workspace_source_missing", "The python entrypoint file is missing: " + python_entry);
        entry_source = read_bytes(directory / python_entry, kMaximumSourceBytes, "workspace_source_missing");
    }
    Workspace workspace;
    workspace.schema = 1;
    workspace.language = parsed.language;
    workspace.entrypoint = parsed.entrypoint;
    std::string declaration;
    dump_value(declaration, parsed.node);
    workspace.declaration = std::move(declaration);
    workspace.node_json = node_json;
    workspace.files = std::move(files);
    workspace.content_sha256 = sha256_of(manifest_text(workspace.files));
    workspace.directory = std::filesystem::absolute(logical_path(directory)).lexically_normal();
    workspace.entry_source = std::move(entry_source);
    return workspace;
}
std::string build_node_json(const Draft& draft, const std::string& language, const std::string& entrypoint) {
    JsonValue document = json_object();
    document.members.emplace_back("workspace_schema", json_number("1"));
    document.members.emplace_back("language", json_string(language));
    document.members.emplace_back("node", JsonParser(draft.declaration).parse());
    if (document.members.back().second.kind != JsonValue::Kind::object)
        fail("workspace_contract_invalid", "The draft declaration must be a JSON object");
    const auto* type_id = document.members.back().second.find("type_id");
    if (!type_id || type_id->kind != JsonValue::Kind::string || type_id->text.empty())
        fail("workspace_contract_invalid", "The draft declaration needs a node type_id");
    if (language == "python") {
        JsonValue section = json_object();
        section.members.emplace_back("source_root", json_string("src"));
        section.members.emplace_back("entrypoint", json_string(entrypoint));
        document.members.emplace_back("python", std::move(section));
    } else {
        JsonValue section = json_object();
        JsonValue sources = json_array();
        sources.items.push_back(json_string("src/node.cpp"));
        section.members.emplace_back("sources", std::move(sources));
        section.members.emplace_back("include_dirs", json_array());
        document.members.emplace_back("native", std::move(section));
    }
    document.members.emplace_back("resources", json_array());
    std::string text;
    dump_value(text, document);
    return text;
}
struct PlannedFile {
    std::string relative;
    std::string bytes;
};
Workspace create_workspace(const std::filesystem::path& target, const Draft& draft,
                          const std::function<bool(WorkspacePhase)>& allow) {
    if (!valid_draft_id(draft.id)) fail("workspace_contract_invalid", "Draft id is not usable as a workspace identity");
    if (draft.language != "python" && draft.language != "cpp")
        fail("workspace_language_unsupported", "Draft language must be python or cpp");
    const auto module = workspace_module_name(draft.id);
    const auto entrypoint = module + ".process:process";
    const auto node_json = build_node_json(draft, draft.language, entrypoint);
    std::vector<PlannedFile> planned;
    planned.push_back({"node.json", node_json});
    if (draft.language == "python") {
        planned.push_back({"src/" + module + "/__init__.py", {}});
        planned.push_back({"src/" + module + "/process.py", draft.source});
    } else {
        planned.push_back({"src/node.cpp", draft.source});
    }
    const auto io_target = io_path(target);
    std::error_code error;
    bool created_target = false;
    if (std::filesystem::exists(io_target, error)) {
        if (!std::filesystem::is_directory(io_target, error))
            fail("workspace_target_not_a_directory", "The target exists and is not a directory: " + path_to_utf8_generic(target));
        if (!std::filesystem::is_empty(io_target, error))
            fail("workspace_target_not_empty", "The target directory is not empty: " + path_to_utf8_generic(target));
    } else {
        if (allow && !allow(WorkspacePhase::create))
            fail("workspace_write_failed", "Injected workspace failure at phase 'create'");
        // Reuse every existing ancestor (volume and share roots included) and only
        // create the missing segments, mirroring the pinned draft-save fix.
        std::filesystem::create_directories(io_target, error);
        if (error) fail("workspace_io_failed", "Could not create the workspace directory: " + error.message());
        created_target = true;
    }
    std::vector<std::filesystem::path> written;
    std::vector<std::filesystem::path> created_directories;
    try {
        for (const auto& file : planned) {
            if (file.relative.find('/') != std::string::npos) {
                const auto parent = std::filesystem::path(file.relative).parent_path();
                if (!std::filesystem::is_directory(io_path(target / parent), error)) {
                    std::filesystem::create_directories(io_path(target / parent), error);
                    created_directories.push_back(target / parent);
                }
                if (error) fail("workspace_io_failed", "Could not create a source directory: " + error.message());
            }
            write_atomic(target / file.relative, file.bytes, allow);
            written.push_back(target / file.relative);
        }
        return open_workspace(target);
    } catch (...) {
        // Only remove files and directories this call created; pre-existing target
        // bytes and pre-existing empty directories are never disturbed.
        for (auto iterator = written.rbegin(); iterator != written.rend(); ++iterator) {
            std::error_code ignored;
            std::filesystem::remove(*iterator, ignored);
        }
        for (auto iterator = created_directories.rbegin(); iterator != created_directories.rend(); ++iterator) {
            std::error_code ignored;
            std::filesystem::remove(io_path(*iterator), ignored);
        }
        if (created_target) {
            std::error_code ignored;
            std::filesystem::remove(io_target, ignored);
        }
        throw;
    }
}
}

std::string workspace_module_name(const std::string& draft_id) {
    std::string module;
    for (const auto character : draft_id) {
        if (character == '-') module.push_back('_');
        else module.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (module.empty()) module = "node";
    if (std::isdigit(static_cast<unsigned char>(module.front()))) module = "node_" + module;
    return module;
}
std::string WorkspaceStore::sha256_hex(const std::string& bytes) { return sha256_of(bytes); }
std::string WorkspaceStore::canonical_json(const std::string& document, std::string& error) {
    try {
        std::string text;
        dump_value(text, JsonParser(document).parse());
        error.clear();
        return text;
    } catch (const WorkspaceFailure& failure) {
        error = failure.code + ": " + failure.message;
        return {};
    }
}
WorkspaceOutcome WorkspaceStore::create(const std::filesystem::path& target, const Draft& draft) const {
    try {
        return create_workspace(target, draft, allow_);
    } catch (const WorkspaceFailure& failure) {
        return diagnostic_of(failure);
    } catch (const std::exception& error) {
        return Diagnostic{"workspace_io_failed", error.what()};
    }
}
WorkspaceOutcome WorkspaceStore::open(const std::filesystem::path& directory) const {
    try {
        return open_workspace(directory);
    } catch (const WorkspaceFailure& failure) {
        return diagnostic_of(failure);
    } catch (const std::exception& error) {
        return Diagnostic{"workspace_io_failed", error.what()};
    }
}
WorkspaceOutcome WorkspaceStore::convert(const std::filesystem::path& draft_file,
                                         const std::filesystem::path& target) const {
    try {
        const auto loaded = DraftFiles{}.load(draft_file);
        if (const auto* error = std::get_if<Diagnostic>(&loaded)) return *error;
        const auto& draft_file_bytes = std::get<DraftFile>(loaded);
        const auto original_sha = sha256_of(draft_file_bytes.bytes);
        auto created = create(target, draft_file_bytes.document);
        if (const auto* error = std::get_if<Diagnostic>(&created)) return *error;
        const auto rechecked = DraftFiles{}.load(draft_file);
        if (const auto* error = std::get_if<Diagnostic>(&rechecked)) return *error;
        if (sha256_of(std::get<DraftFile>(rechecked).bytes) != original_sha ||
            std::get<DraftFile>(rechecked).bytes != draft_file_bytes.bytes)
            return Diagnostic{"workspace_original_changed", "The legacy draft changed while the node project was created"};
        return created;
    } catch (const WorkspaceFailure& failure) {
        return diagnostic_of(failure);
    } catch (const std::exception& error) {
        return Diagnostic{"workspace_io_failed", error.what()};
    }
}
}
