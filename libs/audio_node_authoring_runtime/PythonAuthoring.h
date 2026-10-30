#pragma once
#include "audio_node_authoring/AuthoringChecks.h"
#include <chrono>
#include <mutex>

namespace audioalgo::authoring {
// Worker-side transaction checkpoints; these are unrelated to the published C ABI.
enum class ApplyPhase { package, registration, index_write, index_verify, index_replace, package_publish, package_retry };
class PythonAuthoring final : public AuthoringChecks {
public:
    PythonAuthoring(std::filesystem::path root, std::filesystem::path interpreter = {},
                    std::filesystem::path runner = {}, std::function<bool(ApplyPhase)> allow = {}, std::filesystem::path work = {},
                    std::chrono::milliseconds execution_timeout = std::chrono::seconds(30));
    std::shared_ptr<const AuthoringChecks> with_work_directory(const std::filesystem::path&) const override;
    std::filesystem::path work_directory() const override { return work_; }
    std::filesystem::path library_directory() const override { return root_; }
    std::optional<SourceDiagnostic> copy_library_to(const std::filesystem::path&) const override;
    CheckOutcome check(Draft, const std::shared_ptr<std::atomic<bool>>& cancel) const override;
    CheckOutcome check_workspace(Draft, const Workspace&, const std::shared_ptr<std::atomic<bool>>&) const override;
    PrepareOutcome prepare_apply(const ReadyPackage&, const Draft&, std::shared_ptr<const NodeRegistry>,
                                 const std::shared_ptr<std::atomic<bool>>&) const override;
    ApplyOutcome apply(const ReadyPackage&, const Draft&, std::shared_ptr<const NodeRegistry>,
                       const std::shared_ptr<std::atomic<bool>>&) const override;
    std::variant<NodeRegistry, SourceDiagnostic> restore_defaults(const NodeRegistry&) const;
    CatalogResolution resolve_catalog(std::shared_ptr<const NodeRegistry>, const std::vector<PackageSelection>&,
        const std::filesystem::path& project_directory) const override;
    std::optional<SourceDiagnostic> carry_packages(const std::vector<PackageSelection>&,
        const std::filesystem::path& source_directory, const std::filesystem::path& destination_directory) const override;
private:
    CheckOutcome check_inputs(Draft, std::optional<Workspace>, const std::shared_ptr<std::atomic<bool>>&) const;
    std::filesystem::path root_, interpreter_, runner_, work_;
    std::chrono::milliseconds execution_timeout_;
    std::shared_ptr<std::mutex> apply_mutex_ = std::make_shared<std::mutex>();
    std::function<bool(ApplyPhase)> allow_;
};
std::filesystem::path configured_authoring_root(const std::filesystem::path& settings_file = {});
std::filesystem::path configured_authoring_work_directory(const std::filesystem::path& settings_file = {});
std::filesystem::path configured_authoring_recovery_directory(const std::filesystem::path& settings_file = {});
SaveResult save_authoring_location(bool recovery, const std::filesystem::path&, const std::filesystem::path& settings_file = {});
SaveResult save_authoring_library_location(const std::filesystem::path&, const std::filesystem::path& settings_file = {});
std::string authoring_launch_diagnostic();
}
