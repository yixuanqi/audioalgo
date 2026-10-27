#pragma once
#include "Draft.h"
#include "Workspace.h"
#include "audio_node_sdk/NodeSDK.h"
#include <atomic>

namespace audioalgo::authoring {
struct CheckLogEvent {
    std::string draft_id, source_sha256, declaration_sha256, request_id, phase;
    std::uint64_t generation = 0;
    NodeLogEvent event;
};
struct SourceDiagnostic {
    std::string code, message, file;
    int line = 0, column = 0;
    std::vector<CheckLogEvent> logs;
};
struct PackageSelection {
    std::string node_id, type_id, package_id, content_sha256, package_path, wrapper_version;
    bool operator==(const PackageSelection&) const = default;
};
struct CatalogResolution {
    std::shared_ptr<const NodeRegistry> catalog;
    std::vector<Diagnostic> diagnostics;
};
struct ReadyPackage {
    Draft draft;
    std::optional<Workspace> workspace;
    std::string request_id, source_sha256, declaration_sha256, package_sha256, interpreter_sha256, runner_sha256;
    std::string wrapper_version = "python-function-v1";
    NodeDescriptor descriptor;
    std::filesystem::path package_root, probe_directory;
    AudioPorts probe_outputs;
    std::uint32_t syntax_pid = 0, probe_pid = 0;
    std::vector<CheckLogEvent> logs;
};
using CheckOutcome = std::variant<ReadyPackage, SourceDiagnostic>;
struct AppliedPackage {
    std::shared_ptr<const NodeRegistry> catalog;
    std::string package_id, content_sha256;
    std::filesystem::path package_root;
};
using ApplyOutcome = std::variant<AppliedPackage, SourceDiagnostic>;
class ApplyCommit {
public:
    virtual ~ApplyCommit() = default;
    virtual ApplyOutcome commit(const std::shared_ptr<std::atomic<bool>>&) const = 0;
};
struct PreparedPackage {
    AppliedPackage package;
    std::shared_ptr<const ApplyCommit> transaction;
};
using PrepareOutcome = std::variant<PreparedPackage, SourceDiagnostic>;
class AuthoringChecks {
public:
    virtual ~AuthoringChecks() = default;
    // Worker-only; value inputs, complete cancellation ownership, no GUI dependencies.
    virtual CheckOutcome check(Draft, const std::shared_ptr<std::atomic<bool>>& cancel) const = 0;
    virtual CheckOutcome check_workspace(Draft, const Workspace&, const std::shared_ptr<std::atomic<bool>>&) const {
        return SourceDiagnostic{"workspace_check_unavailable", "This host cannot check node project folders"};
    }
    virtual std::shared_ptr<const AuthoringChecks> with_work_directory(const std::filesystem::path&) const { return {}; }
    virtual std::filesystem::path work_directory() const { return {}; }
    virtual std::filesystem::path library_directory() const { return {}; }
    virtual std::optional<SourceDiagnostic> copy_library_to(const std::filesystem::path&) const {
        return SourceDiagnostic{"authoring_library_copy_unavailable", "This host cannot copy the installed library"};
    }
    virtual CatalogResolution resolve_catalog(std::shared_ptr<const NodeRegistry> base,
        const std::vector<PackageSelection>& selections, const std::filesystem::path&) const {
        CatalogResolution result{std::move(base), {}};
        for (const auto& selection : selections)
            result.diagnostics.push_back({"managed_version_unavailable", "This host cannot resolve exact package versions", selection.node_id});
        return result;
    }
    virtual std::optional<SourceDiagnostic> carry_packages(const std::vector<PackageSelection>& selections,
        const std::filesystem::path&, const std::filesystem::path&) const {
        if (selections.empty()) return {};
        return SourceDiagnostic{"authoring_carry_failed", "This host cannot carry exact package versions"};
    }
    virtual PrepareOutcome prepare_apply(const ReadyPackage&, const Draft&, std::shared_ptr<const NodeRegistry>,
                                         const std::shared_ptr<std::atomic<bool>>&) const {
        return SourceDiagnostic{"authoring_apply_unavailable", "This host does not support preparing a catalog transaction"};
    }
    virtual ApplyOutcome apply(const ReadyPackage&, const Draft&, std::shared_ptr<const NodeRegistry>,
                               const std::shared_ptr<std::atomic<bool>>&) const {
        return SourceDiagnostic{"authoring_apply_unavailable", "This host does not support applying packages", "", 0, 0};
    }
};
}
