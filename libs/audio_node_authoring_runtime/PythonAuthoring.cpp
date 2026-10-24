#include "PythonAuthoring.h"
#include "audio_native_plugins/NativePlugins.h"
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <fstream>
#include <regex>
#include <cmath>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef AUDIOALGO_AUTHORING_PYTHON
#include "audio_python_backend/PythonTask.h"
#endif

namespace audioalgo::authoring {
namespace {
juce::String js(const std::string& text) { return juce::String::fromUTF8(text.data(), static_cast<int>(text.size())); }
std::string hash(const std::string& text) { return juce::SHA256(text.data(), text.size()).toHexString().toStdString(); }
std::string file_hash(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return juce::SHA256(juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size())))).toHexString().toStdString();
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(text.data(), static_cast<std::streamsize>(text.size())); out.close();
    if (!out) throw std::runtime_error("Could not write generated package");
}
std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto s = std::filesystem::absolute(path).lexically_normal().wstring();
    if (s.rfind(L"\\\\?\\", 0) != 0) s = s.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + s.substr(2) : L"\\\\?\\" + s;
    return s;
#else
    return path;
#endif
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void unlinked(const std::filesystem::path& path) {
    auto current = std::filesystem::absolute(path).root_path();
    for (const auto& part : std::filesystem::absolute(path).lexically_normal().relative_path()) {
        current /= part;
        std::error_code error;
        require(!std::filesystem::is_symlink(std::filesystem::symlink_status(io_path(current), error)), "Linked authoring path rejected");
#ifdef _WIN32
        auto attributes = GetFileAttributesW(io_path(current).c_str());
        require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse authoring path rejected");
#endif
    }
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(io_path(path), std::ios::binary);
    require(stream.is_open(), "Cannot read authoring index");
    std::string bytes(1024 * 1024 + 1, '\0'); stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(!stream.bad() && stream.gcount() <= 1024 * 1024, "Authoring index exceeds budget");
    bytes.resize(static_cast<std::size_t>(stream.gcount())); return bytes;
}
bool content_hash(const std::string& value) { return std::regex_match(value, std::regex("[0-9a-f]{64}")); }
bool package_id(const std::string& value) { return std::regex_match(value, std::regex("user\\.authoring\\.[0-9a-f]{24}")); }
juce::var index_at(const std::filesystem::path& root) {
    unlinked(root / "index.json");
    if (!std::filesystem::exists(io_path(root / "index.json"))) {
        juce::DynamicObject::Ptr index = new juce::DynamicObject;
        index->setProperty("schema_version", 1); index->setProperty("packages", juce::Array<juce::var>{});
        return juce::var(index.get());
    }
    auto index = juce::JSON::parse(js(read(root / "index.json")));
    require(index.isObject() && index["schema_version"] == juce::var(1) && index["packages"].isArray(), "Invalid authoring index");
    std::set<std::string> ids;
    for (const auto& entry : *index["packages"].getArray()) {
        const auto id = entry["package_id"].toString().toStdString();
        require(entry.isObject() && package_id(id) && content_hash(entry["content_sha256"].toString().toStdString()) &&
            entry["wrapper_version"].toString() == "python-function-v1" && ids.insert(id).second, "Invalid or duplicate authoring index entry");
    }
    return index;
}
#ifdef AUDIOALGO_AUTHORING_PYTHON
class IndexCommit final : public ApplyCommit {
public:
    AppliedPackage package;
    std::filesystem::path root, temporary;
    std::optional<std::string> original;
    std::string candidate_json;
    std::shared_ptr<std::mutex> mutex;
    std::function<bool(ApplyPhase)> allow;
    ~IndexCommit() override {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(io_path(temporary), ignored); }
    }
    ApplyOutcome commit(const std::shared_ptr<std::atomic<bool>>& cancel) const override {
        std::lock_guard lock(*mutex);
        try {
            require(!cancel || !cancel->load(), "Apply cancelled before commit");
            if (committed_) return package;
            unlinked(root / "index.json");
            const auto current = std::filesystem::exists(io_path(root / "index.json")) ? std::optional(read(root / "index.json")) : std::nullopt;
            require(current == original, "Authoring index changed while preparing the refresh");
            require(read(temporary) == candidate_json, "Prepared index changed before commit");
            if (auto error = validate_python_snapshot(package.package_root)) throw std::runtime_error(error->code + ": " + error->message);
            require(!allow || allow(ApplyPhase::index_replace), "Injected index replacement failure");
            require(!cancel || !cancel->load(), "Apply cancelled before commit");
#ifdef _WIN32
            require(MoveFileExW(io_path(temporary).c_str(), io_path(root / "index.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "Atomic index replacement failed");
#else
            std::filesystem::rename(temporary, root / "index.json");
#endif
            committed_ = true; return package;
        } catch (const std::exception& error) { return SourceDiagnostic{cancel && cancel->load() ? "run_cancelled" : "authoring_apply_failed", error.what()}; }
    }
private:
    mutable bool committed_ = false;
};
class ManagedPythonProcessor final : public AudioNodeProcessor {
public:
    ManagedPythonProcessor(std::filesystem::path path, std::string digest, std::string type)
        : path_(std::move(path)), digest_(std::move(digest)), type_(std::move(type)) {}
    std::optional<Diagnostic> freeze_task(const NodeExecutionContext& context) override {
        unlinked(path_);
        if (auto error = validate_python_snapshot(path_, context.is_cancelled)) { error->node_id = context.node_id; return error; }
        // Construct the ordinary backend from the verified owned copy, so a
        // later change of the installed directory cannot alter this run.
        auto capture = freeze_python_package(path_, std::filesystem::u8path(context.output_directory) / "managed-packages", "", context.is_cancelled);
        if (auto* error = std::get_if<Diagnostic>(&capture)) { error->node_id = context.node_id; return *error; }
        const auto frozen = std::get<std::filesystem::path>(capture);
        if (frozen.filename().string() != digest_) return Diagnostic{"python_snapshot_modified", "Managed package content differs from the exact version lock", context.node_id};
        NodeRegistry catalog;
        const auto discovery = load_native_plugins(catalog, {io_path(frozen)});
        if (!discovery.diagnostics.empty()) return Diagnostic{discovery.diagnostics.front().code, discovery.diagnostics.front().message, context.node_id};
        auto created = catalog.create_processor(type_, context.node_id);
        if (!created.processor) return Diagnostic{"managed_version_unavailable", "Verified package does not provide the locked type", context.node_id};
        auto candidate = std::move(created.processor);
        if (auto error = candidate->freeze_task(context)) return error;
        delegate_ = std::move(candidate); return {};
    }
    NodeProcessOutcome process(const NodeProcessRequest& request) override {
        if (!delegate_) if (auto error = freeze_task(request.context)) return *error;
        return delegate_->process(request);
    }
    void reset() noexcept override { if (delegate_) delegate_->reset(); }
private:
    std::filesystem::path path_;
    std::string digest_, type_;
    std::unique_ptr<AudioNodeProcessor> delegate_;
};
void register_managed(NodeRegistry& catalog, const std::filesystem::path& path, const std::string& id, const std::string& digest) {
    require(package_id(id) && content_hash(digest), "Invalid managed package identity");
    unlinked(path);
    if (auto error = validate_python_snapshot(path)) throw std::runtime_error(error->code + ": " + error->message);
    require(path.filename().string() == digest, "Managed directory identity mismatch");
    auto package = std::make_shared<NodeRegistry>();
    auto discovery = load_native_plugins(*package, {io_path(path)});
    if (!discovery.diagnostics.empty()) throw std::runtime_error("Managed package registration failed: " + discovery.diagnostics.front().code + ": " + discovery.diagnostics.front().message);
    require(discovery.loaded_plugin_ids == std::vector<std::string>{id}, "Managed package identity was not registered");
    auto candidate = catalog;
    for (const auto& [type, original] : package->descriptors()) {
        auto descriptor = original;
        require(descriptor.plugin_origin && descriptor.plugin_origin->runtime == "python", "Managed runtime mismatch");
        descriptor.plugin_origin->managed_content_sha256 = digest;
        descriptor.plugin_origin->managed_wrapper_version = "python-function-v1";
        descriptor.plugin_origin->managed_package_path = "node_packages/" + id + "/" + digest;
        if (const auto* existing = candidate.find(type)) {
            require(existing->plugin_origin && existing->plugin_origin->plugin_id == id &&
                existing->plugin_origin->managed_content_sha256 == digest, "Type already registered to another exact version");
            continue;
        }
        auto result = candidate.register_node(std::move(descriptor), [path, digest, type] {
            return std::make_unique<ManagedPythonProcessor>(path, digest, type);
        });
        require(result.registered, "Managed node registration failed");
    }
    catalog.swap(candidate);
}
#endif
NodeRegistry indexed_catalog(const NodeRegistry& base, const std::filesystem::path& root, const juce::var& index, const std::string& exclude = {}) {
    auto catalog = base.subset([](const NodeDescriptor& descriptor) {
        return !descriptor.plugin_origin || descriptor.plugin_origin->managed_content_sha256.empty();
    });
    for (const auto& entry : *index["packages"].getArray()) {
        const auto id = entry["package_id"].toString().toStdString(), digest = entry["content_sha256"].toString().toStdString();
        if (id == exclude) continue;
#ifdef AUDIOALGO_AUTHORING_PYTHON
        register_managed(catalog, root / "packages" / id / digest, id, digest);
#else
        throw std::runtime_error("Python execution is disabled in this build");
#endif
    }
    return catalog;
}
constexpr auto wrapper = R"PY(import json
from pathlib import Path
from audioalgo_runner.sdk import AudioPort, DiagnosticError
from . import user_source

class Node:
    @classmethod
    def describe(cls):
        return json.loads(Path(__file__).with_name('manifest.json').read_text(encoding='utf-8'))['nodes'][0]
    def prepare(self, context):
        self.state = {}
        initialize = getattr(user_source, 'initialize', None)
        if initialize is not None: initialize(context.parameters, self.state)
        if not callable(getattr(user_source, 'process', None)):
            raise DiagnosticError('authoring_entrypoint_missing', 'user_source.py must define process(inputs, params, state)')
    def process(self, inputs, context):
        reference = next(iter(inputs.values()))
        result = user_source.process({key: value.samples for key, value in inputs.items()}, context.parameters, self.state)
        if not isinstance(result, dict):
            raise DiagnosticError('python_output_ports_invalid', 'process must return a dict keyed by declared output port')
        return {key: AudioPort(value, reference.sample_rate_hz, reference.channel_ids, reference.valid_interval) for key, value in result.items()}
    def reset(self, reason):
        reset = getattr(user_source, 'reset', None)
        if reset is not None: reset(self.state, reason)
        self.state = {}
)PY";
constexpr auto syntax_wrapper = R"PY(import json
from pathlib import Path
from audioalgo_runner.sdk import AudioPort, DiagnosticError

class Node:
    @classmethod
    def describe(cls):
        return json.loads(Path(__file__).with_name('manifest.json').read_text(encoding='utf-8'))['nodes'][0]
    def prepare(self, context):
        for path in sorted(Path(__file__).parent.glob('*.py')):
            try: compile(path.read_bytes(), str(path), 'exec', dont_inherit=True)
            except SyntaxError as error:
                raise DiagnosticError('python_syntax_error', f'{path.name}:{error.lineno or 0}:{error.offset or 0}: {error.msg}') from None
    def process(self, inputs, context):
        reference = next(iter(inputs.values()))
        return {port['id']: AudioPort(reference.samples, reference.sample_rate_hz, reference.channel_ids, reference.valid_interval) for port in self.describe()['outputs']}
    def reset(self, reason): pass
)PY";
}
PythonAuthoring::PythonAuthoring(std::filesystem::path root, std::filesystem::path interpreter, std::filesystem::path runner,
                               std::function<bool(ApplyPhase)> allow, std::filesystem::path work)
    : root_(std::move(root)), interpreter_(std::move(interpreter)), runner_(std::move(runner)), work_(work.empty() ? root_ : std::move(work)), allow_(std::move(allow)) {
#ifdef AUDIOALGO_AUTHORING_PYTHON
    if (interpreter_.empty()) interpreter_ = AUDIOALGO_AUTHORING_INTERPRETER;
    if (runner_.empty()) runner_ = AUDIOALGO_AUTHORING_RUNNER;
#endif
}
std::shared_ptr<const AuthoringChecks> PythonAuthoring::with_work_directory(const std::filesystem::path& work) const {
    auto result = std::make_shared<PythonAuthoring>(root_, interpreter_, runner_, allow_, work);
    result->apply_mutex_ = apply_mutex_;
    return result;
}
ApplyOutcome PythonAuthoring::apply(const ReadyPackage& ready, const Draft& draft, std::shared_ptr<const NodeRegistry> base,
                                   const std::shared_ptr<std::atomic<bool>>& cancel) const {
    auto prepared = prepare_apply(ready, draft, std::move(base), cancel);
    if (auto* error = std::get_if<SourceDiagnostic>(&prepared)) return *error;
    return std::get<PreparedPackage>(prepared).transaction->commit(cancel);
}
PrepareOutcome PythonAuthoring::prepare_apply(const ReadyPackage& ready, const Draft& draft, std::shared_ptr<const NodeRegistry> base,
                                             const std::shared_ptr<std::atomic<bool>>& cancel) const {
#ifndef AUDIOALGO_AUTHORING_PYTHON
    return SourceDiagnostic{"python_disabled", "Python execution is disabled in this build"};
#else
    std::lock_guard lock(*apply_mutex_);
    std::filesystem::path temporary;
    try {
        const auto cancelled = [cancel] { return cancel && cancel->load(); };
        auto checkpoint = [&](ApplyPhase phase) { require(!cancelled(), "Apply cancelled"); require(!allow_ || allow_(phase), "Injected apply storage/registration failure"); };
        require(base && draft == ready.draft && hash(draft.source) == ready.source_sha256 &&
            hash(draft.declaration) == ready.declaration_sha256 && ready.wrapper_version == "python-function-v1", "Ready no longer matches draft");
        const auto id = "user.authoring." + hash(draft.id).substr(0, 24);
        require(content_hash(ready.package_sha256) && ready.package_root.filename().string() == ready.package_sha256, "Invalid Ready package identity");
        unlinked(root_); unlinked(ready.package_root);
        const auto relative = std::filesystem::absolute(ready.package_root).lexically_normal().lexically_relative(std::filesystem::absolute(work_ / "staging").lexically_normal());
        require(!relative.empty() && *relative.begin() != "..", "Ready package is outside authoring staging");
        if (auto error = validate_python_snapshot(ready.package_root, cancelled)) throw std::runtime_error(error->code + ": " + error->message);
        require(file_hash(ready.package_root / "user_source.py") == ready.source_sha256 && file_hash(interpreter_) == ready.interpreter_sha256, "Checked source or environment changed");
        const auto transaction = work_ / "staging" / ("apply-" + juce::Uuid().toString().toStdString());
        auto runner = freeze_python_package(runner_, transaction / "runner", "", cancelled);
        require(std::holds_alternative<std::filesystem::path>(runner) && std::get<std::filesystem::path>(runner).filename().string() == ready.runner_sha256, "Checked runner environment changed");
        checkpoint(ApplyPhase::package);
        // Install staging shares the library volume; the user-selected work
        // directory may be on another drive.
        auto capture = freeze_python_package(ready.package_root, root_ / "install-staging" / juce::Uuid().toString().toStdString(), "", cancelled);
        require(std::holds_alternative<std::filesystem::path>(capture) && std::get<std::filesystem::path>(capture).filename().string() == ready.package_sha256, "Ready capture failed");
        auto target = root_ / "packages" / id / ready.package_sha256;
        unlinked(target); std::filesystem::create_directories(io_path(target.parent_path()));
        if (!std::filesystem::exists(io_path(target))) std::filesystem::rename(io_path(std::get<std::filesystem::path>(capture)), io_path(target));
        // Complete but unindexed orphan versions are harmless after a failed transaction.
        checkpoint(ApplyPhase::registration);
        auto original = std::filesystem::exists(io_path(root_ / "index.json")) ? std::optional(read(root_ / "index.json")) : std::nullopt;
        auto index = index_at(root_);
        auto candidate = std::make_shared<NodeRegistry>(indexed_catalog(*base, root_, index, id));
        register_managed(*candidate, target, id, ready.package_sha256);
        auto* packages = index.getDynamicObject()->getProperty("packages").getArray();
        bool present = false;
        for (auto& entry : *packages) if (entry["package_id"].toString().toStdString() == id) {
            entry.getDynamicObject()->setProperty("content_sha256", js(ready.package_sha256));
            present = true;
        }
        if (!present) {
            juce::DynamicObject::Ptr entry = new juce::DynamicObject;
            entry->setProperty("package_id", js(id)); entry->setProperty("content_sha256", js(ready.package_sha256));
            entry->setProperty("wrapper_version", js(ready.wrapper_version)); packages->add(juce::var(entry.get()));
        }
        const auto json = juce::JSON::toString(index, false).toStdString();
        temporary = root_ / ("index-" + juce::Uuid().toString().toStdString() + ".tmp");
        checkpoint(ApplyPhase::index_write); write(io_path(temporary), json);
        checkpoint(ApplyPhase::index_verify); require(read(temporary) == json, "Index verification failed");
        auto transaction_state = std::make_shared<IndexCommit>();
        transaction_state->root = root_; transaction_state->temporary = temporary; transaction_state->original = std::move(original);
        transaction_state->candidate_json = json; transaction_state->mutex = apply_mutex_; transaction_state->allow = allow_;
        transaction_state->package = AppliedPackage{std::move(candidate), id, ready.package_sha256, target};
        temporary.clear();
        return PreparedPackage{transaction_state->package, transaction_state};
    } catch (const std::exception& error) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(io_path(temporary), ignored); }
        return SourceDiagnostic{cancel && cancel->load() ? "run_cancelled" : "authoring_apply_failed", error.what()};
    }
