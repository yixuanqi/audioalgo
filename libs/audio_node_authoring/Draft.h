#pragma once
#include "audio_core/Diagnostic.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace audioalgo::authoring {
// Declaration is the single imported definition document. Incomplete documents
// are valid drafts; SDK validation is required before checking/applying a package.
struct Draft {
    std::string id;
    std::uint64_t generation = 1;
    std::string language = "python";
    std::string declaration;
    std::string source;
    bool operator==(const Draft&) const = default;
};
using SaveResult = std::optional<Diagnostic>;
SaveResult probe_authoring_directory(const std::filesystem::path&);
// Parent used by draft I/O, including extended Windows volume/share roots.
std::filesystem::path draft_parent_directory(const std::filesystem::path&);
using LoadResult = std::variant<Draft, Diagnostic>;
using ListResult = std::variant<std::vector<std::string>, Diagnostic>;
struct DraftRecovery {
    Draft document;
    std::filesystem::path original_file;
    std::optional<std::string> expected_bytes;
    std::string warning;
    std::filesystem::path recovery_file;
    std::int64_t updated_unix_seconds = 0;
};
using RecoveryOutcome = std::variant<DraftRecovery, Diagnostic>;
class DraftStorage {
public:
    virtual ~DraftStorage() = default;
    virtual SaveResult save(const Draft&) const = 0;
    virtual LoadResult load(const std::string& id) const = 0;
    virtual ListResult list() const = 0;
    virtual std::filesystem::path directory() const { return {}; }
    virtual SaveResult save_recovery(const Draft& draft, const std::filesystem::path&, const std::optional<std::string>&) const { return save(draft); }
    virtual RecoveryOutcome load_recovery(const std::string& id) const {
        auto result = load(id);
        if (auto* error = std::get_if<Diagnostic>(&result)) return *error;
        return DraftRecovery{std::get<Draft>(std::move(result)), {}, {}, {}};
    }
};
enum class SavePhase { write, verify, replace };
class FileDraftStorage final : public DraftStorage {
public:
    explicit FileDraftStorage(std::filesystem::path root,
        std::function<bool(SavePhase)> allow = {});
    SaveResult save(const Draft&) const override;
    LoadResult load(const std::string& id) const override;
    ListResult list() const override;
    std::filesystem::path file_path(const std::string& id) const;
    std::filesystem::path directory() const override { return root_; }
    SaveResult save_recovery(const Draft&, const std::filesystem::path&, const std::optional<std::string>&) const override;
    RecoveryOutcome load_recovery(const std::string&) const override;
private:
    std::filesystem::path root_;
    std::function<bool(SavePhase)> allow_;
};
struct DraftFile {
    Draft document;
    std::string bytes;
};
using DraftFileOutcome = std::variant<DraftFile, Diagnostic>;
using DraftTargetOutcome = std::variant<std::monostate, DraftFile, Diagnostic>;
// A missing expected value means that the target must not exist.
class DraftFiles {
public:
    explicit DraftFiles(std::function<bool(SavePhase)> allow = {}) : allow_(std::move(allow)) {}
    DraftFileOutcome load(const std::filesystem::path&) const;
    DraftTargetOutcome inspect_target(const std::filesystem::path&) const;
    DraftFileOutcome save(const std::filesystem::path&, const Draft&, const std::optional<std::string>& expected) const;
private:
    std::function<bool(SavePhase)> allow_;
};
// Injected monotonic milliseconds; no sleeps or GUI clock in the document model.
class DraftSession {
public:
    explicit DraftSession(Draft draft, bool saved = false);
    void edit(std::string source, std::string declaration, std::uint64_t now_ms);
    bool save_due(std::uint64_t now_ms) const;
    void saved(std::uint64_t generation);
    bool recovery_due(std::uint64_t now_ms) const;
    void recovered(std::uint64_t generation);
    bool recovery_current() const { return recovered_generation_ == draft_.generation; }
    void invalidate_recovery() { recovered_generation_ = 0; }
    const Draft& document() const { return draft_; }
    bool dirty() const { return saved_generation_ != draft_.generation; }
private:
    Draft draft_;
    std::uint64_t saved_generation_ = 0, recovered_generation_ = 0, changed_at_ = 0;
};
enum class PythonTemplate { stateless_gain, stateful_ramp, named_router };
Draft python_draft(std::string id, PythonTemplate kind = PythonTemplate::stateless_gain);
}