#endif
}
std::variant<NodeRegistry, SourceDiagnostic> PythonAuthoring::restore_defaults(const NodeRegistry& base) const {
    std::lock_guard lock(*apply_mutex_);
    try {
        return indexed_catalog(base, root_, index_at(root_));
    } catch (const std::exception& error) { return SourceDiagnostic{"authoring_restore_failed", error.what()}; }
}
CatalogResolution PythonAuthoring::resolve_catalog(std::shared_ptr<const NodeRegistry> base,
    const std::vector<PackageSelection>& selections, const std::filesystem::path& project_directory) const {
    std::lock_guard lock(*apply_mutex_);
    std::set<std::string> selected;
    for (const auto& selection : selections) selected.insert(selection.package_id);
    auto catalog = std::make_shared<NodeRegistry>(base->subset([&](const NodeDescriptor& descriptor) {
        return !descriptor.plugin_origin || descriptor.plugin_origin->managed_content_sha256.empty() ||
            !selected.contains(descriptor.plugin_origin->plugin_id);
    }));
    CatalogResolution result{catalog, {}};
    std::map<std::string, std::string> identities;
    std::set<std::string> conflicts, loaded;
    for (const auto& selection : selections) {
        const auto identity = selection.content_sha256 + "\n" + selection.package_path + "\n" + selection.wrapper_version;
        const auto [entry, inserted] = identities.emplace(selection.package_id, identity);
        if (!inserted && entry->second != identity) conflicts.insert(selection.package_id);
    }
    for (const auto& selection : selections) {
        if (conflicts.contains(selection.package_id)) {
            result.diagnostics.push_back({"managed_version_conflict", "One project cannot resolve multiple identities for a package", selection.node_id});
            continue;
        }
        try {
            require(package_id(selection.package_id) && content_hash(selection.content_sha256) &&
                selection.wrapper_version == "python-function-v1", "Unsupported exact managed identity");
            const auto relative = std::filesystem::u8path(selection.package_path);
            require(!relative.empty() && !relative.is_absolute() && !relative.has_root_path() && relative.lexically_normal() == relative &&
                std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }), "Invalid project package path");
            if (!loaded.contains(selection.package_id)) {
#ifdef AUDIOALGO_AUTHORING_PYTHON
                auto path = root_ / "packages" / selection.package_id / selection.content_sha256;
                // A carried package is authoritative when present. Corrupt carried bytes never fall back to global defaults.
                if (!project_directory.empty()) {
                    auto carried = project_directory / relative;
                    unlinked(carried);
                    if (std::filesystem::exists(io_path(carried))) path = std::move(carried);
                }
                register_managed(*catalog, path, selection.package_id, selection.content_sha256);
                loaded.insert(selection.package_id);
#else
                throw std::runtime_error("Python execution is disabled in this build");
#endif
            }
            const auto* descriptor = catalog->find(selection.type_id);
            require(descriptor && descriptor->plugin_origin && descriptor->plugin_origin->plugin_id == selection.package_id &&
                descriptor->plugin_origin->managed_content_sha256 == selection.content_sha256, "Locked type is absent from the exact package");
        } catch (const std::exception& error) {
            result.diagnostics.push_back({"managed_version_unavailable", error.what(), selection.node_id});
        }
    }
    return result;
}
namespace { std::filesystem::path location_settings_file(); }
std::filesystem::path configured_authoring_root(const std::filesystem::path& supplied_settings) {
    const auto override = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {});
    if (override.isNotEmpty()) return std::filesystem::u8path(override.toStdString());
    const auto settings_path = supplied_settings.empty() ? location_settings_file() : supplied_settings;
    if (std::filesystem::exists(io_path(settings_path))) {
        const auto settings = juce::JSON::parse(js(read(settings_path)));
        require(settings.isObject() && settings["schema_version"] == juce::var(1), "Invalid authoring storage settings; original file preserved");
        const auto value = settings["library_directory"].toString();
        if (value.isNotEmpty()) {
            const auto selected = std::filesystem::u8path(value.toStdString());
            require(selected.is_absolute(), "Library directory must be absolute"); return selected;
        }
    }
    return std::filesystem::u8path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("AudioAlgo/authoring").getFullPathName().toStdString());
}
namespace {
std::filesystem::path location_settings_file() {
    return std::filesystem::u8path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("AudioAlgo/authoring-locations.json").getFullPathName().toStdString());
}
std::filesystem::path configured_location(bool recovery, const std::filesystem::path& supplied_settings) {
    const auto override = juce::SystemStats::getEnvironmentVariable(recovery ? "AUDIOALGO_AUTHORING_RECOVERY_ROOT" : "AUDIOALGO_AUTHORING_WORK_ROOT", {});
    if (override.isNotEmpty()) return std::filesystem::u8path(override.toStdString());
    const auto path = supplied_settings.empty() ? location_settings_file() : supplied_settings;
    if (std::filesystem::exists(io_path(path))) {
        const auto settings = juce::JSON::parse(js(read(path)));
        if (!settings.isObject() || settings["schema_version"] != juce::var(1)) throw std::runtime_error("Invalid authoring location settings: " + path.generic_string());
        const auto value = settings[recovery ? "recovery_directory" : "work_directory"].toString();
        if (value.isNotEmpty()) {
            const auto selected = std::filesystem::u8path(value.toStdString());
            if (!selected.is_absolute()) throw std::runtime_error("Authoring settings require absolute directories");
            return selected;
        }
    }
    return recovery ? configured_authoring_root(supplied_settings) / "drafts" : configured_authoring_root(supplied_settings);
}
}
std::filesystem::path configured_authoring_work_directory(const std::filesystem::path& settings) { return configured_location(false, settings); }
std::filesystem::path configured_authoring_recovery_directory(const std::filesystem::path& settings) { return configured_location(true, settings); }
namespace {
SaveResult save_location_field(const char* field, const std::filesystem::path& directory, const std::filesystem::path& selected_settings) {
    std::filesystem::path temporary;
    try {
        if (!directory.is_absolute()) return Diagnostic{"authoring_settings_failed", "Choose an absolute directory"};
        const auto path = selected_settings.empty() ? location_settings_file() : selected_settings;
        unlinked(path);
        const auto original = std::filesystem::exists(io_path(path)) ? std::optional(read(path)) : std::optional<std::string>{};
        juce::var settings;
        if (original) {
            settings = juce::JSON::parse(js(*original));
            require(settings.isObject() && settings["schema_version"] == juce::var(1), "Existing authoring settings are invalid; preserve them for recovery");
        } else { auto* object = new juce::DynamicObject; object->setProperty("schema_version", 1); settings = juce::var(object); }
        if (std::string(field) == "library_directory") {
            const auto preserve = [&](const char* key, const std::filesystem::path& value) {
                if (settings[key].toString().isEmpty()) { const auto text = value.u8string(); settings.getDynamicObject()->setProperty(key, juce::String::fromUTF8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()))); }
            };
            preserve("work_directory", configured_authoring_work_directory(path));
            preserve("recovery_directory", configured_authoring_recovery_directory(path));
        }
        const auto encoded = directory.u8string();
        settings.getDynamicObject()->setProperty(field, juce::String::fromUTF8(reinterpret_cast<const char*>(encoded.data()), static_cast<int>(encoded.size())));
        const auto bytes = juce::JSON::toString(settings, false).toStdString();
        std::filesystem::create_directories(io_path(path.parent_path()));
        temporary = path; temporary += "." + juce::Uuid().toString().toStdString() + ".tmp";
        write(io_path(temporary), bytes); require(read(temporary) == bytes, "Settings verification failed");
        const auto current = std::filesystem::exists(io_path(path)) ? std::optional(read(path)) : std::optional<std::string>{};
        require(current == original, "Settings changed concurrently; session directory remains usable");
#ifdef _WIN32
        require(MoveFileExW(io_path(temporary).c_str(), io_path(path).c_str(), (original ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) != 0, "Could not atomically save authoring settings");
#else
        std::filesystem::rename(temporary, path);
#endif
        return {};
    } catch (const std::exception& error) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(io_path(temporary), ignored); }
        return Diagnostic{"authoring_settings_failed", error.what()};
    }
}
}
SaveResult save_authoring_location(bool recovery, const std::filesystem::path& directory, const std::filesystem::path& settings) {
    return save_location_field(recovery ? "recovery_directory" : "work_directory", directory, settings);
}
SaveResult save_authoring_library_location(const std::filesystem::path& directory, const std::filesystem::path& settings) {
    return save_location_field("library_directory", directory, settings);
}
std::optional<SourceDiagnostic> PythonAuthoring::copy_library_to(const std::filesystem::path& selected) const {
#ifndef AUDIOALGO_AUTHORING_PYTHON
    return SourceDiagnostic{"python_disabled", "Managed Python library copying is unavailable in this build"};
#else
    std::lock_guard lock(*apply_mutex_);
    std::filesystem::path stage;
    try {
        require(selected.is_absolute(), "Choose an absolute library directory");
        const auto destination = std::filesystem::absolute(selected).lexically_normal();
        const auto source = std::filesystem::absolute(root_).lexically_normal();
        require(destination != source, "Choose a different library directory");
        auto inside = destination.lexically_relative(source);
        require(inside.empty() || *inside.begin() == "..", "The new library cannot be inside the old library");
        unlinked(source); unlinked(destination);
        require(!std::filesystem::exists(io_path(destination)) || std::filesystem::is_empty(io_path(destination)), "Choose an empty folder; existing files will not be overwritten");
        if (auto error = probe_authoring_directory(destination)) return SourceDiagnostic{error->code, error->message};
        const auto original_index = std::filesystem::exists(io_path(source / "index.json")) ? std::optional(read(source / "index.json")) : std::optional<std::string>{};
        const auto index = index_at(source);
        stage = destination.parent_path() / (".audioalgo-library-" + juce::Uuid().toString().toStdString());
        std::filesystem::create_directories(io_path(stage));
        if (std::filesystem::exists(io_path(source / "packages"))) {
            for (const auto& family : std::filesystem::directory_iterator(io_path(source / "packages"))) {
                const auto id = family.path().filename().string(); require(package_id(id), "Invalid installed package identity"); unlinked(family.path());
                for (const auto& version : std::filesystem::directory_iterator(family.path())) {
                    const auto content = version.path().filename().string(); require(content_hash(content), "Invalid installed version identity"); unlinked(version.path());
                    if (auto error = validate_python_snapshot(version.path(), {})) throw std::runtime_error(error->code + ": " + error->message);
                    const auto frozen = freeze_python_package(version.path(), stage / "packages" / id, "", {});
                    require(std::holds_alternative<std::filesystem::path>(frozen) && std::get<std::filesystem::path>(frozen).filename().string() == content, "Copied library version hash mismatch");
                }
            }
        }
        if (original_index) write(io_path(stage / "index.json"), *original_index);
        (void)indexed_catalog(NodeRegistry{}, stage, index);
        const auto current_index = std::filesystem::exists(io_path(source / "index.json")) ? std::optional(read(source / "index.json")) : std::optional<std::string>{};
        require(current_index == original_index, "Library changed during copying; original location remains active");
        require(std::filesystem::is_empty(io_path(destination)), "Destination changed during copying");
        std::filesystem::remove(io_path(destination)); // selected empty directory only
        std::filesystem::rename(io_path(stage), io_path(destination)); stage.clear();
        return {};
    } catch (const std::exception& error) {
        if (!stage.empty()) {
            std::error_code ignored;
            // Only a private sibling generated by this invocation is removed.
            if (stage.parent_path() == selected.lexically_normal().parent_path() && stage.filename().string().starts_with(".audioalgo-library-")) std::filesystem::remove_all(io_path(stage), ignored);
        }
        return SourceDiagnostic{"authoring_library_copy_failed", error.what()};
    }
#endif
}
std::string authoring_launch_diagnostic() {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "Could not inspect process write permissions.";
    DWORD length = 0; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &length);
    std::vector<unsigned char> buffer(length);
    const bool ok = GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), length, &length) != 0;
    CloseHandle(token);
    if (ok) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
        if (count && *GetSidSubAuthority(label->Label.Sid, count - 1) < SECURITY_MANDATORY_MEDIUM_RID)
            return "This editor is running with Low Integrity. Normal user folders may reject writes. Save or copy your draft before closing. Start a verified deployment from a normal user runtime directory; Debug/Release does not change this permission.";
    }
#endif
    return {};
}
std::optional<SourceDiagnostic> PythonAuthoring::carry_packages(const std::vector<PackageSelection>& selections,
    const std::filesystem::path& source_directory, const std::filesystem::path& destination_directory) const {
    if (selections.empty()) return {};
    const auto resolved = resolve_catalog(std::make_shared<const NodeRegistry>(), selections, source_directory);
    if (!resolved.diagnostics.empty()) return SourceDiagnostic{"authoring_carry_failed", resolved.diagnostics.front().message};
    std::lock_guard lock(*apply_mutex_);
    std::filesystem::path temporary;
    try {
        std::set<std::string> copied;
        for (const auto& selection : selections) {
            if (!copied.insert(selection.package_id).second) continue;
#ifdef AUDIOALGO_AUTHORING_PYTHON
            const auto relative = std::filesystem::u8path(selection.package_path);
            auto source = root_ / "packages" / selection.package_id / selection.content_sha256;
            if (!source_directory.empty() && std::filesystem::exists(io_path(source_directory / relative))) source = source_directory / relative;
            const auto destination = destination_directory / relative;
            unlinked(destination);
            if (!std::filesystem::exists(io_path(destination))) {
                std::filesystem::create_directories(io_path(destination.parent_path()));
                temporary = destination.parent_path() / (".carry-" + juce::Uuid().toString().toStdString());
                auto captured = freeze_python_package(source, temporary, "", [] { return false; });
                require(std::holds_alternative<std::filesystem::path>(captured) &&
                    std::get<std::filesystem::path>(captured).filename().string() == selection.content_sha256, "Exact package capture failed");
                // Publish a whole immutable directory, never replace an existing destination.
                std::filesystem::rename(io_path(std::get<std::filesystem::path>(captured)), io_path(destination));
                std::filesystem::remove_all(io_path(temporary)); temporary.clear();
            }
            NodeRegistry verified;
            register_managed(verified, destination, selection.package_id, selection.content_sha256);
#else
            throw std::runtime_error("Python execution is disabled in this build");
#endif
        }
        return {};
    } catch (const std::exception& error) {
        if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove_all(io_path(temporary), ignored); }
        return SourceDiagnostic{"authoring_carry_failed", error.what()};
    }
}
CheckOutcome PythonAuthoring::check(Draft draft, const std::shared_ptr<std::atomic<bool>>& cancel) const {
#ifndef AUDIOALGO_AUTHORING_PYTHON
    return SourceDiagnostic{"python_disabled", "Python execution is disabled in this build", "", 0, 0};
#else
    try {
        if (cancel && cancel->load()) return SourceDiagnostic{"run_cancelled", "Check cancelled", "", 0, 0};
        if (draft.language != "python") return SourceDiagnostic{"authoring_language_unsupported", "This checker accepts Python function drafts", "", 0, 0};
        if (draft.source.size() + draft.declaration.size() > 8 * 1024 * 1024 - 16384)
            return SourceDiagnostic{"python_code_limit", "Generated package would exceed the 8 MiB source budget", "user_source.py", 0, 0};
        auto node = juce::JSON::parse(js(draft.declaration));
        if (!node.isObject()) return SourceDiagnostic{"authoring_definition_invalid", "Definition must be an SDK node JSON object", "definition.json", 0, 0};
        auto* object = node.getDynamicObject();
        if (node["execution"]["kind"].toString() != "block")
            return SourceDiagnostic{"authoring_capability_unsupported", "Function templates currently require block PCM execution", "definition.json", 0, 0};
        object->setProperty("entrypoint", "authoring_wrapper:Node");
        const auto request_id = juce::Uuid().toString().toStdString();
        if (auto error = probe_authoring_directory(work_)) return SourceDiagnostic{error->code, error->message + " | Choose another Work folder. Current: " + work_.generic_string()};
        const auto staging = work_ / "staging" / request_id;
        std::filesystem::create_directories(io_path(staging));
        const auto package_id = "user.authoring." + hash(draft.id).substr(0, 24);
        auto manifest = [&](const std::filesystem::path& path, const char* entrypoint) {
            std::filesystem::create_directories(path);
            auto descriptor = node.clone(); descriptor.getDynamicObject()->setProperty("entrypoint", entrypoint);
            juce::DynamicObject::Ptr root = new juce::DynamicObject;
            root->setProperty("schema_version", 1); root->setProperty("runtime", "python");
            root->setProperty("plugin_id", js(package_id)); root->setProperty("plugin_version", node["version"]);
            root->setProperty("nodes", juce::Array<juce::var>{descriptor});
            write(path / "manifest.json", juce::JSON::toString(juce::var(root.get()), false).toStdString());
            write(path / "user_source.py", draft.source); write(path / "authoring_wrapper.py", wrapper); write(path / "syntax_check.py", syntax_wrapper);
        };
        manifest(staging / "candidate", "authoring_wrapper:Node");
        NodeRegistry catalog;
        const auto discovery = load_native_plugins(catalog, {staging / "candidate"});
        if (!discovery.diagnostics.empty()) return SourceDiagnostic{discovery.diagnostics[0].code, discovery.diagnostics[0].message, "definition.json", 0, 0};
        const auto* descriptor = catalog.find(node["type_id"].toString().toStdString());
        if (!descriptor) return SourceDiagnostic{"authoring_definition_invalid", "Node was not registered", "definition.json", 0, 0};
        const auto cancelled = [cancel] { return cancel && cancel->load(); };
        const auto interpreter_hash = file_hash(interpreter_);
        auto frozen = freeze_python_package(staging / "candidate", staging / "frozen", "", cancelled);
        if (auto* error = std::get_if<Diagnostic>(&frozen)) return SourceDiagnostic{error->code, error->message, "user_source.py", 0, 0};
        const auto frozen_package = std::get<std::filesystem::path>(frozen);
        auto frozen_runner = freeze_python_package(runner_, staging / "runner", "", cancelled);
        if (auto* error = std::get_if<Diagnostic>(&frozen_runner)) return SourceDiagnostic{error->code, error->message, "", 0, 0};
        PythonTaskRequest request;
        request.python_executable = interpreter_; request.runner_module_root = std::get<std::filesystem::path>(frozen_runner);
        request.type_id = descriptor->type_id; request.node_id = "authoring-probe"; request.output_parent = staging;
        request.identity = {draft.id, draft.generation, request_id, draft.generation};
        request.maximum_block_frames = 127; request.execution_timeout = std::chrono::seconds(30);
        request.is_cancelled = cancelled;
        const auto rate = descriptor->audio_format.sample_rates_hz.front();
        const auto channels = descriptor->audio_format.channel_counts.front();
        std::vector<std::string> ids; for (std::size_t c = 0; c < channels; ++c) ids.push_back("authoring-channel-" + std::to_string(c));
        std::size_t index = 0;
        for (const auto& port : descriptor->ports) {
            if (port.direction == PortDirection::output) { request.output_ports.push_back(port.id); continue; }
            AudioBuffer buffer(rate, ids, 513, {0, 513});
            for (std::size_t c = 0; c < channels; ++c)
                for (std::size_t n = 0; n < 513; ++n) buffer.mutable_channel(c)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1) + c + index));
            request.inputs.emplace(port.id, std::move(buffer)); ++index;
        }
        for (const auto& p : descriptor->parameters) request.parameters.emplace(p.id, p.default_value);
        manifest(staging / "syntax", "syntax_check:Node");
        request.plugin_root = staging / "syntax";
        PythonTaskStatistics syntax_stats, probe_stats;
        std::vector<CheckLogEvent> logs;
        std::uint64_t log_sequence = 0;
        std::string log_phase;
        auto add_log = [&](const std::string& stream, const std::string& message, std::uint64_t total, std::uint64_t discarded = 0) {
            if (message.empty() && total == 0) return;
            NodeLogEvent event; event.identity = request.identity; event.node_id = request.node_id;
            event.type_id = request.type_id; event.version = descriptor->version;
            event.sequence = ++log_sequence; event.received_unix_ms = static_cast<std::uint64_t>(juce::Time::currentTimeMillis());
            event.source = log_phase + "." + stream; event.level = stream == "stdout" ? "info" : "error";
            event.text = message; event.total_bytes = total; event.discarded_bytes = discarded;
            logs.push_back({draft.id, hash(draft.source), hash(draft.declaration), request_id, log_phase, draft.generation, std::move(event)});
        };
        auto collect_logs = [&](const PythonTaskStatistics& stats, std::string phase) {
            log_phase = std::move(phase);
            add_log("stdout", stats.stdout_tail, stats.stdout_bytes, stats.stdout_discarded_bytes); add_log("stderr", stats.stderr_tail, stats.stderr_bytes, stats.stderr_discarded_bytes);
        };
        auto syntax = run_python_task(request, &syntax_stats);
        collect_logs(syntax_stats, "syntax");
        auto diagnostic = [&](const Diagnostic& d) {
            SourceDiagnostic result{d.code, d.message, "user_source.py", 0, 0};
            const std::regex location(R"(user_source\.py:([0-9]+):([0-9]+):)"); std::smatch match;
            if (std::regex_search(d.message, match, location)) { result.line = std::stoi(match[1]); result.column = std::stoi(match[2]); }
            const auto message = d.code + ": " + d.message;
            add_log("diagnostic", message, message.size()); result.logs = logs;
            return result;
        };
        if (auto* error = std::get_if<Diagnostic>(&syntax)) return diagnostic(*error);
        request.plugin_root = frozen_package;
        auto probe = run_python_task(request, &probe_stats);
        collect_logs(probe_stats, "contract");
        if (auto* error = std::get_if<Diagnostic>(&probe)) return diagnostic(*error);
        if (auto error = validate_python_snapshot(frozen_package, cancelled)) return diagnostic(*error);
        if (auto error = validate_python_snapshot(request.runner_module_root, cancelled)) return diagnostic(*error);
        if (file_hash(interpreter_) != interpreter_hash) return SourceDiagnostic{"python_environment_changed", "Interpreter changed during the check", "", 0, 0};
        auto success = std::get<PythonTaskSuccess>(std::move(probe));
        ReadyPackage ready;
        ready.draft = std::move(draft); ready.request_id = request_id;
        ready.source_sha256 = hash(ready.draft.source); ready.declaration_sha256 = hash(ready.draft.declaration);
        ready.package_root = std::get<std::filesystem::path>(std::move(frozen)); ready.package_sha256 = ready.package_root.filename().string();
        ready.interpreter_sha256 = interpreter_hash;
        ready.runner_sha256 = request.runner_module_root.filename().string();
        ready.descriptor = *descriptor; ready.probe_directory = success.artifact_directory;
        ready.probe_outputs = std::move(success.outputs); ready.syntax_pid = syntax_stats.process_id; ready.probe_pid = probe_stats.process_id;
        ready.logs = std::move(logs);
        return ready;
    } catch (const std::exception& e) { return SourceDiagnostic{"authoring_check_failed", e.what(), "", 0, 0}; }
#endif
}
}
