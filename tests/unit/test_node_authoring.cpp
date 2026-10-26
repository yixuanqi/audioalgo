#include "EditorNodeLibrary.h"
#include "EditorWorkspace.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "audio_native_plugins/NativePlugins.h"
#include "tests/fixtures/AllocationCounter.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <set>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
template<class T> T* control(juce::Component& parent, const juce::String& id) {
    if (parent.getComponentID() == id) return dynamic_cast<T*>(&parent);
    for (auto* child : parent.getChildren())
        if (auto* found = control<T>(*child, id)) return found;
    return nullptr;
}

class Documents final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path&) const override { return audioalgo::Diagnostic{"unused", ""}; }
    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path&, const audioalgo::ProjectModel&) const override { return std::monostate{}; }
};
class FileDocuments final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path& path) const override { return audioalgo::load_project_file(path); }
    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path& path, const audioalgo::ProjectModel& project) const override { return audioalgo::save_project_file(path, project); }
};
class Preview final : public audioalgo::AudioPreviewService {
public:
    std::filesystem::path played;
    audioalgo::AudioPreviewOutcome play(const std::filesystem::path& path) override { played=path;return std::monostate{}; }
    void pause() override {} void stop() override {} void shutdown() override {}
    bool is_playing() const noexcept override { return false; }
    double position_seconds() const noexcept override { return 0; }
};
template<class Predicate> bool pump(Predicate ready, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!ready() && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    return ready();
}
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
void click(juce::Component& root, const char* id) {
    auto* b = control<juce::TextButton>(root, id); REQUIRE(b); REQUIRE(b->isEnabled());
    struct ClickObserved final : juce::Button::Listener {
        bool received = false;
        void buttonClicked(juce::Button*) override { received = true; }
    } observed;
    juce::Component::SafePointer<juce::TextButton> safe(b);
    b->addListener(&observed); b->triggerClick();
    const bool received = pump([&] { return observed.received; });
    if (safe != nullptr) safe->removeListener(&observed);
    REQUIRE(received);
}
class ControlledChecks final : public audioalgo::authoring::AuthoringChecks {
public:
    mutable std::atomic<bool> entered{false}, release{false};
    audioalgo::authoring::CheckOutcome check(audioalgo::authoring::Draft draft, const std::shared_ptr<std::atomic<bool>>&) const override {
        entered = true;
        while (!release.load()) std::this_thread::yield();
        audioalgo::authoring::ReadyPackage result; result.draft = std::move(draft); return result;
    }
};
struct ReleaseCheck {
    std::shared_ptr<ControlledChecks> checks;
    ~ReleaseCheck() { checks->release = true; }
};
template<class Predicate> bool without_ui(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return ready();
}
// Only inspect the managed request/task or request/log levels. Recursive
// traversal enters long frozen-code paths and protected python-result folders.
// skip_permission_denied does not suppress path-length/status-query errors.
std::vector<std::filesystem::path> check_files(const std::filesystem::path& staging,
                                              const char* directory_prefix, const char* filename) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator requests(staging, ec), end; !ec && requests != end; requests.increment(ec)) {
        std::error_code request_ec;
        for (std::filesystem::directory_iterator entries(requests->path(), request_ec), entries_end;
             !request_ec && entries != entries_end; entries.increment(request_ec)) {
            if (entries->path().filename().string().starts_with(directory_prefix))
                files.push_back(entries->path() / filename);
        }
    }
    return files;
}
bool check_process_barrier(const std::filesystem::path& staging, std::filesystem::path& marker,
                           unsigned long& parent_pid, unsigned long& child_pid) {
    for (const auto& path : check_files(staging, "python-task-", "pc03-check-processes.txt")) {
        unsigned long parent = 0, child = 0;
        std::ifstream in(path);
        if (in >> parent >> child && parent != 0 && child != 0) {
            marker = path; parent_pid = parent; child_pid = child; return true;
        }
    }
    return false;
}
void wire_workspace(audioalgo::editor::EditorComponent& workspace, const audioalgo::NodeRegistry& registry,
                    const std::string& from_node, const std::string& from_port,
                    const std::string& to_node, const std::string& to_port) {
    auto* canvas = control<audioalgo::editor::GraphCanvas>(workspace, "project.canvas"); REQUIRE(canvas);
    auto point = [&](const std::string& node, const std::string& port) {
        for (const auto& instance : workspace.controller().project().nodes) if (instance.id == node)
            for (const auto& p : audioalgo::editor::make_node_geometry(instance, registry.find(instance.type_id), canvas->coordinate_transform()).ports)
                if (p.id == port) return juce::Point<float>(p.anchor.x, p.anchor.y);
        throw std::runtime_error("Missing real canvas port");
    };
    const auto from = point(from_node, from_port), to = point(to_node, to_port);
    auto mouse = [&](juce::Point<float> position, int buttons, bool dragged) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(buttons),
            0, 0, 0, 0, 0, canvas, canvas, juce::Time(100), from, juce::Time(0), 1, dragged);
    };
    canvas->mouseDown(mouse(from, juce::ModifierKeys::leftButtonModifier, false));
    canvas->mouseDrag(mouse(to, juce::ModifierKeys::leftButtonModifier, true));
    canvas->mouseUp(mouse(to, 0, true));
}
}

TEST_CASE("NA-01.a production draft offers a user chosen Save As target",
          "[NA-01.a][draft_files][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(
        std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/authoring-test-tmp/save-as");
    audioalgo::editor::NodeAuthoringPanel panel(audioalgo::authoring::python_draft("save-as"), false, storage, [] {});
    REQUIRE(control<juce::TextButton>(panel, "authoring.save_as"));
}

TEST_CASE("NA-01.a user file save cancel Save As and reopen use the production workspace",
          "[NA-01.a][NA-14.c][draft_files][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0024-draft-files/artifacts" / juce::Uuid().toString().toStdString();
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::DraftFileActions::Completion selection; int selections = 0; bool saving = false;
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(), std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false, {}, {}, true,
        [&](bool save, const std::filesystem::path&, auto completion) { saving = save; ++selections; selection = std::move(completion); });
    const auto revision = workspace.controller().project().revision;
    click(workspace, "authoring.new");
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
    const auto draft = panel->session().document();
    click(workspace, "authoring.save"); REQUIRE(selection); CHECK(saving); CHECK(selections == 1);
    selection({}); CHECK(panel->file_path().empty()); CHECK(panel->session().dirty());
    REQUIRE(pump([&] { return panel->session().recovery_current(); }));
    CHECK(panel->session().dirty()); CHECK(selections == 1);
    click(workspace, "authoring.save");
    const auto first = root / std::filesystem::u8path("用户选择/初稿.aadraft"); selection(first);
    REQUIRE(pump([&] { return !panel->session().dirty(); })); CHECK(panel->file_path() == first);
    REQUIRE(pump([&] {
        auto recovery = storage->load_recovery(draft.id);
        return std::holds_alternative<DraftRecovery>(recovery) && std::get<DraftRecovery>(recovery).original_file == first;
    }));
    CHECK(std::get<DraftFile>(DraftFiles{}.load(first)).document == draft);
    click(workspace, "authoring.save_as"); selection({}); CHECK(panel->file_path() == first);
    const auto second = root / "another-folder" / "renamed.aadraft";
    click(workspace, "authoring.save_as"); selection(second);
    REQUIRE(pump([&] { return !panel->file_save_pending(); })); CHECK(panel->file_path() == second);
    CHECK(std::get<DraftFile>(DraftFiles{}.load(first)).bytes == std::get<DraftFile>(DraftFiles{}.load(second)).bytes);
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, "authoring.open"); CHECK_FALSE(saving); selection(second);
    REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") != nullptr; }));
    panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    CHECK(panel->file_path() == second); CHECK(panel->session().document() == draft);
    CHECK(workspace.controller().project().revision == revision); CHECK_FALSE(workspace.controller().dirty());
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    int drained = 0; workspace.begin_shutdown([&] { ++drained; }); REQUIRE(pump([&] { return drained == 1; }));
}

TEST_CASE("NA-01.b explicit file failure preserves binding and recovery does not clear unsaved text",
          "[NA-01.b][draft_files][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0024-draft-files/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::DraftFileActions files;
    audioalgo::editor::DraftFileActions::Completion choose;
    std::atomic<bool> allowed{false};
    files.choose_save = [&](const auto&, auto completion) { choose = std::move(completion); };
    files.documents = std::make_shared<DraftFiles>([&](SavePhase) { return allowed.load(); });
    int closed = 0;
    audioalgo::editor::NodeAuthoringPanel panel(python_draft("file-failure"), false, storage, [&] { ++closed; }, {}, {}, {}, {}, {}, files);
    click(panel, "authoring.close"); REQUIRE(choose); choose(root / "chosen.aadraft");
    REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(closed == 0); CHECK(panel.file_path().empty()); CHECK(panel.session().dirty());
    REQUIRE(pump([&] { return panel.session().recovery_current(); })); CHECK(panel.session().dirty());
    allowed = true; click(panel, "authoring.save"); choose(root / "chosen.aadraft");
    REQUIRE(pump([&] { return !panel.session().dirty(); }));
    CHECK(std::get<DraftFile>(DraftFiles{}.load(root / "chosen.aadraft")).document == panel.session().document());
}

TEST_CASE("NA-04.d chosen work directory checks and applies without moving installed versions",
          "[NA-04.d][NA-07.d][authoring_locations][component]") {
    using namespace audioalgo::authoring;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0025-authoring-locations/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root / "library");
    { std::ofstream blocked(root / "library/staging"); blocked << "This is a file, so staging here cannot work"; }
    PythonAuthoring service(root / "library");
    const auto relocated = service.with_work_directory(root / std::filesystem::u8path("用户 工作目录"));
    REQUIRE(relocated);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto draft = python_draft("chosen-work"); const auto checked = relocated->check(draft, cancel);
#if !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<SourceDiagnostic>(checked)); CHECK(std::get<SourceDiagnostic>(checked).code == "python_disabled"); return;
#endif
    REQUIRE(std::holds_alternative<ReadyPackage>(checked));
    const auto ready = std::get<ReadyPackage>(checked);
    CHECK(ready.package_root.generic_string().find("staging") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(root / "library/staging"));
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto applied = relocated->apply(ready, draft, registry, cancel);
    REQUIRE(std::holds_alternative<AppliedPackage>(applied));
    CHECK(std::get<AppliedPackage>(applied).package_root.parent_path().parent_path() == root / "library/packages");
    CHECK(std::filesystem::is_regular_file(root / "library/index.json"));
}

TEST_CASE("NA-14.c changing recovery and work folders preserves text when settings cannot persist",
          "[NA-14.c][NA-04.d][authoring_locations][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0025-authoring-locations/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    { std::ofstream blocked(root / "blocked"); blocked << "not a directory"; }
    auto storage = std::make_shared<FileDraftStorage>(root / "blocked/recovery");
    auto checks = std::make_shared<PythonAuthoring>(root / "library");
    audioalgo::editor::DraftFileActions files;
    files.choose_save = [](const auto&, auto completion) { completion({}); };
    audioalgo::editor::DraftFileActions::Completion selection; bool recovery_selected = false;
    files.choose_directory = [&](bool recovery, auto completion) { recovery_selected = recovery; selection = std::move(completion); };
    files.persist_directory = [&](bool recovery, const auto& path) { return save_authoring_location(recovery, path, root / "blocked/settings.json"); };
    auto draft = python_draft("location-ui");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks, {}, {}, {}, {}, files);
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.status")->getText().contains("Recovery unavailable"); }));
    click(panel, "authoring.recovery_folder"); REQUIRE(selection); CHECK(recovery_selected);
    selection(root / "chosen-recovery");
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.locations")->getText().contains("Session only"); }));
    REQUIRE(pump([&] { return panel.session().recovery_current(); }));
    CHECK(panel.session().dirty()); CHECK(panel.session().document() == draft);
    CHECK(std::get<Draft>(FileDraftStorage(root / "chosen-recovery").load(draft.id)) == draft);
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.work_folder")->isEnabled(); }));
    click(panel, "authoring.work_folder"); CHECK_FALSE(recovery_selected); selection(root / "chosen-work");
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.locations")->getText().contains("chosen-work"); }));
    CHECK(control<juce::Label>(panel, "authoring.locations")->getText().contains("Session only"));
    click(panel, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return panel.ready().has_value(); }));
    CHECK(panel.ready()->package_root.generic_string().find("chosen-work") != std::string::npos);
#else
    REQUIRE(pump([&] { return !panel.check_in_progress(); }));
    CHECK(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"));
#endif
    CHECK(panel.session().document() == draft); CHECK(panel.session().dirty());
    CHECK_FALSE(std::filesystem::exists(root / "library/index.json"));
}

TEST_CASE("NA-01.b stale user file save and external modification preserve newer edits",
          "[NA-01.b][NA-02.d][draft_files][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0024-draft-files/artifacts" / juce::Uuid().toString().toStdString();
    std::atomic<bool> entered{false}, release{false};
    audioalgo::editor::DraftFileActions files;
    const auto path = root / "my-node.aadraft";
    files.choose_save = [&](const auto&, auto completion) { completion(path); };
    files.documents = std::make_shared<DraftFiles>([&](SavePhase phase) {
        if (phase == SavePhase::write) { entered = true; while (!release.load()) std::this_thread::yield(); }
        return true;
    });
    auto original = python_draft("stale-file");
    audioalgo::editor::NodeAuthoringPanel panel(original, false, std::make_shared<FileDraftStorage>(root / "recovery"), [] {}, {}, {}, {}, {}, {}, files);
    struct Release { std::atomic<bool>& flag; ~Release() { flag = true; } } guard{release};
    // Target inspection needs UI delivery before the held write starts. Keep
    // the original two-second entry budget while dispatching that delivery.
    click(panel, "authoring.save"); REQUIRE(pump([&] { return entered.load(); }, 2000));
    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    source->selectAll(); source->insertTextAtCaret("# newer edit\ndef incomplete(");
    release = true; REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(panel.session().dirty()); CHECK(std::get<DraftFile>(DraftFiles{}.load(path)).document == original);
    auto prior = std::get<DraftFile>(DraftFiles{}.load(path));
    auto external = original; external.source = "# changed externally"; ++external.generation;
    REQUIRE(std::holds_alternative<DraftFile>(DraftFiles{}.save(path, external, prior.bytes)));
    click(panel, "authoring.save"); REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(panel.session().dirty()); CHECK(panel.session().document().source == "# newer edit\ndef incomplete(");
    CHECK(std::get<DraftFile>(DraftFiles{}.load(path)).document == external);
    CHECK(control<juce::Label>(panel, "authoring.status")->getText().contains("draft_file_conflict"));
}

TEST_CASE("NA-07.d library copy preserves both immutable versions and refuses a nonempty destination",
          "[NA-07.d][NA-09.a][authoring_locations][component]") {
    using namespace audioalgo::authoring;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0027-library-copy/artifacts" / juce::Uuid().toString().toStdString();
    PythonAuthoring service(root / "library");
#if !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto disabled = service.copy_library_to(root / "chosen-library"); REQUIRE(disabled); CHECK(disabled->code == "python_disabled"); return;
#endif
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    const auto cancel = std::make_shared<std::atomic<bool>>(false); auto draft = python_draft("migrated-library");
    const auto v1 = service.check(draft, cancel); REQUIRE(std::holds_alternative<ReadyPackage>(v1));
    const auto applied1 = service.apply(std::get<ReadyPackage>(v1), draft, registry, cancel); REQUIRE(std::holds_alternative<AppliedPackage>(applied1));
    draft.source += "\n# second immutable version\n"; ++draft.generation;
    const auto v2 = service.check(draft, cancel); REQUIRE(std::holds_alternative<ReadyPackage>(v2));
    const auto applied2 = service.apply(std::get<ReadyPackage>(v2), draft, std::get<AppliedPackage>(applied1).catalog, cancel); REQUIRE(std::holds_alternative<AppliedPackage>(applied2));
    const auto destination = root / "chosen-library";
    const auto copy_error = service.copy_library_to(destination);
    INFO((copy_error ? copy_error->code + ": " + copy_error->message : "Library copy succeeded"));
    REQUIRE_FALSE(copy_error);
    const auto first = std::get<AppliedPackage>(applied1), second = std::get<AppliedPackage>(applied2);
    CHECK(std::filesystem::exists(destination / "packages" / first.package_id / first.content_sha256 / "snapshot.json"));
    CHECK(std::filesystem::exists(destination / "packages" / second.package_id / second.content_sha256 / "snapshot.json"));
    CHECK(std::filesystem::exists(first.package_root / "user_source.py"));
    PythonAuthoring migrated(destination);
    auto restored = migrated.restore_defaults(*registry); REQUIRE(std::holds_alternative<audioalgo::NodeRegistry>(restored));
    const auto* descriptor = std::get<audioalgo::NodeRegistry>(restored).find("user.custom_node"); REQUIRE(descriptor);
    CHECK(descriptor->plugin_origin->managed_content_sha256 == second.content_sha256);
    CHECK(service.copy_library_to(destination).has_value()); // explicit refusal, no overwrite
    CHECK(std::filesystem::exists(first.package_root / "user_source.py"));
}

TEST_CASE("NA-14.c library folder selection is verified for next startup without changing the live catalog",
          "[NA-14.c][NA-07.d][authoring_locations][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0027-library-copy/artifacts" / juce::Uuid().toString().toStdString();
    const auto destination = root / "next-library", settings = root / "preferences.json";
    auto checks = std::make_shared<PythonAuthoring>(root / "current-library");
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::DraftFileActions files;
    files.choose_save = [](const auto&, auto done) { done({}); };
    files.choose_directory = [](bool, auto done) { done({}); };
    files.choose_library_directory = [&](auto done) { done(destination); };
    files.persist_library_directory = [&](const auto& path) { return save_authoring_library_location(path, settings); };
    bool library_change_pending = false;
    files.allow_apply = [&] { return !library_change_pending; };
    files.library_change_queued = [&] { library_change_pending = true; };
    const auto draft = python_draft("next-startup");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks, {}, {}, {}, {}, files);
    REQUIRE(pump([&] { return panel.session().recovery_current(); }));
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.library_folder")->isEnabled(); }));
    click(panel, "authoring.library_folder");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.locations")->getText().contains("next startup"); }));
    CHECK(configured_authoring_root(settings) == destination);
    CHECK(library_change_pending);
#else
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.locations")->getText().contains("python_disabled"); }));
    CHECK_FALSE(std::filesystem::exists(settings));
#endif
    CHECK(checks->library_directory() == root / "current-library");
    CHECK(panel.session().document() == draft); CHECK(panel.session().dirty()); CHECK_FALSE(panel.ready());
}

TEST_CASE("NA-14.c location settings round trip and preserve corrupt preferences", "[NA-14.c][authoring_locations][component]") {
    using namespace audioalgo::authoring;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0025-authoring-locations/artifacts" / juce::Uuid().toString().toStdString();
    const auto settings = root / "preferences.json";
    REQUIRE_FALSE(save_authoring_location(false, root / "work", settings));
    REQUIRE_FALSE(save_authoring_location(true, root / "recovery", settings));
    REQUIRE_FALSE(save_authoring_library_location(root / "library", settings));
    CHECK(configured_authoring_work_directory(settings) == root / "work");
    CHECK(configured_authoring_recovery_directory(settings) == root / "recovery");
    CHECK(configured_authoring_root(settings) == root / "library");
    const std::string damaged = "{this is not valid preferences";
    { std::ofstream file(settings); file << damaged; }
    REQUIRE(save_authoring_location(false, root / "new-work", settings));
    CHECK_THROWS(configured_authoring_work_directory(settings));
    std::ifstream stream(settings); const std::string actual(std::istreambuf_iterator<char>(stream), {});
    CHECK(actual == damaged);
}

TEST_CASE("NA-01.b Save As confirmation is revocable and cannot overwrite an externally changed target",
          "[NA-01.b][NA-02.d][draft_files][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0029-overwrite-confirmation/artifacts" / juce::Uuid().toString().toStdString();
    const auto target = root / "existing.aadraft";
    auto existing = python_draft("existing-other-document");
    const auto stored = DraftFiles{}.save(target, existing, {}); REQUIRE(std::holds_alternative<DraftFile>(stored));
    const auto baseline = std::get<DraftFile>(stored).bytes;
    audioalgo::editor::DraftFileActions files;
    files.choose_save = [&](const auto&, auto done) { done(target); };
    std::function<void(bool)> confirm;
    files.confirm_overwrite = [&](const auto& selected, auto done) { CHECK(selected == target); confirm = std::move(done); };
    auto draft = python_draft("document-being-edited");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, std::make_shared<FileDraftStorage>(root / "recovery"), [] {}, {}, {}, {}, {}, {}, files);
    click(panel, "authoring.save_as");
    REQUIRE(pump([&] { return static_cast<bool>(confirm) || !panel.file_save_pending(); }));
    CHECK(static_cast<bool>(confirm));
    CHECK(std::get<DraftFile>(DraftFiles{}.load(target)).bytes == baseline);
    if (!confirm) return;
    confirm(false); CHECK(panel.file_path().empty()); CHECK(panel.session().dirty());
    confirm = {}; click(panel, "authoring.save_as"); REQUIRE(pump([&] { return static_cast<bool>(confirm); }));
    existing.source = "# externally changed while the confirmation is open"; ++existing.generation;
    REQUIRE(std::holds_alternative<DraftFile>(DraftFiles{}.save(target, existing, baseline)));
    confirm(true); REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(panel.file_path().empty()); CHECK(panel.session().document() == draft); CHECK(panel.session().dirty());
    CHECK(std::get<DraftFile>(DraftFiles{}.load(target)).document == existing);
    CHECK(control<juce::Label>(panel, "authoring.status")->getText().contains("draft_file_conflict"));
    confirm = {}; click(panel, "authoring.save_as"); REQUIRE(pump([&] { return static_cast<bool>(confirm); }));
    confirm(true); REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(panel.file_path() == target); CHECK_FALSE(panel.session().dirty());
    CHECK(std::get<DraftFile>(DraftFiles{}.load(target)).document == draft);
}

TEST_CASE("NA-01.c unnamed user draft can discard normally without choosing a save target",
          "[NA-01.c][draft_files][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0030-draft-close/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::DraftFileActions files;
    int selections = 0, closed = 0;
    files.choose_save = [&](const auto&, auto done) { ++selections; done({}); };
    const auto draft = python_draft("discard-unnamed");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [&] { ++closed; }, {}, {}, {}, {}, {}, files);
    auto* discard = control<juce::TextButton>(panel, "authoring.discard"); REQUIRE(discard); REQUIRE(discard->isVisible());
    click(panel, "authoring.discard"); REQUIRE(pump([&] { return closed == 1; }));
    CHECK(selections == 0); CHECK(panel.session().document() == draft); CHECK(panel.file_path().empty());
}

TEST_CASE("NA-14.b cancelled completed check cannot deliver Ready to the production panel",
          "[NA-14.b][NA-04.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    auto checks = std::make_shared<ControlledChecks>();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/authoring-test-tmp/cancel-race");
    audioalgo::editor::NodeAuthoringPanel panel(audioalgo::authoring::python_draft("cancel-race"), true, storage, [] {}, checks);
    ReleaseCheck release{checks};
    control<juce::TextButton>(panel, "authoring.check")->onClick();
    REQUIRE(without_ui([&] { return checks->entered.load(); })); checks->release = true;
    REQUIRE(without_ui([&] { return panel.check_result_pending(); }));
    // The result exists, but the message thread has never received it.
    control<juce::TextButton>(panel, "authoring.cancel")->onClick();
    REQUIRE(pump([&] { return !panel.check_in_progress(); }));
    CHECK_FALSE(panel.ready());
    CHECK(control<juce::Label>(panel, "authoring.check_status")->getText().contains("cancelled"));
    click(panel, "authoring.check"); REQUIRE(pump([&] { return panel.ready().has_value(); }));
}

TEST_CASE("PC-03.c production Check cancellation reclaims only its process tree and permits recovery",
          "[PC-03.c][PC-03][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("The production authoring process-tree check requires the Windows Python-enabled build");
#else
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0303-check-cancel/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto checks = std::make_shared<PythonAuthoring>(root / "library", std::filesystem::path{}, std::filesystem::path{},
        std::function<bool(ApplyPhase)>{}, root / "work");
    auto draft = python_draft("pc03-active-check-cancel");
    draft.source = R"PY(import os
import pathlib
import subprocess
import sys
import threading

def process(inputs, params, state):
    child = subprocess.Popen([sys.executable, "-c", "import threading; threading.Event().wait()"])
    print("pc03-check-entered", flush=True)
    marker = pathlib.Path("pc03-check-processes.tmp")
    marker.write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    marker.replace("pc03-check-processes.txt")
    threading.Event().wait()
    return {"audio_out": inputs["audio_in"]}
)PY";
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks);

    struct OwnedProcess final {
        PROCESS_INFORMATION info{};
        ~OwnedProcess() {
            if (info.hProcess) {
                if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 5000);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } unrelated;
    std::wstring system_directory(MAX_PATH, L'\0');
    const auto system_directory_size = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(system_directory_size > 0); REQUIRE(system_directory_size < system_directory.size());
    system_directory.resize(system_directory_size);
    const auto canary_executable = std::filesystem::path(system_directory) / "ping.exe";
    std::wstring canary_command = L"\"" + canary_executable.wstring() + L"\" -t 127.0.0.1";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(canary_executable.c_str(), canary_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &unrelated.info) != 0);
    REQUIRE(WaitForSingleObject(unrelated.info.hProcess, 100) == WAIT_TIMEOUT);

    click(panel, "authoring.check");
    std::filesystem::path marker;
    DWORD parent_pid = 0, child_pid = 0;
    REQUIRE(pump([&] {
        return check_process_barrier(checks->work_directory() / "staging", marker, parent_pid, child_pid);
    }, 15000));
    { std::ofstream observed(root / "check-process-barrier.txt"); observed << parent_pid << ' ' << child_pid << '\n' << marker.generic_string() << '\n'; }
    REQUIRE(parent_pid != 0); REQUIRE(child_pid != 0); REQUIRE(parent_pid != child_pid);
    struct WaitHandle final { HANDLE value = nullptr; ~WaitHandle() { if (value) CloseHandle(value); } } parent, child;
    parent.value = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    child.value = OpenProcess(SYNCHRONIZE, FALSE, child_pid);
    REQUIRE(parent.value); REQUIRE(child.value);
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT);

    const auto cancelled_at = std::chrono::steady_clock::now();
    click(panel, "authoring.cancel");
    REQUIRE(pump([&] { return !panel.check_in_progress(); }, 5000));
    CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(5));
    CHECK_FALSE(panel.ready());
    CHECK(control<juce::Label>(panel, "authoring.check_status")->getText().contains("cancelled"));
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unrelated.info.hProcess, 0) == WAIT_TIMEOUT);

    bool archived_marker = false;
    for (const auto& path : check_files(checks->work_directory() / "staging", "python-log-", "stdout.log")) {
        std::ifstream in(path, std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(in), {});
        if (bytes.find("pc03-check-entered") != std::string::npos) { archived_marker = true; break; }
    }
    CHECK(archived_marker);

    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    source->selectAll();
    source->insertTextAtCaret("def process(inputs, params, state):\n    print('pc03-check-recovered', flush=True)\n    return {'audio_out': inputs['audio_in']}\n");
    click(panel, "authoring.check");
    REQUIRE(pump([&] { return panel.ready().has_value(); }, 15000));
    auto* check_log = control<audioalgo::editor::RunLogTextView>(panel, "authoring.check_logs"); REQUIRE(check_log);
    CHECK(check_log->getText().contains("pc03-check-recovered"));
#endif
}

TEST_CASE("PC-03.c production Check deadline preserves logs reclaims its tree and permits recovery",
          "[PC-03.c][PC-03][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("The production authoring process-tree check requires the Windows Python-enabled build");
#else
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0305-check-timeout/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto checks = std::make_shared<PythonAuthoring>(root / "library", std::filesystem::path{}, std::filesystem::path{},
        std::function<bool(ApplyPhase)>{}, root / "work", std::chrono::seconds(3));
    auto draft = python_draft("pc03-active-check-timeout");
    draft.source = R"PY(import os
import pathlib
import subprocess
import sys
import threading

def process(inputs, params, state):
    child = subprocess.Popen([sys.executable, "-c", "import threading; threading.Event().wait()"])
    print("pc03-check-entered", flush=True)
    marker = pathlib.Path("pc03-check-processes.tmp")
    marker.write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    marker.replace("pc03-check-processes.txt")
    threading.Event().wait()
    return {"audio_out": inputs["audio_in"]}
)PY";
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks);

    struct OwnedProcess final {
        PROCESS_INFORMATION info{};
        ~OwnedProcess() {
            if (info.hProcess) {
                if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 5000);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } unrelated;
    std::wstring system_directory(MAX_PATH, L'\0');
    const auto system_directory_size = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(system_directory_size > 0); REQUIRE(system_directory_size < system_directory.size());
    system_directory.resize(system_directory_size);
    const auto canary_executable = std::filesystem::path(system_directory) / "ping.exe";
    std::wstring canary_command = L"\"" + canary_executable.wstring() + L"\" -t 127.0.0.1";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(canary_executable.c_str(), canary_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &unrelated.info) != 0);
    REQUIRE(WaitForSingleObject(unrelated.info.hProcess, 100) == WAIT_TIMEOUT);

    click(panel, "authoring.check");
    std::filesystem::path marker;
    DWORD parent_pid = 0, child_pid = 0;
    REQUIRE(pump([&] {
        return check_process_barrier(checks->work_directory() / "staging", marker, parent_pid, child_pid);
    }, 15000));
    { std::ofstream observed(root / "check-process-barrier.txt"); observed << parent_pid << ' ' << child_pid << '\n' << marker.generic_string() << '\n'; }
    REQUIRE(parent_pid != 0); REQUIRE(child_pid != 0); REQUIRE(parent_pid != child_pid);
    struct WaitHandle final { HANDLE value = nullptr; ~WaitHandle() { if (value) CloseHandle(value); } } parent, child;
    parent.value = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    child.value = OpenProcess(SYNCHRONIZE, FALSE, child_pid);
    REQUIRE(parent.value); REQUIRE(child.value);
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT);

    const auto cancelled_at = std::chrono::steady_clock::now();
    REQUIRE(pump([&] { return !panel.check_in_progress(); }, 5000));
    CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(5));
    CHECK_FALSE(panel.ready());
    CHECK(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_execution_timeout"));
    CHECK(control<audioalgo::editor::RunLogTextView>(panel, "authoring.check_logs")->getText().contains("pc03-check-entered"));
    CHECK(control<juce::Label>(panel, "authoring.check_status")->getText().contains("failed"));
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unrelated.info.hProcess, 0) == WAIT_TIMEOUT);

    bool archived_marker = false;
    for (const auto& path : check_files(checks->work_directory() / "staging", "python-log-", "stdout.log")) {
        std::ifstream in(path, std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(in), {});
        if (bytes.find("pc03-check-entered") != std::string::npos) { archived_marker = true; break; }
    }
    CHECK(archived_marker);

    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    source->selectAll();
    source->insertTextAtCaret("def process(inputs, params, state):\n    print('pc03-check-recovered', flush=True)\n    return {'audio_out': inputs['audio_in']}\n");
    click(panel, "authoring.check");
    REQUIRE(pump([&] { return !panel.check_in_progress(); }, 15000));
    const auto terminal = control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString();
    std::ofstream(root / "recovery-terminal.txt") << terminal;
    INFO(terminal);
    REQUIRE(panel.ready().has_value());
    auto* check_log = control<audioalgo::editor::RunLogTextView>(panel, "authoring.check_logs"); REQUIRE(check_log);
    CHECK(check_log->getText().contains("pc03-check-recovered"));
#endif
}

TEST_CASE("PC-03.c closing the production panel cancels its Check before destruction and permits reopening",
          "[PC-03.c][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("The production authoring process-tree check requires the Windows Python-enabled build");
#else
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0304-check-close/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto checks = std::make_shared<PythonAuthoring>(root / "library", std::filesystem::path{}, std::filesystem::path{},
        std::function<bool(ApplyPhase)>{}, root / "work");
    auto draft = python_draft("pc03-active-check-close");
    draft.source = R"PY(import os
import pathlib
import subprocess
import sys
import threading

def process(inputs, params, state):
    child = subprocess.Popen([sys.executable, "-c", "import threading; threading.Event().wait()"])
    print("pc03-check-entered", flush=True)
    marker = pathlib.Path("pc03-check-processes.tmp")
    marker.write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    marker.replace("pc03-check-processes.txt")
    threading.Event().wait()
    return {"audio_out": inputs["audio_in"]}
)PY";
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    std::unique_ptr<audioalgo::editor::NodeAuthoringPanel> panel;
    int closed = 0;
    panel = std::make_unique<audioalgo::editor::NodeAuthoringPanel>(draft, true, storage, [&] { ++closed; panel.reset(); }, checks);

    struct OwnedProcess final {
        PROCESS_INFORMATION info{};
        ~OwnedProcess() {
            if (info.hProcess) {
                if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 5000);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } unrelated;
    std::wstring system_directory(MAX_PATH, L'\0');
    const auto system_directory_size = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(system_directory_size > 0); REQUIRE(system_directory_size < system_directory.size());
    system_directory.resize(system_directory_size);
    const auto canary_executable = std::filesystem::path(system_directory) / "ping.exe";
    std::wstring canary_command = L"\"" + canary_executable.wstring() + L"\" -t 127.0.0.1";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(canary_executable.c_str(), canary_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &unrelated.info) != 0);
    REQUIRE(WaitForSingleObject(unrelated.info.hProcess, 100) == WAIT_TIMEOUT);

    click(*panel, "authoring.check");
    std::filesystem::path marker;
    DWORD parent_pid = 0, child_pid = 0;
    REQUIRE(pump([&] {
        return check_process_barrier(checks->work_directory() / "staging", marker, parent_pid, child_pid);
    }, 15000));
    { std::ofstream observed(root / "check-process-barrier.txt"); observed << parent_pid << ' ' << child_pid << '\n' << marker.generic_string() << '\n'; }
    REQUIRE(parent_pid != 0); REQUIRE(child_pid != 0); REQUIRE(parent_pid != child_pid);
    struct WaitHandle final { HANDLE value = nullptr; ~WaitHandle() { if (value) CloseHandle(value); } } parent, child;
    parent.value = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    child.value = OpenProcess(SYNCHRONIZE, FALSE, child_pid);
    REQUIRE(parent.value); REQUIRE(child.value);
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT);

    const auto close_at = std::chrono::steady_clock::now();
    click(*panel, "authoring.close");
    REQUIRE(pump([&] { return closed == 1 && !panel; }, 5000));
    CHECK(std::chrono::steady_clock::now() - close_at < std::chrono::seconds(5));
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unrelated.info.hProcess, 0) == WAIT_TIMEOUT);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

    auto recovered = draft;
    recovered.source = "def process(inputs, params, state):\n    print('pc03-check-recovered', flush=True)\n    return {'audio_out': inputs['audio_in']}\n";
    int reopened = 0;
    panel = std::make_unique<audioalgo::editor::NodeAuthoringPanel>(recovered, true, storage, [&] { ++reopened; }, checks);
    click(*panel, "authoring.check");
    REQUIRE(pump([&] { return panel && panel->ready().has_value(); }, 15000));
    auto* check_log = control<audioalgo::editor::RunLogTextView>(*panel, "authoring.check_logs"); REQUIRE(check_log);
    CHECK(check_log->getText().contains("pc03-check-recovered"));
    panel.reset();
#endif
}

TEST_CASE("PC-03.c active Check View destruction and application shutdown reclaim owned work and permit recovery",
          "[PC-03.c][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("The production authoring process-tree check requires the Windows Python-enabled build");
#else
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    for (const bool application_shutdown : {false, true}) {
    INFO(application_shutdown);
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0306-check-view-close/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto checks = std::make_shared<PythonAuthoring>(root / "library", std::filesystem::path{}, std::filesystem::path{},
        std::function<bool(ApplyPhase)>{}, root / "work");
    auto draft = python_draft("pc03-active-check-close");
    draft.source = R"PY(import os
import pathlib
import subprocess
import sys
import threading

def process(inputs, params, state):
    child = subprocess.Popen([sys.executable, "-c", "import threading; threading.Event().wait()"])
    print("pc03-check-entered", flush=True)
    marker = pathlib.Path("pc03-check-processes.tmp")
    marker.write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    marker.replace("pc03-check-processes.txt")
    threading.Event().wait()
    return {"audio_out": inputs["audio_in"]}
)PY";
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto make_workspace = [&] {
        return std::make_unique<audioalgo::editor::EditorComponent>(registry, std::make_shared<Documents>(),
            std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false, checks);
    };
    auto workspace = make_workspace();
    click(*workspace, "authoring.new");
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(*workspace, "authoring.panel"); REQUIRE(panel);
    auto* editor = control<juce::CodeEditorComponent>(*panel, "authoring.source"); REQUIRE(editor);
    editor->selectAll(); editor->insertTextAtCaret(juce::String::fromUTF8(draft.source.c_str()));

    struct OwnedProcess final {
        PROCESS_INFORMATION info{};
        ~OwnedProcess() {
            if (info.hProcess) {
                if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, 5000);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } unrelated;
    std::wstring system_directory(MAX_PATH, L'\0');
    const auto system_directory_size = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(system_directory_size > 0); REQUIRE(system_directory_size < system_directory.size());
    system_directory.resize(system_directory_size);
    const auto canary_executable = std::filesystem::path(system_directory) / "ping.exe";
    std::wstring canary_command = L"\"" + canary_executable.wstring() + L"\" -t 127.0.0.1";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    REQUIRE(CreateProcessW(canary_executable.c_str(), canary_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &unrelated.info) != 0);
    REQUIRE(WaitForSingleObject(unrelated.info.hProcess, 100) == WAIT_TIMEOUT);

    click(*panel, "authoring.check");
    std::filesystem::path marker;
    DWORD parent_pid = 0, child_pid = 0;
    REQUIRE(pump([&] {
        return check_process_barrier(checks->work_directory() / "staging", marker, parent_pid, child_pid);
    }, 15000));
    { std::ofstream observed(root / "check-process-barrier.txt"); observed << parent_pid << ' ' << child_pid << '\n' << marker.generic_string() << '\n'; }
    REQUIRE(parent_pid != 0); REQUIRE(child_pid != 0); REQUIRE(parent_pid != child_pid);
    struct WaitHandle final { HANDLE value = nullptr; ~WaitHandle() { if (value) CloseHandle(value); } } parent, child;
    parent.value = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    child.value = OpenProcess(SYNCHRONIZE, FALSE, child_pid);
    REQUIRE(parent.value); REQUIRE(child.value);
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT);

    const auto close_at = std::chrono::steady_clock::now();
    int drained = 0;
    if (application_shutdown) {
        workspace->begin_shutdown([&] { ++drained; });
        REQUIRE(pump([&] { return drained == 1; }, 5000));
        CHECK_FALSE(control<audioalgo::editor::NodeAuthoringPanel>(*workspace, "authoring.panel"));
        CHECK_FALSE(workspace->isEnabled());
    }
    workspace.reset(); panel = nullptr;
    CHECK(std::chrono::steady_clock::now() - close_at < std::chrono::seconds(5));
    CHECK(WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unrelated.info.hProcess, 0) == WAIT_TIMEOUT);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

    workspace = make_workspace();
    click(*workspace, "authoring.new");
    panel = control<audioalgo::editor::NodeAuthoringPanel>(*workspace, "authoring.panel"); REQUIRE(panel);
    CHECK_FALSE(panel->ready());
    CHECK(control<audioalgo::editor::RunLogTextView>(*panel, "authoring.check_logs")->getText().isEmpty());
    editor = control<juce::CodeEditorComponent>(*panel, "authoring.source"); REQUIRE(editor);
    editor->selectAll(); editor->insertTextAtCaret("def process(inputs, params, state):\n    print('pc03-check-recovered', flush=True)\n    return {'audio_out': inputs['audio_in']}\n");
    click(*panel, "authoring.check");
    REQUIRE(pump([&] { return panel->ready().has_value(); }, 15000));
    auto* check_log = control<audioalgo::editor::RunLogTextView>(*panel, "authoring.check_logs"); REQUIRE(check_log);
    CHECK(check_log->getText().contains("pc03-check-recovered"));
    CHECK_FALSE(check_log->getText().contains("pc03-check-entered"));
    int reopened_drained = 0;
    workspace->begin_shutdown([&] { ++reopened_drained; });
    REQUIRE(pump([&] { return reopened_drained == 1; }, 5000));
    workspace.reset();
    std::ofstream(root / "scenario.txt") << (application_shutdown ? "application_shutdown" : "view_destruction") << '\n';
    }
#endif
}

TEST_CASE("NA-01.c discard after save failure survives an active check and never saves again",
          "[NA-01.c][NA-14.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    struct Storage final : audioalgo::authoring::DraftStorage {
        mutable std::atomic<int> saves{0};
        audioalgo::authoring::SaveResult save(const audioalgo::authoring::Draft&) const override { ++saves; return audioalgo::Diagnostic{"disk_full", "controlled failure"}; }
        audioalgo::authoring::LoadResult load(const std::string&) const override { return audioalgo::Diagnostic{"unused", ""}; }
        audioalgo::authoring::ListResult list() const override { return std::vector<std::string>{}; }
    };
    auto storage = std::make_shared<Storage>(); auto checks = std::make_shared<ControlledChecks>();
    int closed = 0;
    audioalgo::editor::NodeAuthoringPanel panel(audioalgo::authoring::python_draft("discard-race"), false, storage, [&] { ++closed; }, checks);
    ReleaseCheck release{checks};
    click(panel, "authoring.check"); REQUIRE(without_ui([&] { return checks->entered.load(); }));
    click(panel, "authoring.close");
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.discard")->isVisible(); }));
    click(panel, "authoring.discard"); CHECK(closed == 0);
    checks->release = true;
    REQUIRE(pump([&] { return !panel.check_in_progress(); }));
    CHECK(pump([&] { return closed == 1; }));
    CHECK(storage->saves.load() == 1); CHECK_FALSE(panel.ready());
}

TEST_CASE("NA-01.a production new button opens a persistent draft without editing the graph",
          "[NA-01.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    auto registry = std::make_shared<audioalgo::NodeRegistry>();
    REQUIRE(audioalgo::register_builtin_nodes(*registry));
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0001-draft/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root);
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(),
        std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false);
    std::filesystem::create_directories(root);
    const auto project_path = root / "before.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, workspace.controller().project())));
    auto bytes = [](const std::filesystem::path& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    const auto project = bytes(project_path);
    const auto generation = workspace.controller().document_generation();
    const auto catalog_size = registry->descriptors().size();
    click(workspace, "authoring.new");
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
    const auto id = panel->session().document().id;
    const std::string source = "# 中文 自定义节点\ndef process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain']}\n";
    const std::string declaration = R"({"type_id":"user.test_saved","parameters":[{"id":"gain","type":"float","default":0.25}]})";
    auto* editor = control<juce::CodeEditorComponent>(workspace, "authoring.source"); REQUIRE(editor);
    editor->selectAll(); editor->insertTextAtCaret(juce::String::fromUTF8(source.c_str()));
    auto* definition = control<juce::CodeEditorComponent>(workspace, "authoring.definition"); REQUIRE(definition);
    definition->selectAll(); definition->insertTextAtCaret(juce::String::fromUTF8(declaration.c_str()));
    click(workspace, "authoring.save");
    REQUIRE(pump([&] { return !panel->session().dirty(); }));
    const auto saved = storage->load(id); REQUIRE(std::holds_alternative<audioalgo::authoring::Draft>(saved));
    CHECK(std::get<audioalgo::authoring::Draft>(saved).source == source);
    CHECK(std::get<audioalgo::authoring::Draft>(saved).declaration == declaration);
    click(workspace, "authoring.close");
    REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, "authoring.open");
    REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") != nullptr; }));
    panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    CHECK(panel->session().document().id == id);
    CHECK(panel->session().document().source == source);
    CHECK(panel->session().document().declaration == declaration);
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "after.aaproj", workspace.controller().project())));
    CHECK(bytes(root / "after.aaproj") == project);
    CHECK(workspace.controller().document_generation() == generation);
    CHECK_FALSE(workspace.controller().dirty());
    CHECK_FALSE(workspace.controller().history().can_undo());
    CHECK(registry->descriptors().size() == catalog_size);
}

TEST_CASE("NA-01.c failed close retains the draft and offers explicit discard",
          "[NA-01.c][NA-01.b][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0002-save-recovery/artifacts" / juce::Uuid().toString().toStdString();
    std::atomic<bool> allow{true};
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root,
        [&](audioalgo::authoring::SavePhase) { return allow.load(); });
    auto draft = audioalgo::authoring::python_draft("recovery");
    REQUIRE_FALSE(storage->save(draft).has_value());
    int closed = 0;
    audioalgo::editor::NodeAuthoringPanel panel(draft, true, storage, [&] { ++closed; });
    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    source->selectAll(); source->insertTextAtCaret("def incomplete(");
    allow = false;
    click(panel, "authoring.close");
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.status")->getText().contains(juce::String::fromUTF8("Unsaved ·")); }));
    CHECK(closed == 0);
    CHECK(panel.session().dirty());
    CHECK(panel.session().document().source == "def incomplete(");
    REQUIRE(std::holds_alternative<audioalgo::authoring::Draft>(storage->load(draft.id)));
    CHECK(std::get<audioalgo::authoring::Draft>(storage->load(draft.id)).source == draft.source);
    auto* discard = control<juce::TextButton>(panel, "authoring.discard");
    REQUIRE(discard); REQUIRE(discard->isVisible());
    click(panel, "authoring.discard"); REQUIRE(pump([&] { return closed == 1; }));
    CHECK(std::get<audioalgo::authoring::Draft>(storage->load(draft.id)).source == draft.source);
}

TEST_CASE("NA-01.c application shutdown waits for draft save and failure leaves editing enabled",
          "[NA-01.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0003-shutdown/artifacts" / juce::Uuid().toString().toStdString();
    std::atomic<bool> allow{false};
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root,
        [&](audioalgo::authoring::SavePhase) { return allow.load(); });
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(),
        std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false);
    click(workspace, "authoring.new");
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
    const auto id = panel->session().document().id;
    auto* source = control<juce::CodeEditorComponent>(workspace, "authoring.source"); REQUIRE(source);
    source->selectAll(); source->insertTextAtCaret("# preserve on application close\ndef incomplete(");
    int drained = 0;
    workspace.begin_shutdown([&] { ++drained; });
    CHECK(workspace.isEnabled());
    REQUIRE(pump([&] {
        auto* status = control<juce::Label>(workspace, "authoring.status");
        return status && status->getText().contains(juce::String::fromUTF8("Unsaved ·"));
    }));
    CHECK(drained == 0); CHECK(panel->session().dirty());
    allow = true;
    click(workspace, "authoring.save");
    REQUIRE(pump([&] { return !panel->session().dirty(); }));
    workspace.begin_shutdown([&] { ++drained; });
    REQUIRE(pump([&] { return drained == 1; }));
    REQUIRE(std::holds_alternative<audioalgo::authoring::Draft>(storage->load(id)));
    CHECK(std::get<audioalgo::authoring::Draft>(storage->load(id)).source == "# preserve on application close\ndef incomplete(");
}

TEST_CASE("NA-04.a production panel offers isolated source checking",
          "[NA-04.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0004-python-check/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    auto draft = audioalgo::authoring::python_draft("check");
    const auto marker = root / "should-not-execute.txt";
    draft.source = "from pathlib import Path\nPath(" + juce::JSON::toString(juce::String(marker.generic_string())).toStdString() + ").write_text('executed')\ndef process(:\n    pass\n";
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks);
    REQUIRE(control<juce::TextButton>(panel, "authoring.check") != nullptr);
    click(panel, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.check_status")->getText().contains("failed"); }));
    CHECK_FALSE(std::filesystem::exists(marker)); CHECK_FALSE(panel.ready());
    const auto error = control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText();
    CHECK(error.contains("python_syntax_error")); CHECK(error.contains("user_source.py:3:13:"));
    auto* editor = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(editor);
    editor->selectAll(); editor->insertTextAtCaret(juce::String::fromUTF8(audioalgo::authoring::python_draft("check").source.c_str()));
    click(panel, "authoring.check");
    REQUIRE(pump([&] { return panel.ready().has_value() || control<juce::Label>(panel, "authoring.check_status")->getText().contains("failed"); }));
    INFO(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString());
    REQUIRE(panel.ready());
    const auto& ready = *panel.ready();
    CHECK(ready.draft == panel.session().document());
    CHECK(ready.syntax_pid != 0); CHECK(ready.probe_pid != 0); CHECK(ready.probe_pid != ready.syntax_pid);
    CHECK(ready.source_sha256.size() == 64); CHECK(ready.package_sha256.size() == 64);
    CHECK(std::filesystem::is_regular_file(ready.package_root / "user_source.py"));
    REQUIRE(ready.probe_outputs.contains("audio_out"));
    const auto& output = ready.probe_outputs.at("audio_out");
    CHECK(output.frame_count() == 513); CHECK(output.sample_rate_hz() == 16000); CHECK(output.channel_count() == 1);
    for (std::size_t n = 0; n < output.frame_count(); ++n) {
        const auto expected = 0.1 * std::sin(0.03 * (n + 1));
        CHECK(std::abs(output.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
    }
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("package_root", juce::String(ready.package_root.generic_string()));
    receipt->setProperty("probe_directory", juce::String(ready.probe_directory.generic_string()));
    receipt->setProperty("source_sha256", juce::String(ready.source_sha256));
    receipt->setProperty("declaration_sha256", juce::String(ready.declaration_sha256));
    receipt->setProperty("package_sha256", juce::String(ready.package_sha256));
    receipt->setProperty("interpreter_sha256", juce::String(ready.interpreter_sha256));
    receipt->setProperty("syntax_pid", static_cast<juce::int64>(ready.syntax_pid));
    receipt->setProperty("probe_pid", static_cast<juce::int64>(ready.probe_pid));
    receipt->setProperty("draft_generation", static_cast<juce::int64>(ready.draft.generation));
    std::ofstream(root / "receipt.json", std::ios::binary) << juce::JSON::toString(juce::var(receipt.get()), false).toStdString();
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
    CHECK_FALSE(panel.ready());
#endif
}

TEST_CASE("NA-05.b generated Python wrapper rejects invalid outputs and subsequent valid check recovers",
          "[NA-05.b][NA-05.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0004-python-check/errors" / juce::Uuid().toString().toStdString();
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    audioalgo::editor::NodeAuthoringPanel panel(audioalgo::authoring::python_draft("outputs"), false, storage, [] {}, checks);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    for (const auto& [expression, error] : std::vector<std::pair<std::string, std::string>>{
        {"{'audio_out': inputs['audio_in'].astype('float64')}", "python_output_dtype_invalid"},
        {"{'audio_out': inputs['audio_in'][:, :-1]}", "python_output_shape_invalid"},
        {"{'audio_out': inputs['audio_in'] * float('nan')}", "python_output_non_finite"},
        {"{}", "python_output_ports_invalid"},
        {"{'extra': inputs['audio_in']}", "python_output_ports_invalid"}}) {
        auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source");
        source->selectAll(); source->insertTextAtCaret(juce::String("def process(inputs, params, state):\n    return " + expression + "\n"));
        click(panel, "authoring.check");
        REQUIRE(pump([&] { return control<juce::Label>(panel, "authoring.check_status")->getText().contains("failed"); }));
        CHECK_FALSE(panel.ready());
        INFO(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString());
        CHECK(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains(juce::String(error)));
    }
    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source");
    source->selectAll(); source->insertTextAtCaret(juce::String(audioalgo::authoring::python_draft("outputs").source));
    click(panel, "authoring.check"); REQUIRE(pump([&] { return panel.ready().has_value(); }));
    CHECK(panel.ready()->probe_outputs.at("audio_out").frame_count() == 513);
#else
    click(panel, "authoring.check");
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

TEST_CASE("NA-11.a schema2 preserves exact managed version while schema1 stays legacy",
          "[NA-11.a][node_authoring][component]") {
    const auto fixture = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "tests/fixtures/authoring/schema2_locked.aaproj";
    auto loaded = audioalgo::load_project_file(fixture);
    if (auto* e = std::get_if<audioalgo::Diagnostic>(&loaded)) INFO(e->code + ": " + e->message);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0005-version-format/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    const auto path = root / "roundtrip.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(path, std::get<audioalgo::ProjectModel>(loaded))));
    auto read = [](const std::filesystem::path& p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); };
    const auto original = juce::JSON::parse(juce::String::fromUTF8(read(fixture).c_str()));
    const auto roundtrip = juce::JSON::parse(juce::String::fromUTF8(read(path).c_str()));
    CHECK(roundtrip["schema_version"] == original["schema_version"]);
    CHECK(juce::JSON::toString(roundtrip["nodes"][0]["managed_version"]) == juce::JSON::toString(original["nodes"][0]["managed_version"]));
    CHECK(juce::JSON::toString(roundtrip["nodes"][0]["unknown_vendor"]) == juce::JSON::toString(original["nodes"][0]["unknown_vendor"]));
    CHECK(juce::JSON::toString(roundtrip["unknown_project"]) == juce::JSON::toString(original["unknown_project"]));
    const auto second = audioalgo::load_project_file(path); REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(second));
    CHECK(std::get<audioalgo::ProjectModel>(second).schema_version == 2);
}

TEST_CASE("NA-09.a an unresolved exact version cannot run a type-only replacement",
          "[NA-09.a][node_authoring][component]") {
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    auto descriptor = *registry.find("builtin.gain"); descriptor.type_id = "user.fixture";
    REQUIRE(registry.register_descriptor(descriptor));
    audioalgo::ProjectModel project;
    auto node = audioalgo::NodeInstance{"custom", "user.fixture", {{"gain_db", 0.0}}, 0, 0};
    node.managed_version = audioalgo::NodeInstance::ManagedVersion{"user.authoring.fixture", std::string(64, 'a'), "node_packages/fixture/" + std::string(64, 'a'), "python-function-v1"};
    project.schema_version = 2; project.nodes.push_back(node);
    const auto errors = audioalgo::validate_project_structure(project, registry);
    CHECK(std::any_of(errors.begin(), errors.end(), [](const auto& e) { return e.code == "managed_version_unavailable" && e.node_id == "custom"; }));
}

TEST_CASE("NA-04.c Ready retains the exact source that was probed despite mutable candidate edits",
          "[NA-04.c][NA-07.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0006-frozen-check/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    auto draft = audioalgo::authoring::python_draft("immutable");
    draft.source = "from pathlib import Path\ndef process(inputs, params, state):\n"
        "    for path in Path(" + juce::JSON::toString(juce::String(root.generic_string())).toStdString() + ").glob('staging/*/candidate/user_source.py'):\n"
        "        path.write_text('def process(inputs, params, state):\\n    return {}\\n', encoding='utf-8')\n"
        "    return {'audio_out': inputs['audio_in'] * params['gain']}\n";
    audioalgo::editor::NodeAuthoringPanel panel(draft, false, storage, [] {}, checks);
    click(panel, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return panel.ready().has_value() || control<juce::Label>(panel, "authoring.check_status")->getText().contains("failed"); }));
    INFO(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString());
    REQUIRE(panel.ready());
    std::ifstream f(panel.ready()->package_root / "user_source.py", std::ios::binary);
    const std::string captured(std::istreambuf_iterator<char>(f), {});
    CHECK(captured == draft.source);
    CHECK(panel.ready()->draft.source == captured);
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

TEST_CASE("NA-11.b first schema migration preserves the original bytes in a verified backup",
          "[NA-11.b][node_authoring][component]") {
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0007-migration/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    const auto path = root / "legacy.aaproj";
    const std::string original = "{\n  \"schema_version\":1, \"project_id\":\"legacy\",\"name\":\"Before migration\",\"revision\":3,\"selected_output_node_id\":\"\",\"nodes\":[],\"connections\":[],\"unknown\":{\"keep\":true}\n}\n\n";
    { std::ofstream f(path, std::ios::binary); f << original; }
    auto loaded = audioalgo::load_project_file(path); REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(loaded));
    auto project = std::get<audioalgo::ProjectModel>(loaded); project.schema_version = 2;
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(path, project)));
    const auto backup = std::filesystem::path(path.string() + ".schema-1.bak");
    REQUIRE(std::filesystem::is_regular_file(backup));
    std::ifstream f(backup, std::ios::binary); CHECK(std::string(std::istreambuf_iterator<char>(f), {}) == original);
    REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(audioalgo::load_project_file(path)));
    CHECK(std::get<audioalgo::ProjectModel>(audioalgo::load_project_file(path)).schema_version == 2);
    const auto fresh = root / "new.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(fresh, project)));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(fresh.string() + ".schema-1.bak")));
}

TEST_CASE("NA-11.b migration faults and conflicting backups never replace original project bytes",
          "[NA-11.b][node_authoring][component]") {
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0007-migration/faults" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    const std::string original = R"({"schema_version":1,"project_id":"legacy","name":"original","revision":0,"selected_output_node_id":"","nodes":[],"connections":[]})";
    auto read = [](const auto& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    for (const auto phase : {audioalgo::ProjectSavePhase::backup, audioalgo::ProjectSavePhase::write,
                            audioalgo::ProjectSavePhase::verify, audioalgo::ProjectSavePhase::replace}) {
        const auto path = root / (std::to_string(static_cast<int>(phase)) + ".aaproj");
        { std::ofstream f(path, std::ios::binary); f << original; }
        auto project = std::get<audioalgo::ProjectModel>(audioalgo::load_project_file(path)); project.schema_version = 2;
        const auto result = audioalgo::save_project_file(path, project, {[phase](auto p) { return p != phase; }});
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
        CHECK(read(path) == original);
        REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(audioalgo::load_project_file(path)));
        CHECK(std::get<audioalgo::ProjectModel>(audioalgo::load_project_file(path)).schema_version == 1);
    }
    const auto path = root / "conflicting.aaproj";
    const auto backup = std::filesystem::path(path.string() + ".schema-1.bak");
    { std::ofstream f(path); f << original; std::ofstream b(backup); b << "existing backup - preserve exactly"; }
    auto project = std::get<audioalgo::ProjectModel>(audioalgo::load_project_file(path)); project.schema_version = 2;
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(audioalgo::save_project_file(path, project)));
    CHECK(read(path) == original); CHECK(read(backup) == "existing backup - preserve exactly");
    const auto concurrent = root / "concurrent.aaproj";
    { std::ofstream f(concurrent); f << original; }
    const auto newer = original + "\n  \n";
    const auto result = audioalgo::save_project_file(concurrent, project, {[&](auto phase) {
        if (phase == audioalgo::ProjectSavePhase::verify) { std::ofstream f(concurrent, std::ios::binary); f << newer; }
        return true;
    }});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
    CHECK(std::get<audioalgo::Diagnostic>(result).code == "project_save_conflict");
    CHECK(read(concurrent) == newer);
}

TEST_CASE("NA-07.a checked Python draft applies one immutable library version",
          "[NA-07.a][NA-08.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0008-apply/artifacts" / juce::Uuid().toString().toStdString();
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(),
        std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false, checks);
    click(workspace, "authoring.new");
    REQUIRE(control<juce::TextButton>(workspace, "authoring.apply") != nullptr);
    CHECK_FALSE(control<juce::TextButton>(workspace, "authoring.apply")->isEnabled());
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
    const auto before = workspace.controller().project();
    click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto checked_ok = pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); });
    INFO(control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().toStdString());
    REQUIRE(checked_ok);
    const auto type = panel->ready()->descriptor.type_id;
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library." + juce::String(type)) != nullptr; }));
    CHECK(workspace.controller().project().revision == before.revision);
    CHECK(workspace.controller().project().nodes.size() == before.nodes.size());
    CHECK_FALSE(workspace.controller().dirty()); CHECK_FALSE(workspace.controller().history().can_undo());
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::Label>(workspace, "authoring.check_status")->getText().contains("Available"); }));
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, ("library." + type).c_str());
    REQUIRE(workspace.controller().project().nodes.size() == 1);
    const auto& node = workspace.controller().project().nodes.front(); REQUIRE(node.managed_version);
    CHECK(node.managed_version->content_sha256.size() == 64); CHECK(workspace.controller().project().schema_version == 2);
    CHECK(std::get<double>(node.parameters.at("gain")) == 0.5);
    CHECK(control<juce::Slider>(workspace, "parameter.gain") != nullptr);
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

TEST_CASE("NA-08.a production Apply library insertion Inspector and Run execute the checked Python code",
          "[NA-08.a][NA-11.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0008-apply/project-run" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    audioalgo::AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.project_id = "authoring-real-run"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, {"output", "builtin.output", {}, 640, 100}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    struct RealDocuments final : audioalgo::ProjectDocumentService {
        audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path& p) const override { return audioalgo::load_project_file(p); }
        audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path& p, const audioalgo::ProjectModel& m) const override { return audioalgo::save_project_file(p, m); }
    };
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "authoring/drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring");
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<RealDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, checks);
    workspace.open_project_file(root / "project.aaproj");
    REQUIRE(pump([&] { return workspace.controller().project().project_id == project.project_id; }));
    click(workspace, "authoring.new"); click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto checked_ok = pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); });
    INFO(control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().toStdString());
    REQUIRE(checked_ok);
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    const auto type = panel->ready()->descriptor.type_id;
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library." + juce::String(type)) != nullptr; }));
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, ("library." + type).c_str());
    REQUIRE(workspace.controller().project().nodes.size() == 3);
    const auto custom = workspace.controller().project().nodes.back().id;
    auto* gain = control<juce::Slider>(workspace, "parameter.gain"); REQUIRE(gain);
    gain->setValue(0.8, juce::sendNotificationSync);
    REQUIRE(std::abs(gain->getValue() - 0.8) <= 1e-15);
    REQUIRE(std::get<double>(workspace.controller().project().nodes.back().parameters.at("gain")) == gain->getValue());
    auto* canvas = control<audioalgo::editor::GraphCanvas>(workspace, "project.canvas"); REQUIRE(canvas);
    auto point = [&](const std::string& node, const std::string& port) {
        for (const auto& instance : workspace.controller().project().nodes) if (instance.id == node)
            for (const auto& p : audioalgo::editor::make_node_geometry(instance, registry->find(instance.type_id), canvas->coordinate_transform()).ports)
                if (p.id == port) return juce::Point<float>(p.anchor.x, p.anchor.y);
        throw std::runtime_error("Missing real canvas port");
    };
    auto mouse = [&](juce::Point<float> p, int buttons, juce::Point<float> origin, bool dragged) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(buttons),
            0, 0, 0, 0, 0, canvas, canvas, juce::Time(100), origin, juce::Time(0), 1, dragged);
    };
    auto wire = [&](auto from, auto to) {
        canvas->mouseDown(mouse(from, juce::ModifierKeys::leftButtonModifier, from, false));
        canvas->mouseDrag(mouse(to, juce::ModifierKeys::leftButtonModifier, from, true));
        canvas->mouseUp(mouse(to, 0, from, true));
    };
    wire(point("file", "audio"), point(custom, "audio_in")); wire(point(custom, "audio_out"), point("output", "audio"));
    REQUIRE(workspace.controller().project().connections.size() == 2);
    click(workspace, "project.save"); REQUIRE(pump([&] { return !workspace.controller().dirty(); }));
    click(workspace, "project.run");
    auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
    REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }));
    std::vector<std::filesystem::path> wavs;
    // Outputs are direct children of each run. Avoid traversing private runner
    // trees with the STL Windows MAX_PATH-limited directory enumerator.
    for (const auto& run : std::filesystem::directory_iterator(root / "runs"))
        if (std::filesystem::is_regular_file(run.path() / "output.wav")) wavs.push_back(run.path() / "output.wav");
    REQUIRE(wavs.size() == 1);
    auto read = audioalgo::read_wav_file(wavs.front()); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
    const auto& output = std::get<audioalgo::AudioBuffer>(read); REQUIRE(output.frame_count() == 513);
    for (std::size_t n = 0; n < 513; ++n) {
        const auto expected = 0.16 * std::sin(0.03 * (n + 1));
        CHECK(std::abs(output.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
    }
    std::ofstream(root / "receipt.json") << "{\"output_file\":" << juce::JSON::toString(juce::String(wavs.front().generic_string())).toStdString() << ",\"gain\":0.8}";
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

namespace {
void print_workspace_scenario(bool failure_checks, bool check_log_checks = false, bool live_checks = false, bool heartbeat_checks = false, bool filter_checks = false, bool level_checks = false, bool history_checks = false) {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / (history_checks ? "build/reports/loops/PC0407-log-history/artifacts" : level_checks ? "build/reports/loops/PC0406-level-filter/artifacts" : filter_checks ? "build/reports/loops/PC0404-node-filter/artifacts" : failure_checks ? "build/reports/loops/PC0301-run-log/artifacts" : "build/reports/loops/PC0201-print/artifacts") / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    if (level_checks) std::cout << "PC0406_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    if (history_checks) std::cout << "PC0407_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    audioalgo::AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.project_id = "pc02-print"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, {"output", "builtin.output", {}, 640, 100}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "authoring/drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring");
    auto preview=std::make_shared<Preview>();
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
        preview, storage, false, checks);
    workspace.open_project_file(root / "project.aaproj");
    REQUIRE(pump([&] { return workspace.controller().project().project_id == project.project_id; }));
    click(workspace, "authoring.new");
    control<juce::TextEditor>(workspace, "authoring.type_id")->setText("user.pc02_print", true);
    control<juce::TextEditor>(workspace, "authoring.display_name")->setText("Print", true);
    auto* editor = control<juce::CodeEditorComponent>(workspace, "authoring.source"); REQUIRE(editor);
    std::string body = "def process(inputs, params, state):\n    print('aaaaaaa', flush=True)\n";
    if (level_checks) body += "    import sys\n    print('level-error-marker', file=sys.stderr, flush=True)\n";
    if (history_checks) {
        const auto literal=juce::JSON::toString(juce::var(juce::String(root.generic_string())),true).toStdString();
        body += "    if params['gain'] < 0:\n        import pathlib, time\n        root=pathlib.Path("+literal+")\n        (root/'history-entered.txt').write_text('entered',encoding='ascii')\n        while (root/'history-hold.txt').exists(): time.sleep(.005)\n";
    }
    if (failure_checks) body += "    if params['gain'] < 0: raise RuntimeError('pc03 failure after print')\n";
    if (live_checks) {
        body += "    if params['gain'] < -1:\n        import time\n        print('live-before-terminal', flush=True)\n";
        body += heartbeat_checks ? "        for n in range(650):\n            print('live-heartbeat-record-' + str(n), flush=True)\n            time.sleep(.02)\n" : "        while True: time.sleep(.005)\n";
    }
    body += "    return {'audio_out': inputs['audio_in']}\n";
    editor->getDocument().replaceAllContent(juce::String(body));
    REQUIRE(pump([&] { return control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel")->session().document().source.find("aaaaaaa") != std::string::npos; }));
    CHECK_FALSE(control<juce::TextButton>(workspace, "library.user.pc02_print"));
    click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    CHECK_FALSE(control<juce::TextButton>(workspace, "library.user.pc02_print"));
    if (check_log_checks) {
        auto* check_logs = control<audioalgo::editor::RunLogTextView>(workspace, "authoring.check_logs"); REQUIRE(check_logs);
        auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
        const auto text = check_logs->getText();
        CHECK(text.contains("Check")); CHECK(text.contains("contract")); CHECK(text.contains(panel->session().document().id));
        CHECK(text.contains(panel->ready()->source_sha256)); CHECK(text.contains(panel->ready()->declaration_sha256));
        CHECK(text.contains(panel->ready()->request_id));
        const auto raw = text.toStdString(); std::size_t count = 0, offset = 0;
        while ((offset = raw.find("aaaaaaa", offset)) != std::string::npos) { ++count; offset += 7; }
        CHECK(count == 5); // independent probe contract: 513 frames, host127
        auto* run_logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(run_logs);
        CHECK(run_logs->getText().isEmpty());
        std::ofstream(root / "check-ui-log.txt", std::ios::binary) << text.toStdString();
    }
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library.user.pc02_print") != nullptr; }));
    REQUIRE(workspace.controller().project().nodes.size() == 2);
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    auto* button = control<audioalgo::editor::NodeLibraryButton>(workspace, "library.user.pc02_print"); REQUIRE(button);
    auto event = [&](juce::Component* target, juce::Point<float> p, int modifiers) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(modifiers),
            0, 0, 0, 0, 0, target, target, juce::Time(100), p, juce::Time(0), 1, false);
    };
    button->mouseDown(event(button, {10, 10}, juce::ModifierKeys::leftButtonModifier));
    auto* canvas = control<audioalgo::editor::GraphCanvas>(workspace, "project.canvas"); REQUIRE(canvas);
    // JUCE target input with the real library source/epoch. OS pointer routing
    // remains a separate desktop requirement; no add_node shortcut is used.
    juce::DragAndDropTarget::SourceDetails drop(audioalgo::editor::make_library_drag_description("user.pc02_print"), button, {380, 200});
    REQUIRE(canvas->isInterestedInDragSource(drop)); canvas->itemDragEnter(drop); canvas->itemDropped(drop);
    REQUIRE(workspace.controller().project().nodes.size() == 3);
    const auto custom = workspace.controller().project().nodes.back().id;
    std::string second_custom;
    if (filter_checks) {
        button->mouseDown(event(button, {10, 10}, juce::ModifierKeys::leftButtonModifier));
        juce::DragAndDropTarget::SourceDetails second_drop(audioalgo::editor::make_library_drag_description("user.pc02_print"), button, {520, 200});
        REQUIRE(canvas->isInterestedInDragSource(second_drop)); canvas->itemDragEnter(second_drop); canvas->itemDropped(second_drop);
        REQUIRE(workspace.controller().project().nodes.size() == 4);
        second_custom = workspace.controller().project().nodes.back().id;
        wire_workspace(workspace, *registry, "file", "audio", custom, "audio_in");
        wire_workspace(workspace, *registry, custom, "audio_out", second_custom, "audio_in");
        wire_workspace(workspace, *registry, second_custom, "audio_out", "output", "audio");
    } else {
        wire_workspace(workspace, *registry, "file", "audio", custom, "audio_in");
        wire_workspace(workspace, *registry, custom, "audio_out", "output", "audio");
    }
    click(workspace, "project.save"); REQUIRE(pump([&] { return !workspace.controller().dirty(); }));
    std::string previous;
    for (int attempt = 0; attempt < 2; ++attempt) {
        click(workspace, "project.run");
        auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }));
        auto* logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs);
        const auto text = logs->getText();
        CHECK(text.contains(custom)); CHECK(text.contains("user.pc02_print")); CHECK(text.contains("pc02-print"));
        CHECK(text.contains("aaaaaaa\n")); // ADR0039: chunk headers may split the two records
        const auto log_bytes = text.toStdString(); std::size_t occurrences = 0, offset = 0;
        while ((offset = log_bytes.find("aaaaaaa", offset)) != std::string::npos) { ++occurrences; offset += 7; }
        CHECK(occurrences == (filter_checks ? 4 : 2));
        CHECK(text != juce::String(previous)); previous = text.toStdString();
        std::ofstream(root / ("ui-log-" + std::to_string(attempt) + ".txt"), std::ios::binary) << previous;
    }
    if (history_checks) {
        auto* logs=control<audioalgo::editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);
        auto* status=control<audioalgo::editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);
        const auto current=logs->getText(),terminal=status->status_text();
        const auto request_from=[](const juce::String& text){auto start=text.indexOf("[Run ");REQUIRE(start>=0);auto end=text.indexOfChar(start+5,' ');REQUIRE(end>start+5);return text.substring(start+5,end);};
        const auto old_request=request_from(juce::File((root/"ui-log-0.txt").generic_string()).loadFileAsString());
        const auto current_request=request_from(current);REQUIRE(old_request!=current_request);
        const auto revision=workspace.controller().revision();const auto dirty=workspace.controller().dirty();
        click(workspace,"project.run_logs.history_refresh");
        auto* history=control<juce::ComboBox>(workspace,"project.run_logs.history");REQUIRE(history);
        REQUIRE(pump([&]{return history->getNumItems()>=3&&history->isEnabled();}));
        int old_id=0;for(int i=0;i<history->getNumItems();++i)if(history->getItemText(i).contains(old_request))old_id=history->getItemId(i);REQUIRE(old_id>1);
        history->setSelectedId(old_id,juce::sendNotificationSync);
        REQUIRE(pump([&]{return logs->getText().contains(old_request)&&history->isEnabled();}));
        CHECK_FALSE(logs->getText().contains(current_request));CHECK(logs->getText().contains("aaaaaaa"));CHECK(status->status_text()==terminal);
        CHECK(workspace.controller().revision()==revision);CHECK(workspace.controller().dirty()==dirty);
        std::ofstream(root/"history-old-ui.log",std::ios::binary)<<logs->getText().toStdString();
        click(workspace,"project.play");
        std::filesystem::path current_output;
        for(const auto& run:std::filesystem::directory_iterator(root/"runs")){
            auto record=juce::JSON::parse(juce::File((run.path()/"run.json").generic_string()));if(record["request_id"].toString()==current_request)current_output=run.path()/"output.wav";
        }
        REQUIRE_FALSE(current_output.empty());CHECK(preview->played==current_output);
        history->setSelectedId(1,juce::sendNotificationSync);REQUIRE(pump([&]{return logs->getText().contains(current_request)&&history->isEnabled();}));
        CHECK_FALSE(logs->getText().contains(old_request));CHECK(preview->played==current_output);
        std::ofstream(root/"history-current-ui.log",std::ios::binary)<<logs->getText().toStdString();
        {std::ofstream(root/"history-hold.txt")<<"hold";}
        struct ReleaseHistoryGate{std::filesystem::path path;~ReleaseHistoryGate(){std::error_code ec;std::filesystem::remove(path,ec);}}release{root/"history-hold.txt"};
        auto* gain=control<juce::Slider>(workspace,"parameter.gain");REQUIRE(gain);gain->setValue(-1,juce::sendNotificationSync);click(workspace,"project.run");
        REQUIRE(pump([&]{return std::filesystem::is_regular_file(root/"history-entered.txt");}));
        click(workspace,"project.run_logs.history_refresh");CHECK_FALSE(history->isEnabled());
        // The render occupies the sole worker, so this read is definitely queued.
        // New/Open revoke its generation before the worker can deliver anything.
        click(workspace,"project.new");
        audioalgo::ProjectModel replacement;replacement.project_id="history-replacement";
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"replacement.aaproj",replacement)));
        workspace.open_project_file(root/"replacement.aaproj");REQUIRE(pump([&]{return workspace.controller().project().project_id=="history-replacement";}));
        CHECK(logs->getText().isEmpty());CHECK(history->getNumItems()==1);CHECK(history->getSelectedId()==1);
        click(workspace,"project.play");CHECK(status->status_text().contains("Run the graph before previewing audio"));CHECK(preview->played==current_output);
        int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(logs->getText().isEmpty());
        std::ofstream(root/"history-cleared-ui.log",std::ios::binary)<<logs->getText().toStdString();
        std::ofstream(root/"history-demo.json")<<"{\"old_request\":\""<<old_request<<"\",\"current_request\":\""<<current_request<<"\",\"preview\":\""<<current_output.generic_string()<<"\",\"stale_history_cleared\":true}";
    }
    if (filter_checks) {
        auto* logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs);
        auto* filter = control<juce::ComboBox>(workspace, "project.run_logs.node_filter"); REQUIRE(filter);
        auto find_filter_id = [&](const std::string& node_id) {
            for (int i = 0; i < filter->getNumItems(); ++i)
                if (filter->getItemText(i).contains(node_id)) return filter->getItemId(i);
            return 0;
        };
        const auto first_filter = find_filter_id(custom), second_filter = find_filter_id(second_custom);
        REQUIRE(first_filter > 1); REQUIRE(second_filter > 1); CHECK(first_filter != second_filter);
        const auto all_logs = logs->getText();
        const auto run_marker = all_logs.indexOf("[Run "); REQUIRE(run_marker >= 0);
        const auto request_end = all_logs.indexOfChar(run_marker + 5, ' '); REQUIRE(request_end > run_marker + 5);
        const auto request_id = all_logs.substring(run_marker + 5, request_end);
        std::vector<std::string> filtered_logs;
        for (const auto [filter_id, visible_node, hidden_node] : {
                 std::tuple{first_filter, custom, second_custom}, std::tuple{second_filter, second_custom, custom}}) {
            filter->setSelectedId(filter_id, juce::sendNotificationSync);
            const auto visible = logs->getText();
            CHECK(visible.contains(visible_node)); CHECK_FALSE(visible.contains(hidden_node));
            CHECK(visible.contains(request_id));
            std::size_t visible_prints = 0, offset = 0;
            const auto bytes = visible.toStdString();
            while ((offset = bytes.find("aaaaaaa", offset)) != std::string::npos) { ++visible_prints; offset += 7; }
            CHECK(visible_prints == 2);
            filtered_logs.push_back(bytes);
        }
        filter->setSelectedId(1, juce::sendNotificationSync);
        const auto restored = logs->getText();
        CHECK(restored.contains(custom)); CHECK(restored.contains(second_custom));
        CHECK(restored.contains(request_id));
        std::ofstream(root / "filter-first-node.log", std::ios::binary) << filtered_logs[0];
        std::ofstream(root / "filter-second-node.log", std::ios::binary) << filtered_logs[1];
        std::ofstream(root / "filter-all-nodes.log", std::ios::binary) << restored.toStdString();
        std::ofstream(root / "filter-demo.json") << "{\"project_file\":\"project.aaproj\",\"request_id\":\"" << request_id.toStdString()
            << "\",\"node_ids\":[\"" << custom << "\",\"" << second_custom << "\"],\"samples\":513,\"expected_print_records_per_node\":2}\n";
    }
    if (level_checks) {
        auto* logs=control<audioalgo::editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);
        auto* levels=control<juce::ComboBox>(workspace,"project.run_logs.level_filter");REQUIRE(levels);
        const auto all=logs->getText();CHECK(all.contains("aaaaaaa"));CHECK(all.contains("level-error-marker"));
        const auto start=all.indexOf("[Run ");REQUIRE(start>=0);const auto end=all.indexOfChar(start+5,' ');REQUIRE(end>start+5);
        const auto request=all.substring(start+5,end);
        for(const auto& level:{"info","error"}){
            int id=0;for(int i=0;i<levels->getNumItems();++i)if(levels->getItemText(i)==level)id=levels->getItemId(i);REQUIRE(id>1);
            levels->setSelectedId(id,juce::sendNotificationSync);const auto visible=logs->getText();CHECK(visible.contains(request));CHECK(visible.contains(custom));
            CHECK(visible.contains(level==std::string("info")?"aaaaaaa":"level-error-marker"));CHECK_FALSE(visible.contains(level==std::string("info")?"level-error-marker":"aaaaaaa"));
            std::ofstream(root/("ui-level-"+std::string(level)+".log"),std::ios::binary)<<visible.toStdString();
        }
        levels->setSelectedId(1,juce::sendNotificationSync);CHECK(logs->getText()==all);
        std::ofstream(root/"ui-level-all.log",std::ios::binary)<<logs->getText().toStdString();
        std::ofstream(root/"level-demo.json")<<"{\"request_id\":\""<<request<<"\",\"node_id\":\""<<custom<<"\",\"project_file\":\"project.aaproj\"}";
    }
    if (live_checks) {
        auto* gain = control<juce::Slider>(workspace, "parameter.gain"); REQUIRE(gain);
        gain->setValue(heartbeat_checks ? -2 : -1.5, juce::sendNotificationSync); click(workspace, "project.run");
        auto* logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs);
        REQUIRE(pump([&] { return logs->getText().contains("live-before-terminal"); }));
        auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        CHECK_FALSE(status->status_text().contains("Completed run"));
        CHECK(logs->getText().getNumBytesAsUTF8() <= 256 * 1024);
        if (heartbeat_checks) {
            struct Heartbeat final : juce::Timer {
                std::chrono::steady_clock::time_point previous;
                std::vector<double> gaps;
                void timerCallback() override { const auto now=std::chrono::steady_clock::now();gaps.push_back(std::chrono::duration<double,std::milli>(now-previous).count());previous=now; }
            } heartbeat;
            const auto started=std::chrono::steady_clock::now();heartbeat.previous=started;heartbeat.startTimer(20);
            REQUIRE(pump([&] { return std::chrono::steady_clock::now()-started>=std::chrono::seconds(10); },11000));
            heartbeat.stopTimer();const auto finished=std::chrono::steady_clock::now();
            heartbeat.gaps.push_back(std::chrono::duration<double,std::milli>(finished-heartbeat.previous).count());
            const auto duration=std::chrono::duration<double,std::milli>(finished-started).count();
            std::sort(heartbeat.gaps.begin(),heartbeat.gaps.end());REQUIRE_FALSE(heartbeat.gaps.empty());
            const auto p95=heartbeat.gaps[static_cast<std::size_t>(.95*(heartbeat.gaps.size()-1))];const auto maximum=heartbeat.gaps.back();
            CHECK(duration>=10000);CHECK(p95<=100);CHECK(maximum<=500);
            CHECK(logs->getText().getNumBytesAsUTF8()<=256*1024);CHECK_FALSE(status->status_text().contains("Completed run"));
            std::ofstream(root/"heartbeat.json")<<"{\"window_ms\":"<<duration<<",\"interval_ms\":20,\"samples\":"<<heartbeat.gaps.size()<<",\"p95_ms\":"<<p95<<",\"max_ms\":"<<maximum<<",\"ui_text_bytes\":"<<logs->getText().getNumBytesAsUTF8()<<"}";
        }
        std::ofstream(root / "live-ui-log.txt", std::ios::binary) << logs->getText().toStdString();
        click(workspace, "project.new"); CHECK(logs->getText().isEmpty());
    }
    if (failure_checks) {
        auto* logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs);
        const auto& locked = workspace.controller().project().nodes.back().managed_version; REQUIRE(locked);
        CHECK(logs->getText().contains(locked->content_sha256));
        audioalgo::NodeLogEvent late; late.node_id = custom; late.type_id = "user.pc02_print"; late.text = "must-not-leak";
        for (const auto& run : std::filesystem::directory_iterator(root / "runs")) {
            auto record = juce::JSON::parse(juce::File(run.path().generic_string()).getChildFile("run.json"));
            if (!logs->getText().contains(record["request_id"].toString())) continue;
            late.identity = {record["project_id"].toString().toStdString(), static_cast<std::uint64_t>(static_cast<juce::int64>(record["session_epoch"])),
                record["request_id"].toString().toStdString(), static_cast<std::uint64_t>(static_cast<juce::int64>(record["project_revision"]))};
        }
        const auto baseline = logs->getText();
        for (int field = 0; field < 4; ++field) {
            auto stale = late;
            if (field == 0) stale.identity.project_id += "other";
            if (field == 1) ++stale.identity.session_epoch;
            if (field == 2) stale.identity.request_id += "old";
            if (field == 3) ++stale.identity.revision;
            workspace.run_logs_received({stale}); CHECK(logs->getText() == baseline);
            workspace.run_log_received(std::move(stale)); CHECK(logs->getText() == baseline);
        }
        auto current_batch = late; current_batch.text = "accepted-current-batch";
        auto stale_before = current_batch; stale_before.identity.project_id += "other"; stale_before.text = "rejected-batch-project";
        auto stale_after = current_batch; stale_after.identity.request_id += "old"; stale_after.text = "rejected-batch-request";
        workspace.run_logs_received({stale_before, current_batch, stale_after});
        CHECK(logs->getText().contains("accepted-current-batch"));
        CHECK_FALSE(logs->getText().contains("rejected-batch-project"));
        CHECK_FALSE(logs->getText().contains("rejected-batch-request"));
        std::map<std::filesystem::path, juce::String> previous_outputs;
        for (const auto& run : std::filesystem::directory_iterator(root / "runs"))
            previous_outputs.emplace(run.path() / "output.wav", juce::SHA256(juce::File((run.path() / "output.wav").generic_string())).toHexString());
        auto* gain = control<juce::Slider>(workspace, "parameter.gain"); REQUIRE(gain);
        gain->setValue(-1, juce::sendNotificationSync); click(workspace, "project.run");
        auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        REQUIRE(pump([&] { return status->status_text().contains("pc03 failure after print"); }));
        CHECK_FALSE(status->status_text().contains("Completed run"));
        CHECK(logs->getText().contains("aaaaaaa")); CHECK(logs->getText().contains(custom));
        CHECK(logs->getText().contains("pc03 failure after print"));
        std::ofstream(root / "failed-ui-log.txt", std::ios::binary) << logs->getText().toStdString();
        for (const auto& [path, hash] : previous_outputs) CHECK(juce::SHA256(juce::File(path.generic_string())).toHexString() == hash);
        for (const auto& run : std::filesystem::directory_iterator(root / "runs")) if (!previous_outputs.contains(run.path() / "output.wav")) {
            CHECK_FALSE(std::filesystem::exists(run.path() / "complete.json"));
            CHECK_FALSE(std::filesystem::exists(run.path() / "output.wav"));
        }
        CHECK(std::filesystem::is_directory(root / "run-logs"));
        gain->setValue(0.5, juce::sendNotificationSync); click(workspace, "project.run");
        REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }));
        CHECK_FALSE(logs->getText().contains("must-not-leak")); CHECK(logs->getText().contains("aaaaaaa"));
        click(workspace, "project.new");
        CHECK(logs->getText().isEmpty()); workspace.run_log_received(late); CHECK(logs->getText().isEmpty());
    }
    for (const auto& run : std::filesystem::directory_iterator(root / "runs")) {
        if (!std::filesystem::is_regular_file(run.path() / "output.wav")) continue;
        auto wav = audioalgo::read_wav_file(run.path() / "output.wav"); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(wav));
        const auto& output = std::get<audioalgo::AudioBuffer>(wav); REQUIRE(output.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) CHECK(output.channel(0)[n] == input.channel(0)[n]);
    }
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

} // namespace
TEST_CASE("PC-02 production Print Apply library drop Canvas Run exposes only this run logs", "[PC-02][node_authoring][component]") { print_workspace_scenario(false); }
TEST_CASE("PC-03 production failure logs preserve exact version reject stale identities and recover", "[PC-03][node_authoring][component]") { print_workspace_scenario(true); }
TEST_CASE("PC-03 production check logs bind draft hashes and remain separate from Canvas Run", "[PC-03][node_authoring][component]") { print_workspace_scenario(false, true); }
TEST_CASE("PC-04 production Canvas exposes live node logs before termination and clears them on New", "[PC-04][node_authoring][component]") { print_workspace_scenario(false, false, true); }
TEST_CASE("PC-04.d production live log UI meets twenty ms heartbeat over ten seconds", "[PC-04.d][node_authoring][component]") { print_workspace_scenario(false, false, true, true); }
TEST_CASE("PC-04.d production filter separates live logs from two real node instances while preserving run identity", "[PC-04.d][node_authoring][component]") { print_workspace_scenario(false, false, false, false, true); }
TEST_CASE("PC-04.d production Run level filter separates real stdout and stderr without changing audio", "[PC-04.d][node_authoring][component]") { print_workspace_scenario(false, false, false, false, false, true); }
TEST_CASE("PC-04.d production historical Run logs do not replace current result or preview", "[PC-04.d][node_authoring][component]") { print_workspace_scenario(false, false, false, false, false, false, true); }
TEST_CASE("PC-04.a production log text joins partial stream chunks without invented newlines", "[PC-04.a][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel; panel.setSize(600,150);
    audioalgo::NodeLogEvent event;event.identity={"project",1,"request",2};event.node_id="node";event.source="stdout";event.level="info";
    event.text="part";panel.append(event);event.text="ial without newline";panel.append(event);
    auto* text=control<audioalgo::editor::RunLogTextView>(panel,"project.run_logs");REQUIRE(text);
    CHECK(text->getText().contains("partial without newline"));
}

TEST_CASE("PC-04.d production run log node filter preserves hidden current run entries", "[PC-04.d][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel; panel.setSize(720,240);
    auto* text=control<audioalgo::editor::RunLogTextView>(panel,"project.run_logs"); REQUIRE(text);
    auto* filter=control<juce::ComboBox>(panel,"project.run_logs.node_filter"); REQUIRE(filter);
    audioalgo::NodeLogEvent event; event.identity={"project",1,"request",2}; event.source="stdout"; event.level="info";
    event.node_id="first-node"; event.type_id="user.first"; event.text="first-only\n"; panel.append(event);
    event.node_id="second-node"; event.type_id="user.second"; event.text="second-before-filter\n"; panel.append(event);
    const int first_id=[&] { for (int i=0;i<filter->getNumItems();++i) if (filter->getItemText(i).contains("first-node")) return filter->getItemId(i); return 0; }();
    const int second_id=[&] { for (int i=0;i<filter->getNumItems();++i) if (filter->getItemText(i).contains("second-node")) return filter->getItemId(i); return 0; }();
    REQUIRE(first_id>1); REQUIRE(second_id>1); CHECK(first_id!=second_id);
    filter->setSelectedId(first_id,juce::sendNotificationSync);
    CHECK(text->getText().contains("first-only")); CHECK_FALSE(text->getText().contains("second-before-filter"));
    event.text="second-after-filter\n"; panel.append(event);
    CHECK_FALSE(text->getText().contains("second-after-filter"));
    filter->setSelectedId(second_id,juce::sendNotificationSync);
    CHECK_FALSE(text->getText().contains("first-only"));
    CHECK(text->getText().contains("second-before-filter")); CHECK(text->getText().contains("second-after-filter"));
    filter->setSelectedId(1,juce::sendNotificationSync);
    CHECK(text->getText().contains("first-only"));
    CHECK(text->getText().contains("second-before-filter")); CHECK(text->getText().contains("second-after-filter"));
}

TEST_CASE("PC-04.d log level and node filters intersect without discarding hidden entries", "[PC-04.d][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel;panel.setSize(720,240);
    auto* levels=control<juce::ComboBox>(panel,"project.run_logs.level_filter");REQUIRE(levels);
    auto* nodes=control<juce::ComboBox>(panel,"project.run_logs.node_filter");REQUIRE(nodes);
    auto* text=control<audioalgo::editor::RunLogTextView>(panel,"project.run_logs");REQUIRE(text);
    audioalgo::NodeLogEvent event;event.identity={"project",1,"request",2};event.source="sdk";
    for(const auto& node:{"first","second"})for(const auto& level:{"info","error"}){
        event.node_id=node;event.level=level;event.text=std::string(node)+"-"+level+"\n";panel.append(event);
    }
    auto select=[](juce::ComboBox& box,const juce::String& label){int id=0;for(int i=0;i<box.getNumItems();++i)if(box.getItemText(i)==label)id=box.getItemId(i);REQUIRE(id>1);box.setSelectedId(id,juce::sendNotificationSync);};
    select(*levels,"error");CHECK(text->getText().contains("first-error"));CHECK(text->getText().contains("second-error"));CHECK_FALSE(text->getText().contains("first-info"));
    select(*nodes,"first");CHECK(text->getText().contains("first-error"));CHECK_FALSE(text->getText().contains("second-error"));
    event.node_id="second";event.level="info";event.text="hidden-later-info\n";panel.append(event);CHECK_FALSE(text->getText().contains("hidden-later-info"));
    levels->setSelectedId(1,juce::sendNotificationSync);CHECK(text->getText().contains("first-info"));CHECK_FALSE(text->getText().contains("hidden-later-info"));
    nodes->setSelectedId(1,juce::sendNotificationSync);CHECK(text->getText().contains("hidden-later-info"));CHECK(text->getText().contains("second-error"));CHECK(text->getText().contains("request"));
    CHECK(panel.text_for_copy()==text->getText());
    panel.clear();CHECK(text->getText().isEmpty());CHECK(levels->getNumItems()==1);CHECK(levels->getSelectedId()==1);CHECK(nodes->getNumItems()==1);
}

TEST_CASE("PC-04.d history reader bounds owned tail and rejects mixed or malformed archives", "[PC-04.d][component]") {
    using namespace audioalgo::editor;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0407-log-history/reader"/juce::Uuid().toString().toStdString();
    std::cout<<"PC0407_READER_ROOT="<<root.generic_string()<<'\n';
    std::filesystem::create_directories(root/"valid");const auto file=root/"valid/events.jsonl";
    const auto row=[](int sequence,const std::string& request){return std::string("{\"project_id\":\"reader\",\"session_epoch\":1,\"request_id\":\"")+request+"\",\"revision\":7,\"node_id\":\"node\",\"type_id\":\"type\",\"package_id\":\"package\",\"content_sha256\":\"hash\",\"version\":\"1\",\"sequence\":"+std::to_string(sequence)+",\"received_unix_ms\":1,\"source\":\"stdout\",\"level\":\"info\",\"text\":\""+std::string(1024,'F')+"\",\"total_bytes\":1024,\"discarded_bytes\":0,\"dropped_text_bytes\":0,\"raw_log_file\":\"stdout.log\"}\n";};
    {std::ofstream output(file,std::ios::binary);for(int n=1;n<=7000;++n)output<<row(n,"request");REQUIRE(output.good());}
    const auto listing=std::async(std::launch::async,[&]{return RunLogArchiveReader::list(root,"reader",[]{return false;});}).get();
    REQUIRE(listing.runs.size()==1);const auto reference=listing.runs.front();
    const auto loaded=std::async(std::launch::async,[&]{return RunLogArchiveReader::load(reference,[]{return false;});}).get();
    REQUIRE_FALSE(loaded.events.empty());CHECK_FALSE(loaded.notice.empty());CHECK(loaded.events.front().sequence>1);CHECK(loaded.events.back().sequence==7000);
    std::size_t owned=0;for(const auto& event:loaded.events){owned+=RunLogMailbox::weight(event);CHECK(event.identity==reference.identity);}
    CHECK(owned<=256*1024);
    std::ofstream(root/"reader-result.json")<<"{\"owned_bytes\":"<<owned<<",\"first_sequence\":"<<loaded.events.front().sequence<<",\"last_sequence\":"<<loaded.events.back().sequence<<",\"events\":"<<loaded.events.size()<<"}";
    std::filesystem::create_directories(root/"mixed");std::filesystem::copy_file(file,root/"mixed/events.jsonl");
    {std::ofstream output(root/"mixed/events.jsonl",std::ios::binary|std::ios::app);output<<row(7001,"different-request");}
    auto mixed=reference;mixed.file=root/"mixed/events.jsonl";const auto rejected=RunLogArchiveReader::load(mixed,[]{return false;});CHECK(rejected.events.empty());CHECK(rejected.notice.find("identity")!=std::string::npos);
    std::filesystem::create_directories(root/"broken");{std::ofstream(root/"broken/events.jsonl")<<"{broken}\n";}
    auto broken=reference;broken.file=root/"broken/events.jsonl";const auto invalid=RunLogArchiveReader::load(broken,[]{return false;});CHECK(invalid.events.empty());CHECK_FALSE(invalid.notice.empty());
    CHECK(RunLogArchiveReader::load(reference,[]{return true;}).events.empty());
    const auto partial=RunLogArchiveReader::list(root,"reader",[]{return false;});CHECK_FALSE(partial.notice.empty());
}

TEST_CASE("PC-04 authoring malformed raw logs preserve byte counts without discard underflow", "[PC-04][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0403-ui-bound/artifacts" / juce::Uuid().toString().toStdString();
    auto draft = audioalgo::authoring::python_draft("malformed-log");
    draft.source = "import sys\ndef process(inputs, params, state):\n    sys.stdout.buffer.write(bytes([255,0,228])); sys.stdout.buffer.flush()\n    return {'audio_out': inputs['audio_in']}\n";
    audioalgo::authoring::PythonAuthoring checks(root);
    auto future = std::async(std::launch::async, [&] { return checks.check(draft, std::make_shared<std::atomic<bool>>(false)); });
    auto result = future.get();
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(result));
    const auto& logs = std::get<audioalgo::authoring::ReadyPackage>(result).logs; REQUIRE(logs.size() == 1);
    CHECK(logs[0].event.total_bytes == 15); CHECK(logs[0].event.discarded_bytes == 0);
    CHECK(logs[0].event.text.find("\xef\xbf\xbd") != std::string::npos); CHECK(logs[0].event.text.find("\xe2\x90\x80") != std::string::npos);
#else
    REQUIRE(std::holds_alternative<audioalgo::authoring::SourceDiagnostic>(result));
#endif
}

TEST_CASE("NA-07.b Apply publishes verified bytes atomically and failure retains the original catalog",
          "[NA-07.b][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0008-apply/transactions" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto draft = audioalgo::authoring::python_draft("atomic");
    std::optional<audioalgo::authoring::ApplyPhase> fail;
    audioalgo::authoring::PythonAuthoring service(root, {}, {}, [&](auto phase) { return !fail || *fail != phase; });
    auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    if (auto* e = std::get_if<audioalgo::authoring::SourceDiagnostic>(&checked)) INFO(e->code + ": " + e->message);
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    const auto& ready = std::get<audioalgo::authoring::ReadyPackage>(checked);
    const auto count = base->descriptors().size();
    for (auto phase : {audioalgo::authoring::ApplyPhase::package, audioalgo::authoring::ApplyPhase::registration,
                      audioalgo::authoring::ApplyPhase::index_write, audioalgo::authoring::ApplyPhase::index_verify,
                      audioalgo::authoring::ApplyPhase::index_replace}) {
        fail = phase;
        REQUIRE(std::holds_alternative<audioalgo::authoring::SourceDiagnostic>(service.apply(ready, draft, base, cancel)));
        CHECK_FALSE(std::filesystem::exists(root / "index.json")); CHECK(base->descriptors().size() == count);
        auto restored = service.restore_defaults(*base); REQUIRE(std::holds_alternative<audioalgo::NodeRegistry>(restored));
        CHECK(std::get<audioalgo::NodeRegistry>(restored).descriptors().size() == count);
    }
    fail.reset();
    auto result = service.apply(ready, draft, base, cancel);
    if (auto* e = std::get_if<audioalgo::authoring::SourceDiagnostic>(&result)) INFO(e->code + ": " + e->message);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(result));
    const auto& applied = std::get<audioalgo::authoring::AppliedPackage>(result);
    CHECK(base->descriptors().size() == count); CHECK(applied.catalog->descriptors().size() == count + 1);
    REQUIRE(applied.catalog->find(ready.descriptor.type_id));
    CHECK(applied.catalog->find(ready.descriptor.type_id)->plugin_origin->managed_content_sha256 == ready.package_sha256);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(service.apply(ready, draft, applied.catalog, cancel)));
    CHECK(std::distance(std::filesystem::directory_iterator(applied.package_root.parent_path()), std::filesystem::directory_iterator{}) == 1);
    auto restored = service.restore_defaults(*base); REQUIRE(std::holds_alternative<audioalgo::NodeRegistry>(restored));
    CHECK(std::get<audioalgo::NodeRegistry>(restored).find(ready.descriptor.type_id) != nullptr);
    auto changed = draft; changed.source += "\n# newer edit\n"; ++changed.generation;
    std::ifstream before(root / "index.json"); const std::string bytes(std::istreambuf_iterator<char>(before), {});
    REQUIRE(std::holds_alternative<audioalgo::authoring::SourceDiagnostic>(service.apply(ready, changed, applied.catalog, cancel)));
    std::ifstream after(root / "index.json"); CHECK(std::string(std::istreambuf_iterator<char>(after), {}) == bytes);
    std::ofstream(root / "receipt.json") << "{\"package_root\":" << juce::JSON::toString(juce::String(applied.package_root.generic_string())).toStdString()
        << ",\"source_sha256\":\"" << ready.source_sha256 << "\",\"content_sha256\":\"" << applied.content_sha256 << "\"}";
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-08.c queued production run owns its catalog before a directory refresh",
          "[NA-08.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0008-apply/catalog-lease" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto nodes = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*nodes));
    audioalgo::AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.1 * std::sin(n * 0.03));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.project_id = "catalog-lease"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, {"gain", "builtin.gain", {{"gain_db", 0.0}}}, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "gain", "audio_in"}, {"gain", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    juce::Component target;
    auto dispatch = std::make_shared<audioalgo::editor::ComponentDispatchState<juce::Component>>(target);
    audioalgo::editor::EditorWorkerPool pool(1);
    struct WaitJob final : juce::ThreadPoolJob {
        std::atomic<bool> entered{false}, release{false};
        WaitJob() : ThreadPoolJob("authoring catalog barrier") {}
        JobStatus runJob() override { entered = true; while (!release && !shouldExit()) juce::Thread::sleep(1); return jobHasFinished; }
    } barrier;
    REQUIRE(pool.add_job(&barrier, false)); REQUIRE(pump([&] { return barrier.entered.load(); }));
    std::optional<audioalgo::RunOutcome> result;
    audioalgo::editor::EditorRunQueue<juce::Component> queue(pool, dispatch, nodes, std::make_shared<audioalgo::ProjectResourceService>(),
        [&](juce::Component&, auto outcome, auto, auto) { result = std::move(outcome); });
    const audioalgo::RunIdentity identity{project.project_id, 1, "catalog-request", 0};
    audioalgo::RunRequest request{project, root / "project.aaproj", root / "run", 127}; request.identity = identity;
    const auto submitted = queue.submit(std::move(request), identity);
    *nodes = audioalgo::NodeRegistry{};
    barrier.release = true;
    REQUIRE(submitted); const bool finished = pump([&] { return result.has_value(); });
    bool drained = false; REQUIRE(pool.shutdown_async(5000, [&](bool) { drained = true; }));
    REQUIRE(pump([&] { return drained; })); pool.join_shutdown();
    REQUIRE(finished);
    if (auto* error = std::get_if<audioalgo::Diagnostic>(&*result)) INFO(error->code + ": " + error->message);
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(*result));
    const auto actual = audioalgo::read_wav_file(std::get<audioalgo::RunSuccess>(*result).output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(actual));
    for (std::size_t n = 0; n < 513; ++n) CHECK(std::abs(std::get<audioalgo::AudioBuffer>(actual).channel(0)[n] - input.channel(0)[n]) <= 2e-6 + 1e-5 * std::abs(input.channel(0)[n]));
}

TEST_CASE("NA-08.d fresh CLI restores the installed exact package and rejects a missing lock",
          "[NA-08.d][NA-09.a][NA-11.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0010-reopen-cli/artifacts" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    audioalgo::authoring::PythonAuthoring service(root / "authoring"); auto draft = audioalgo::authoring::python_draft("cli-reopen");
    auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto* check_error = std::get_if<audioalgo::authoring::SourceDiagnostic>(&checked);
    const std::string check_message = check_error ? check_error->code + ": " + check_error->message : "Ready";
    INFO(check_message);
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto applied = service.apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied));
    const auto& installed = std::get<audioalgo::authoring::AppliedPackage>(applied);
    const auto previous = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {}).toStdString();
    struct Restore { std::string value; ~Restore() { _putenv_s("AUDIOALGO_AUTHORING_ROOT", value.c_str()); } } restore{previous};
    REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT", (root / "authoring").string().c_str()) == 0);
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    juce::ChildProcess listing; REQUIRE(listing.start(juce::StringArray{executable.getFullPathName(), "--list-nodes"}));
    auto catalog_reader = std::async(std::launch::async, [&] { return listing.readAllProcessOutput(); });
    const auto listed = listing.waitForProcessToFinish(10000); if (!listed) listing.kill();
    const auto catalog = catalog_reader.get(); REQUIRE(listed);
    INFO(catalog.toStdString()); REQUIRE(listing.getExitCode() == 0);
    REQUIRE(catalog.contains("user.custom_node"));
    audioalgo::AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "cli-exact-reopen"; project.selected_output_node_id = "output";
    const auto instance = audioalgo::editor::make_node_instance(*installed.catalog, {}, "user.custom_node", "custom", 300, 100); REQUIRE(instance);
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}},
        *instance, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    auto run = [&](const char* name, bool success) {
        juce::ChildProcess child;
        REQUIRE(child.start(juce::StringArray{executable.getFullPathName(), "run", juce::String((root / "project.aaproj").generic_string()), juce::String((root / name).generic_string()), "127"}));
        auto reader = std::async(std::launch::async, [&] { return child.readAllProcessOutput(); });
        const auto finished = child.waitForProcessToFinish(10000); if (!finished) child.kill();
        const auto output = reader.get(); INFO(output.toStdString()); REQUIRE(finished);
        REQUIRE((child.getExitCode() == 0) == success);
        if (!success) { CHECK(output.contains("managed_version_unavailable")); CHECK_FALSE(std::filesystem::exists(root / name / "complete.json")); return; }
        auto wav = audioalgo::read_wav_file(root / name / "output.wav"); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(wav));
        const auto& audio = std::get<audioalgo::AudioBuffer>(wav); REQUIRE(audio.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) CHECK(std::abs(audio.channel(0)[n] - 0.1 * std::sin(0.03 * (n + 1))) <= 2e-6 + 1e-5 * std::abs(0.1 * std::sin(0.03 * (n + 1))));
    };
    run("v1", true);
    project.nodes[1].managed_version->content_sha256 = std::string(64, 'f');
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    run("missing", false);
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-11.d managed runtime rejects modified installed code before import and recovers exact bytes",
          "[NA-11.d][NA-07.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0011-runtime-integrity" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    audioalgo::authoring::PythonAuthoring service(root / "authoring"); const auto draft = audioalgo::authoring::python_draft("integrity");
    auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto result = service.apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(result));
    const auto& installed = std::get<audioalgo::authoring::AppliedPackage>(result);
    auto created = installed.catalog->create_processor("user.custom_node", "integrity-instance"); REQUIRE(created.processor);
    const auto marker = root / "must-not-import.txt";
    { std::ofstream f(installed.package_root / "user_source.py", std::ios::binary); f << "from pathlib import Path\nPath(" << juce::JSON::toString(juce::String(marker.generic_string())).toStdString() << ").write_text('imported')\n" << draft.source; REQUIRE(f.good()); }
    std::filesystem::create_directories(root / "run");
    audioalgo::NodeExecutionContext context; context.node_id = "integrity-instance"; context.output_directory = (root / "run").generic_string();
    context.frozen_task_packages = std::make_shared<std::map<std::string, std::string>>();
    const auto failed = created.processor->freeze_task(context);
    REQUIRE(failed.has_value()); CHECK(failed->code == "python_snapshot_modified"); CHECK_FALSE(std::filesystem::exists(marker));
    { std::ofstream f(installed.package_root / "user_source.py", std::ios::binary); f << draft.source; REQUIRE(f.good()); }
    REQUIRE_FALSE(created.processor->freeze_task(context).has_value());
    CHECK_FALSE(std::filesystem::exists(marker));
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-07.b prepared catalog does not publish before refresh acceptance and cancelled commit is invisible",
          "[NA-07.b][NA-07.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0012-refresh-transaction" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    audioalgo::authoring::PythonAuthoring service(root); const auto draft = audioalgo::authoring::python_draft("prepared");
    auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    const auto& ready = std::get<audioalgo::authoring::ReadyPackage>(checked);
    auto prepared = service.prepare_apply(ready, draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::PreparedPackage>(prepared));
    CHECK_FALSE(std::filesystem::exists(root / "index.json")); CHECK_FALSE(base->find("user.custom_node"));
    auto stage = std::get<audioalgo::authoring::PreparedPackage>(std::move(prepared)); REQUIRE(stage.package.catalog->find("user.custom_node"));
    cancel->store(true);
    REQUIRE(std::holds_alternative<audioalgo::authoring::SourceDiagnostic>(stage.transaction->commit(cancel)));
    CHECK_FALSE(std::filesystem::exists(root / "index.json"));
    cancel->store(false); prepared = service.prepare_apply(ready, draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::PreparedPackage>(prepared));
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(std::get<audioalgo::authoring::PreparedPackage>(prepared).transaction->commit(cancel)));
    CHECK(std::filesystem::is_regular_file(root / "index.json")); CHECK_FALSE(base->find("user.custom_node"));
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-07.b production library preparation failure preserves index catalog and graph then retries",
          "[NA-07.b][NA-08.b][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0012-refresh-transaction/ui" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    bool allow = false; int preparations = 0;
    audioalgo::editor::EditorComponent workspace(base, std::make_shared<Documents>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, service, [&] { ++preparations; return allow; });
    const auto count = base->descriptors().size(); const auto revision = workspace.controller().project().revision;
    click(workspace, "authoring.new"); click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    const auto type = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel")->ready()->descriptor.type_id;
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return !control<juce::Label>(workspace, "authoring.check_status")->getText().contains("Applying"); }));
    CHECK(preparations == 1); CHECK_FALSE(std::filesystem::exists(root / "index.json")); CHECK(base->descriptors().size() == count);
    CHECK_FALSE(control<juce::TextButton>(workspace, "library." + juce::String(type)));
    CHECK(workspace.controller().project().revision == revision); CHECK_FALSE(workspace.controller().dirty()); CHECK_FALSE(workspace.controller().history().can_undo());
    CHECK(control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("authoring_catalog_refresh_failed"));
    allow = true; click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library." + juce::String(type)) != nullptr; }));
    CHECK(preparations == 2); CHECK(std::filesystem::exists(root / "index.json")); CHECK(base->descriptors().size() == count + 1);
    CHECK(workspace.controller().project().revision == revision); CHECK_FALSE(workspace.controller().dirty());
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

TEST_CASE("NA-02.a production name and type fields edit one declaration and new nodes have independent identities",
          "[NA-02.a][NA-02.b][NA-03.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0013-definition-fields" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    audioalgo::editor::EditorComponent workspace(base, std::make_shared<Documents>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, service);
    const auto revision = workspace.controller().project().revision;
    click(workspace, "authoring.new");
    auto* type = control<juce::TextEditor>(workspace, "authoring.type_id"); REQUIRE(type);
    auto* name = control<juce::TextEditor>(workspace, "authoring.display_name"); REQUIRE(name);
    const auto first_type = type->getText(); CHECK(first_type.isNotEmpty());
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
    const auto source = panel->session().document().source;
    type->setText("user.named_transform", true); name->setText(juce::String::fromUTF8("中文 自定义增益"), true);
    REQUIRE(pump([&] { return juce::JSON::parse(juce::String(panel->session().document().declaration))["type_id"].toString() == "user.named_transform"; }));
    CHECK(juce::JSON::parse(juce::String(panel->session().document().declaration))["display_name"].toString() == juce::String::fromUTF8("中文 自定义增益"));
    CHECK(panel->session().document().source == source);
    click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    CHECK(panel->ready()->descriptor.type_id == "user.named_transform");
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library.user.named_transform") != nullptr; }));
    CHECK(control<juce::TextButton>(workspace, "library.user.named_transform")->getButtonText() == juce::String::fromUTF8("中文 自定义增益"));
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, "authoring.new");
    type = control<juce::TextEditor>(workspace, "authoring.type_id"); REQUIRE(type);
    CHECK(type->getText() != first_type); CHECK(type->getText() != "user.named_transform");
    CHECK(workspace.controller().project().nodes.empty()); CHECK(workspace.controller().project().revision == revision);
    CHECK_FALSE(workspace.controller().dirty()); CHECK_FALSE(workspace.controller().history().can_undo());
}

TEST_CASE("NA-03.b production stateful template keeps frame state across probe blocks and resets on rerun",
          "[NA-03.b][NA-02.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0014-stateful-template" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, service);
    click(workspace, "authoring.new");
    auto* actual_panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(actual_panel);
    auto& panel = *actual_panel;
    auto* templates = control<juce::ComboBox>(panel, "authoring.template"); REQUIRE(templates);
    templates->setSelectedId(2, juce::sendNotificationSync);
    const auto definition = juce::JSON::parse(juce::String(panel.session().document().declaration));
    REQUIRE(static_cast<bool>(definition["execution"]["stateful"]));
    std::string previous_request;
    for (int iteration = 0; iteration < 2; ++iteration) {
        click(panel, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
        const auto checked_ok = pump([&] { return panel.ready().has_value(); });
        INFO(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString());
        REQUIRE(checked_ok);
        const auto& ready = *panel.ready(); CHECK(ready.request_id != previous_request); previous_request = ready.request_id;
        const auto& audio = ready.probe_outputs.at("audio_out"); REQUIRE(audio.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) {
            const auto expected = 0.1 * std::sin(0.03 * (n + 1)) + n * 1e-5;
            CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
        }
        const auto receipt_root = root / (iteration == 0 ? "first" : "second"); std::filesystem::create_directories(receipt_root);
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
        receipt->setProperty("package_root", juce::String(ready.package_root.generic_string()));
        receipt->setProperty("probe_directory", juce::String(ready.probe_directory.generic_string()));
        receipt->setProperty("source_sha256", juce::String(ready.source_sha256)); receipt->setProperty("package_sha256", juce::String(ready.package_sha256));
        receipt->setProperty("syntax_pid", static_cast<juce::int64>(ready.syntax_pid)); receipt->setProperty("probe_pid", static_cast<juce::int64>(ready.probe_pid));
        std::ofstream(receipt_root / "receipt.json") << juce::JSON::toString(juce::var(receipt.get()), false).toStdString();
#else
        REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
    }
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto type = panel.ready()->descriptor.type_id;
    click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library." + juce::String(type)) != nullptr; }));
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, ("library." + type).c_str());
    auto* step = control<juce::Slider>(workspace, "parameter.step"); REQUIRE(step);
    CHECK(std::abs(step->getValue() - 1e-5) <= 1e-15);
    step->setValue(2e-5, juce::sendNotificationSync);
    CHECK(std::abs(step->getValue() - 2e-5) <= 1e-15);
    CHECK(std::get<double>(workspace.controller().project().nodes.back().parameters.at("step")) == step->getValue());
#endif
}

TEST_CASE("NA-03.c production named template generates both input and output contracts and probes both outputs",
          "[NA-03.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0015-named-template" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    audioalgo::editor::NodeAuthoringPanel panel(audioalgo::authoring::python_draft("named"), false, storage, [] {}, service);
    auto* templates = control<juce::ComboBox>(panel, "authoring.template"); REQUIRE(templates); REQUIRE(templates->getNumItems() >= 3);
    templates->setSelectedId(3, juce::sendNotificationSync);
    const auto definition = juce::JSON::parse(juce::String(panel.session().document().declaration));
    REQUIRE(definition["inputs"].getArray()); REQUIRE(definition["outputs"].getArray());
    CHECK(definition["inputs"].getArray()->size() == 2); CHECK(definition["outputs"].getArray()->size() == 2);
    click(panel, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto finished = pump([&] { return panel.ready().has_value(); });
    INFO(control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().toStdString()); REQUIRE(finished);
    const auto& ready = *panel.ready(); REQUIRE(ready.probe_outputs.size() == 2);
    REQUIRE(ready.probe_outputs.contains("main")); REQUIRE(ready.probe_outputs.contains("side"));
    CHECK(ready.descriptor.ports.size() == 4);
    for (const auto& port : {std::string("main"), std::string("side")}) {
        const auto& audio = ready.probe_outputs.at(port); REQUIRE(audio.frame_count() == 513);
        CHECK(audio.sample_rate_hz() == 16000); CHECK(audio.channel_ids() == std::vector<std::string>{"authoring-channel-0"});
        for (std::size_t n = 0; n < 513; ++n) {
            const auto a = 0.2 * std::sin(0.03 * (n + 1)), b = 0.2 * std::sin(0.03 * (n + 1) + 1);
            const auto expected = port == "main" ? (2 * a + 3 * b) * 0.5 : a - b;
            CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
        }
    }
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("package_root", juce::String(ready.package_root.generic_string())); receipt->setProperty("probe_directory", juce::String(ready.probe_directory.generic_string()));
    receipt->setProperty("source_sha256", juce::String(ready.source_sha256)); receipt->setProperty("package_sha256", juce::String(ready.package_sha256));
    std::ofstream(root / "receipt.json") << juce::JSON::toString(juce::var(receipt.get()), false).toStdString();
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(panel, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

TEST_CASE("NA-03.c applied named package runs two independent WAV sources and selects each exact output",
          "[NA-03.c][NA-08.a][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0015-named-template/project-run" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    audioalgo::authoring::PythonAuthoring service(root / "authoring"); const auto draft = audioalgo::authoring::python_draft("named-runtime", audioalgo::authoring::PythonTemplate::named_router);
    auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    const auto* check_error = std::get_if<audioalgo::authoring::SourceDiagnostic>(&checked);
    const std::string check_message = check_error ? check_error->code + ": " + check_error->message : "Ready"; INFO(check_message);
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto applied = service.apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied));
    auto catalog = std::get<audioalgo::authoring::AppliedPackage>(applied).catalog;
    for (int index = 0; index < 2; ++index) {
        audioalgo::AudioBuffer input(16000, {"ch0"}, 513);
        for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1) + index));
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / (index == 0 ? "primary.wav" : "reference.wav"), input, audioalgo::WavEncoding::float32)));
    }
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "named-source-routing";
    const auto instance = audioalgo::editor::make_node_instance(*catalog, {}, "user.custom_node", "custom", 340, 100); REQUIRE(instance);
    project.nodes = {{"primary", "builtin.file_source", {{"path", std::string("primary.wav")}}, 40, 100},
        {"reference", "builtin.file_source", {{"path", std::string("reference.wav")}}, 40, 300}, *instance};
    project.connections = {{"primary", "audio", "custom", "primary"}, {"reference", "audio", "custom", "reference"}};
    audioalgo::editor::EditorController controller(*catalog); controller.adopt_project(project);
    for (const auto& port : {std::string("main"), std::string("side")}) {
        REQUIRE(controller.set_selected_output_port("custom", port).status == audioalgo::editor::ProjectEditStatus::applied);
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", controller.project())));
        auto worker = std::async(std::launch::async, [catalog, project = controller.project(), root, port] {
            return audioalgo::RunService(catalog).run({project, root / "project.aaproj", root / port, 127});
        });
        REQUIRE(pump([&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
        const auto result = worker.get(); const auto* error = std::get_if<audioalgo::Diagnostic>(&result);
        const std::string message = error ? error->code + ": " + error->message : "RunSuccess"; INFO(message);
        REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(result));
        auto wav = audioalgo::read_wav_file(std::get<audioalgo::RunSuccess>(result).output_file); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(wav));
        const auto& audio = std::get<audioalgo::AudioBuffer>(wav); REQUIRE(audio.frame_count() == 513);
        CHECK(audio.sample_rate_hz() == 16000); CHECK(audio.channel_ids() == std::vector<std::string>{"ch0"});
        CHECK(audio.valid_interval() == audioalgo::AudioInterval{0, 513});
        for (std::size_t n = 0; n < 513; ++n) {
            const auto a = 0.2 * std::sin(0.03 * (n + 1)), b = 0.2 * std::sin(0.03 * (n + 1) + 1);
            const auto expected = port == "main" ? (2 * a + 3 * b) * 0.5 : a - b;
            CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
        }
    }
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-09.a Apply v2 publishes a new default while an owned v1 catalog keeps its exact old code",
          "[NA-09.a][NA-07.b][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0016-version-default" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    audioalgo::authoring::PythonAuthoring service(root); auto draft = audioalgo::authoring::python_draft("versions");
    auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service.check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto initial = service.apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(initial));
    const auto old = std::get<audioalgo::authoring::AppliedPackage>(initial);
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    auto project_for = [&](const auto& catalog, const char* id) {
        audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = id; project.selected_output_node_id = "output";
        const auto instance = audioalgo::editor::make_node_instance(*catalog, {}, "user.custom_node", "custom", 340, 100); REQUIRE(instance);
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, *instance, {"output", "builtin.output", {}}};
        project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}}; return project;
    };
    const auto old_project = project_for(old.catalog, "locked-v1");
    auto run = [&](const auto& catalog, audioalgo::ProjectModel project, const char* name, double amplitude) {
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / (std::string(name) + ".aaproj"), project)));
        auto worker = std::async(std::launch::async, [catalog, project = std::move(project), root, name] {
            return audioalgo::RunService(catalog).run({project, root / (std::string(name) + ".aaproj"), root / name, 127});
        });
        REQUIRE(pump([&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
        const auto result = worker.get(); const auto* error = std::get_if<audioalgo::Diagnostic>(&result);
        const std::string message = error ? error->code + ": " + error->message : "RunSuccess"; INFO(message);
        REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(result));
        const auto read = audioalgo::read_wav_file(std::get<audioalgo::RunSuccess>(result).output_file); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
        const auto& audio = std::get<audioalgo::AudioBuffer>(read); REQUIRE(audio.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) {
            const auto expected = amplitude * std::sin(0.03 * (n + 1));
            CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
        }
    };
    run(old.catalog, old_project, "v1-before", 0.1);
    auto bytes = [](const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    const auto old_source = bytes(old.package_root / "user_source.py"), old_wav = bytes(root / "v1-before/output.wav");
    draft.source = "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n"; ++draft.generation;
    checked = service.check(draft, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto updated = service.apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, old.catalog, cancel);
    const auto* apply_error = std::get_if<audioalgo::authoring::SourceDiagnostic>(&updated);
    const std::string apply_message = apply_error ? apply_error->code + ": " + apply_error->message : "Applied"; INFO(apply_message);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(updated));
    const auto current = std::get<audioalgo::authoring::AppliedPackage>(updated);
    CHECK(current.package_id == old.package_id); CHECK(current.content_sha256 != old.content_sha256);
    CHECK(old.catalog->find("user.custom_node")->plugin_origin->managed_content_sha256 == old.content_sha256);
    auto restored = service.restore_defaults(*base); REQUIRE(std::holds_alternative<audioalgo::NodeRegistry>(restored));
    CHECK(std::get<audioalgo::NodeRegistry>(restored).find("user.custom_node")->plugin_origin->managed_content_sha256 == current.content_sha256);
    run(current.catalog, project_for(current.catalog, "new-default-v2"), "v2", 0.15);
    run(old.catalog, old_project, "v1-after", 0.1);
    CHECK(bytes(old.package_root / "user_source.py") == old_source); CHECK(bytes(root / "v1-before/output.wav") == old_wav);
    CHECK(bytes(root / "v1-after/output.wav") == old_wav);
    CHECK(std::distance(std::filesystem::directory_iterator(old.package_root.parent_path()), std::filesystem::directory_iterator{}) == 2);
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-09.a production Apply v2 leaves the saved graph on v1 and a new graph selects v2",
          "[NA-09.a][NA-08.b][NA-08.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0017-project-catalog" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.project_id = "saved-v1"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, {"output", "builtin.output", {}, 640, 100}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts");
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root);
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, service);
    workspace.open_project_file(root / "project.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "saved-v1"; }));
    click(workspace, "authoring.new"); click(workspace, "authoring.check");
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    const auto type = panel->ready()->descriptor.type_id, v1 = panel->ready()->package_sha256;
    click(workspace, "authoring.apply"); REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "library." + juce::String(type)) != nullptr; }));
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, ("library." + type).c_str()); const auto custom = workspace.controller().project().nodes.back().id;
    control<juce::Slider>(workspace, "parameter.gain")->setValue(0.8, juce::sendNotificationSync);
    wire_workspace(workspace, *registry, "file", "audio", custom, "audio_in"); wire_workspace(workspace, *registry, custom, "audio_out", "output", "audio");
    REQUIRE(workspace.controller().project().connections.size() == 2);
    click(workspace, "project.save"); REQUIRE(pump([&] { return !workspace.controller().dirty(); }));
    std::set<std::filesystem::path> seen;
    auto run_v1 = [&] {
        click(workspace, "project.run"); auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status");
        const auto finished = pump([&] { return status->status_text().contains("Completed run"); }); INFO(status->status_text().toStdString()); REQUIRE(finished);
        std::vector<std::filesystem::path> added;
        for (const auto& run : std::filesystem::directory_iterator(root / "runs"))
            if (std::filesystem::is_regular_file(run.path() / "output.wav") && !seen.contains(run.path() / "output.wav")) added.push_back(run.path() / "output.wav");
        REQUIRE(added.size() == 1); seen.insert(added.front());
        auto read = audioalgo::read_wav_file(added.front()); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
        const auto& audio = std::get<audioalgo::AudioBuffer>(read); REQUIRE(audio.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) { const auto expected = 0.16 * std::sin(0.03 * (n + 1)); CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected)); }
    };
    run_v1(); const auto generation = workspace.controller().document_generation(); const auto revision = workspace.controller().project().revision;
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "before-v2.aaproj", workspace.controller().project())));
    click(workspace, "authoring.open"); REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") != nullptr; }));
    auto* source = control<juce::CodeEditorComponent>(workspace, "authoring.source"); REQUIRE(source);
    source->selectAll(); source->insertTextAtCaret("def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n");
    click(workspace, "authoring.check"); REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); const auto v2 = panel->ready()->package_sha256; CHECK(v1 != v2);
    click(workspace, "authoring.apply"); REQUIRE(pump([&] { return control<juce::Label>(workspace, "authoring.check_status")->getText().contains("Available"); }));
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "after-v2.aaproj", workspace.controller().project())));
    auto bytes = [](const auto& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    CHECK(bytes(root / "before-v2.aaproj") == bytes(root / "after-v2.aaproj"));
    CHECK(workspace.controller().document_generation() == generation); CHECK(workspace.controller().project().revision == revision); CHECK_FALSE(workspace.controller().dirty());
    REQUIRE(registry->find(type)->plugin_origin->managed_content_sha256 == v1);
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    run_v1();
    click(workspace, "project.new"); REQUIRE(workspace.controller().project().nodes.empty());
    click(workspace, ("library." + type).c_str()); REQUIRE(workspace.controller().project().nodes.size() == 1);
    REQUIRE(workspace.controller().project().nodes.front().managed_version); CHECK(workspace.controller().project().nodes.front().managed_version->content_sha256 == v2);
#else
    REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.diagnostic")->getButtonText().contains("python_disabled"); }));
#endif
}

namespace {
void log_versions_scenario(bool active_apply) {
#if !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Python-enabled production Run");
#else
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / (active_apply ? "build/reports/loops/PC0308-active-version/artifacts" : "build/reports/loops/PC0307-log-versions/artifacts") / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto base = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*base));
    auto service = std::make_shared<PythonAuthoring>(root / "library");
    auto draft = python_draft("pc03-version-identity");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::vector<AppliedPackage> versions;
    std::optional<ReadyPackage> pending_v2;
    const auto gate = root / "run-gate.txt";
    const auto gate_literal = juce::JSON::toString(juce::var(juce::String(gate.generic_string())), true).toStdString();
    for (int version = 1; version <= 2; ++version) {
        draft.source = "def process(inputs, params, state):\n    print('pc03-v" + std::to_string(version) + " gain=' + str(params['gain']), flush=True)\n    return {'audio_out': inputs['audio_in'] * params['gain'] * " + std::to_string(version) + "}\n";
        if (active_apply && version == 1)
            draft.source = "import pathlib\nimport time\n" + draft.source.substr(0, draft.source.find("    print(")) +
                "    gate = pathlib.Path(" + gate_literal + ")\n    if gate.exists():\n        gate.with_name('run-entered.txt').write_text('entered', encoding='ascii')\n        while gate.exists():\n            time.sleep(.005)\n" + draft.source.substr(draft.source.find("    print("));
        ++draft.generation;
        const auto checked = service->check(draft, cancel);
        REQUIRE(std::holds_alternative<ReadyPackage>(checked));
        if (active_apply && version == 2) { pending_v2 = std::get<ReadyPackage>(checked); break; }
        const auto applied = service->apply(std::get<ReadyPackage>(checked), draft, versions.empty() ? base : versions.back().catalog, cancel);
        REQUIRE(std::holds_alternative<AppliedPackage>(applied)); versions.push_back(std::get<AppliedPackage>(applied));
    }
    if (!active_apply) REQUIRE(versions[0].content_sha256 != versions[1].content_sha256);
    AudioBuffer input(16000, {"ch0"}, 513);
    for (std::size_t n = 0; n < input.frame_count(); ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root / "input.wav", input, WavEncoding::float32)));
    const auto save_version_project = [&](int index) {
        ProjectModel project; project.schema_version = 2; project.project_id = "pc03-project-v" + std::to_string(index + 1);
        project.revision = 7; project.selected_output_node_id = "output";
        auto first = editor::make_node_instance(*versions[index].catalog, {}, "user.custom_node", "first", 240, 100);
        auto second = editor::make_node_instance(*versions[index].catalog, {}, "user.custom_node", "second", 440, 100);
        REQUIRE(first); REQUIRE(second); first->parameters["gain"] = .5; second->parameters["gain"] = .25;
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, *first, *second, {"output", "builtin.output", {}, 640, 100}};
        project.connections = {{"file", "audio", "first", "audio_in"}, {"first", "audio_out", "second", "audio_in"}, {"second", "audio_out", "output", "audio"}};
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root / ("v" + std::to_string(index + 1) + ".aaproj"), project)));
    };
    save_version_project(0);
    if (!active_apply) save_version_project(1);
    auto registry = std::make_shared<NodeRegistry>(*versions.back().catalog);
    editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<ProjectResourceService>(),
        std::make_shared<Preview>(), std::make_shared<FileDraftStorage>(root / "drafts"), false, service);
    std::set<std::filesystem::path> seen;
    std::optional<NodeLogEvent> old_log;
    juce::Array<juce::var> receipts;
    for (int pass = 0; pass < 3; ++pass) {
        const int index = pass == 1 ? 1 : 0;
        const auto project_file = root / ("v" + std::to_string(index + 1) + ".aaproj");
        workspace.open_project_file(project_file);
        REQUIRE(pump([&] { return workspace.controller().project().project_id == "pc03-project-v" + std::to_string(index + 1); }));
        REQUIRE(workspace.controller().project().revision == 7);
        auto* logs = control<audioalgo::editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs); CHECK(logs->getText().isEmpty());
        if (old_log) { workspace.run_log_received(*old_log); CHECK(logs->getText().isEmpty()); }
        struct RestoreOriginal final {
            std::filesystem::path path, gate;
            std::string bytes;
            ~RestoreOriginal() {
                if (!path.empty()) { std::ofstream restore(path, std::ios::binary); restore << bytes; }
                if (!gate.empty()) { std::error_code ec; std::filesystem::remove(gate, ec); }
            }
        } restore;
        if (active_apply && pass == 0) { std::ofstream(gate) << "hold"; restore.gate = gate; }
        click(workspace, "project.run");
        if (active_apply && pass == 0) {
            REQUIRE(pump([&] { return std::filesystem::is_regular_file(root / "run-entered.txt"); }, 15000));
            REQUIRE(pending_v2);
            const auto applied = service->apply(*pending_v2, draft, versions[0].catalog, cancel);
            REQUIRE(std::holds_alternative<AppliedPackage>(applied)); versions.push_back(std::get<AppliedPackage>(applied));
            REQUIRE(versions[0].content_sha256 != versions[1].content_sha256);
            save_version_project(1);
            restore.path = std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(versions[0].package_root / "user_source.py").wstring());
            { std::ifstream original(restore.path, std::ios::binary); REQUIRE(original); restore.bytes.assign(std::istreambuf_iterator<char>(original), {}); }
            const std::string changed = "raise AssertionError('installed source changed after freeze')\n";
            { std::ofstream modified(restore.path, std::ios::binary); modified << changed; modified.close(); REQUIRE(modified); }
            { std::ifstream observed(restore.path, std::ios::binary); REQUIRE(observed);
              REQUIRE(std::string(std::istreambuf_iterator<char>(observed), {}) == changed); }
            std::ofstream(root / "mutation-receipt.txt") << versions[0].content_sha256 << '\n'
                << juce::SHA256(restore.bytes.data(), restore.bytes.size()).toHexString() << '\n'
                << juce::SHA256(changed.data(), changed.size()).toHexString() << '\n';
            REQUIRE(std::filesystem::remove(gate));
        }
        auto* status = control<editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }, 15000));
        const auto text = logs->getText();
        CHECK(text.contains("node=first")); CHECK(text.contains("node=second")); CHECK(text.contains(versions[index].content_sha256));
        CHECK_FALSE(text.contains(versions[1-index].content_sha256));
        CHECK(text.contains("pc03-v" + juce::String(index+1) + " gain=0.5"));
        CHECK(text.contains("pc03-v" + juce::String(index+1) + " gain=0.25"));
        std::filesystem::path run;
        for (const auto& entry : std::filesystem::directory_iterator(root / "runs"))
            if (std::filesystem::is_regular_file(entry.path() / "run.json") && !seen.contains(entry.path())) { REQUIRE(run.empty()); run = entry.path(); }
        REQUIRE_FALSE(run.empty()); seen.insert(run);
        const auto record = juce::JSON::parse(juce::File((run / "run.json").generic_string()));
        const auto request_id = record["request_id"].toString(); CHECK(text.contains(request_id));
        const auto audio = read_wav_file(run / "output.wav"); REQUIRE(std::holds_alternative<AudioBuffer>(audio));
        const auto& output = std::get<AudioBuffer>(audio); REQUIRE(output.frame_count() == 513); CHECK(output.channel_ids() == input.channel_ids());
        for (std::size_t n = 0; n < 513; ++n) {
            const float stage = input.channel(0)[n] * .5f * static_cast<float>(index+1);
            const float expected = stage * .25f * static_cast<float>(index+1);
            CHECK(std::abs(output.channel(0)[n]-expected) <= 2e-6 + 1e-5*std::abs(expected));
        }
        const auto ui_file = "ui-" + std::to_string(pass) + ".log";
        std::ofstream(root / ui_file, std::ios::binary) << text.toStdString();
        if (old_log) { workspace.run_log_received(*old_log); CHECK(logs->getText() == text); }
        NodeLogEvent late; late.identity = {record["project_id"].toString().toStdString(),
            static_cast<std::uint64_t>(static_cast<juce::int64>(record["session_epoch"])), request_id.toStdString(), 7};
        late.node_id = "first"; late.content_sha256 = versions[index].content_sha256; late.text = "late-old-project"; old_log = late;
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
        receipt->setProperty("project_file", juce::String(project_file.filename().string())); receipt->setProperty("run_directory", juce::String(run.lexically_relative(root).generic_string()));
        receipt->setProperty("request_id", request_id); receipt->setProperty("content_sha256", juce::String(versions[index].content_sha256));
        receipt->setProperty("version", index + 1); receipt->setProperty("ui_log", juce::String(ui_file)); receipts.add(juce::var(receipt.get()));
    }
    std::ofstream(root / "identity-demo.json") << juce::JSON::toString(receipts, false).toStdString();
    int drained = 0; workspace.begin_shutdown([&] { ++drained; }); REQUIRE(pump([&] { return drained == 1; }));
#endif
}

}
TEST_CASE("PC-03.d two versions and two instances keep exact log identity across same revision projects",
          "[PC-03.d][node_authoring][component]") { log_versions_scenario(false); }
TEST_CASE("PC-03.d active v1 Run retains frozen code and logs while v2 Apply changes the default",
          "[PC-03.d][node_authoring][component]") { log_versions_scenario(true); }

TEST_CASE("PC-03.a slow production Run cannot pollute a new project revision or replacement Run",
          "[PC-03.a][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled production Run");
#else
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    juce::Array<juce::var> artifact_roots;
    for (int mode = 0; mode < 3; ++mode) {
        INFO(mode);
        const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PC0309-live-identity/artifacts" / juce::Uuid().toString().toStdString();
        std::filesystem::create_directories(root);
        const auto gate = root / "gate.txt", marker = root / "entered.txt";
        auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
        auto service = std::make_shared<PythonAuthoring>(root / "library");
        auto draft = python_draft("pc03-live-identity");
        auto definition = juce::JSON::parse(draft.declaration);
        auto* parameters = definition["parameters"].getArray(); REQUIRE(parameters); REQUIRE(parameters->size() == 1);
        (*parameters)[0].getDynamicObject()->setProperty("step", .01);
        draft.declaration = juce::JSON::toString(definition, false).toStdString();
        const auto literal = juce::JSON::toString(juce::var(juce::String(gate.generic_string())), true).toStdString();
        draft.source = "import os\nimport pathlib\nimport time\ndef process(inputs, params, state):\n    print('pc03-live gain=' + str(params['gain']), flush=True)\n    gate = pathlib.Path(" + literal + ")\n    if params['gain'] == .5 and gate.exists():\n        gate.with_name('entered.txt').write_text(str(os.getpid()), encoding='ascii')\n        while gate.exists():\n            time.sleep(.005)\n    return {'audio_out': inputs['audio_in'] * params['gain']}\n";
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        const auto checked = service->check(draft, cancel); REQUIRE(std::holds_alternative<ReadyPackage>(checked));
        const auto applied = service->apply(std::get<ReadyPackage>(checked), draft, registry, cancel); REQUIRE(std::holds_alternative<AppliedPackage>(applied));
        const auto installed = std::get<AppliedPackage>(applied); *registry = *installed.catalog;
        AudioBuffer input(16000, {"ch0"}, 513);
        for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03*(n+1)));
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav", input, WavEncoding::float32)));
        for (int index = 0; index < 2; ++index) {
            ProjectModel project; project.schema_version=2; project.project_id=index==0 ? "live-A" : "live-B"; project.revision=7; project.selected_output_node_id="output";
            auto custom=editor::make_node_instance(*registry, {}, "user.custom_node", "custom", 240, 100); REQUIRE(custom);
            custom->parameters["gain"]=index==0 ? .5 : .25;
            project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},40,100},*custom,{"output","builtin.output",{},640,100}};
            project.connections={{"file","audio","custom","audio_in"},{"custom","audio_out","output","audio"}};
            REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/(index==0 ? "A.aaproj" : "B.aaproj"),project)));
        }
        struct TrackedPreview final : AudioPreviewService {
            std::filesystem::path played;
            AudioPreviewOutcome play(const std::filesystem::path& path) override { played=path;return std::monostate{}; }
            void pause() override {} void stop() override {} void shutdown() override {}
            bool is_playing() const noexcept override { return false; }
            double position_seconds() const noexcept override { return 0; }
        };
        auto preview = std::make_shared<TrackedPreview>();
        editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),preview,
            std::make_shared<FileDraftStorage>(root/"drafts"),false,service);
        workspace.open_project_file(root/"A.aaproj"); REQUIRE(pump([&]{return workspace.controller().project().project_id=="live-A";}));
        struct ReleaseGate { std::filesystem::path path; ~ReleaseGate(){std::error_code ec;std::filesystem::remove(path,ec);} } release{gate};
        {std::ofstream(gate)<<"hold";}
        click(workspace,"project.run");
        DWORD old_pid=0;
        REQUIRE(pump([&]{std::ifstream in(marker);return static_cast<bool>(in>>old_pid) && old_pid!=0;},15000));
        struct ProcessHandle { HANDLE value=nullptr; ~ProcessHandle(){if(value)CloseHandle(value);} } process;
        process.value=OpenProcess(SYNCHRONIZE,FALSE,old_pid); REQUIRE(process.value); CHECK(WaitForSingleObject(process.value,0)==WAIT_TIMEOUT);
        juce::var archived;
        REQUIRE(pump([&]{
            for(const auto& dir:std::filesystem::directory_iterator(root/"run-logs")) {
                std::ifstream in(dir.path()/"events.jsonl");std::string line;
                while(std::getline(in,line)){auto value=juce::JSON::parse(line);if(value["text"].toString().contains("pc03-live")){archived=value;return true;}}
            }
            return false;
        }));
        NodeLogEvent old; old.identity={archived["project_id"].toString().toStdString(),static_cast<std::uint64_t>(static_cast<juce::int64>(archived["session_epoch"])),
            archived["request_id"].toString().toStdString(),7}; old.node_id="custom";old.text="late-A-must-not-leak";
        std::ofstream(root/"old-event.json")<<juce::JSON::toString(archived,false).toStdString();
        auto* logs=control<audioalgo::editor::RunLogTextView>(workspace,"project.run_logs"); REQUIRE(logs);
        if(mode==0){
            workspace.open_project_file(root/"B.aaproj"); REQUIRE(pump([&]{return workspace.controller().project().project_id=="live-B";}));
            CHECK(workspace.controller().revision()==7);CHECK(logs->getText().isEmpty());
        } else if(mode==1) {
            auto* canvas=control<editor::GraphCanvas>(workspace,"project.canvas");REQUIRE(canvas);canvas->select("custom");
            auto* gain=control<juce::Slider>(workspace,"parameter.gain");REQUIRE(gain);gain->setValue(.25,juce::sendNotificationSync);
            CHECK(std::abs(gain->getValue()-.25)<1e-12);
            REQUIRE(workspace.controller().revision()==8);
        }
        if(mode!=2){const auto before=logs->getText();workspace.run_log_received(old);CHECK(logs->getText()==before);}
        click(workspace,"project.run");
        // Replace the request before releasing A. Even a fast A terminal now
        // belongs to the revoked identity, while the queued Run gets fresh state.
        if(mode==2) REQUIRE(std::filesystem::remove(gate));
        const auto submitted=logs->getText();workspace.run_log_received(old);CHECK(logs->getText()==submitted);
        auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);
        REQUIRE(pump([&]{return status->status_text().contains("Completed run");},15000));
        CHECK(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0);
        CHECK_FALSE(logs->getText().contains(old.identity.request_id));CHECK_FALSE(logs->getText().contains("late-A-must-not-leak"));
        const auto baseline=logs->getText();workspace.run_log_received(old);CHECK(logs->getText()==baseline);
        std::filesystem::path success;
        for(const auto& dir:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(dir.path()/"run.json")){
            REQUIRE(success.empty());success=dir.path();
        }
        REQUIRE_FALSE(success.empty());
        click(workspace,"project.play"); CHECK(preview->played == success/"output.wav");
        const auto playing_status = status->status_text();
        workspace.run_log_received(old); CHECK(status->status_text() == playing_status); CHECK(preview->played == success/"output.wav");
        std::ofstream(root/"playback-file.txt")<<preview->played.generic_string();
        const auto receipt=juce::JSON::parse(juce::File((success/"run.json").generic_string()));
        CHECK(receipt["request_id"].toString().toStdString()!=old.identity.request_id);CHECK(logs->getText().contains(receipt["request_id"].toString()));
        const auto audio=read_wav_file(success/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(audio));
        const auto& output=std::get<AudioBuffer>(audio);REQUIRE(output.frame_count()==513);
        const float gain=mode==2 ? .5f : .25f;
        for(std::size_t n=0;n<513;++n){const float expected=input.channel(0)[n]*gain;CHECK(std::abs(output.channel(0)[n]-expected)<=2e-6+1e-5*std::abs(expected));}
        std::ofstream(root/"current-ui.log",std::ios::binary)<<logs->getText().toStdString();
        std::ofstream(root/"scenario.json")<<"{\"mode\":"<<mode<<",\"old_pid\":"<<old_pid<<",\"old_request\":\""<<old.identity.request_id<<"\",\"current_request\":\""<<receipt["request_id"].toString()<<"\"}";
        artifact_roots.add(juce::String(root.generic_string()));
        int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));
    }
    std::cout << "PC0309_ARTIFACT_ROOTS=" << juce::JSON::toString(artifact_roots,true).toStdString() << '\n';
#endif
}

TEST_CASE("PC-04.b production flood enforces complete and UI budgets while remaining responsive and recovering",
          "[PC-04.b][PC-04.d][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled production Run");
#else
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0405-live-flood/artifacts"/juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout<<"PC0405_ARTIFACT_ROOT="<<root.generic_string()<<'\n';
    auto registry=std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
    auto service=std::make_shared<PythonAuthoring>(root/"library");
    auto draft=python_draft("pc04-live-flood");
    const auto literal=juce::JSON::toString(juce::var(juce::String(root.generic_string())),true).toStdString();
    draft.source="import os\nimport pathlib\nimport subprocess\nimport sys\nimport time\ndef process(inputs, params, state):\n    root=pathlib.Path("+literal+")\n    mode=int(-params['gain'])\n    if mode>0 and not state.get('printed'):\n        state['printed']=True\n        child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(120)'],creationflags=0x08000000)\n        temporary=root/'entered.tmp'\n        temporary.write_text(str(os.getpid())+' '+str(child.pid),encoding='ascii')\n        temporary.replace(root/'entered.txt')\n        while not (root/'start.txt').exists(): time.sleep(.005)\n        chunk=b'F'*4096\n        for n in range(4096): os.write(1,chunk)\n        if mode==2: os.write(1,b'X')\n        if mode==3:\n            while True: os.write(1,chunk)\n        (root/'burst.txt').write_text('complete',encoding='ascii')\n        while not (root/'finish.txt').exists(): time.sleep(.005)\n        child.terminate()\n        child.wait()\n    return {'audio_out':inputs['audio_in']}\n";
    // Three integer modes without relying on Inspector quantization.
    auto definition=juce::JSON::parse(draft.declaration);
    auto* parameters=definition["parameters"].getArray(); REQUIRE(parameters);
    (*parameters)[0].getDynamicObject()->setProperty("minimum",-3.0);
    draft.declaration=juce::JSON::toString(definition,false).toStdString();
    auto cancel=std::make_shared<std::atomic<bool>>(false);
    auto checked=service->check(draft,cancel); REQUIRE(std::holds_alternative<ReadyPackage>(checked));
    auto applied=service->apply(std::get<ReadyPackage>(checked),draft,registry,cancel); REQUIRE(std::holds_alternative<AppliedPackage>(applied));
    *registry=*std::get<AppliedPackage>(applied).catalog;
    AudioBuffer input(16000,{"ch0"},513);
    for(std::size_t n=0;n<513;++n)input.mutable_channel(0)[n]=static_cast<float>(.2*std::sin(.03*(n+1)));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc04-flood";project.selected_output_node_id="output";
    auto custom=editor::make_node_instance(*registry,{},"user.custom_node","custom",240,100);REQUIRE(custom);
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},40,100},*custom,{"output","builtin.output",{},640,100}};
    project.connections={{"file","audio","custom","audio_in"},{"custom","audio_out","output","audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"project.aaproj",project)));
    struct FloodPreview final:AudioPreviewService{
        int calls=0;AudioPreviewOutcome play(const std::filesystem::path&)override{++calls;return std::monostate{};}
        void pause()override{}void stop()override{}void shutdown()override{}bool is_playing()const noexcept override{return false;}double position_seconds()const noexcept override{return 0;}
    };
    auto preview=std::make_shared<FloodPreview>();
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),preview,
        std::make_shared<FileDraftStorage>(root/"drafts"),false,service);
    workspace.open_project_file(root/"project.aaproj");REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));
    auto* canvas=control<editor::GraphCanvas>(workspace,"project.canvas");REQUIRE(canvas);canvas->select("custom");
    auto* gain=control<juce::Slider>(workspace,"parameter.gain");REQUIRE(gain);
    auto* logs=control<audioalgo::editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);
    auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);
    struct OwnedProcess { HANDLE handle=nullptr;bool terminate=false;~OwnedProcess(){if(handle){if(terminate)TerminateProcess(handle,0);CloseHandle(handle);}} } canary;
    wchar_t system_directory[MAX_PATH]{};REQUIRE(GetSystemDirectoryW(system_directory,MAX_PATH)>0);
    auto ping=std::filesystem::path(system_directory)/"ping.exe";auto command=L"\""+ping.wstring()+L"\" -t 127.0.0.1";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(ping.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi));
    CloseHandle(pi.hThread);canary.handle=pi.hProcess;canary.terminate=true;
    std::set<std::filesystem::path> successes;
    juce::Array<juce::var> receipts;
    for(int mode=1;mode<=3;++mode){
        INFO(mode);
        for(const auto* name:{"entered.txt","start.txt","finish.txt","burst.txt"})std::filesystem::remove(root/name);
        gain->setValue(-mode,juce::sendNotificationSync);REQUIRE(gain->getValue()==-mode);
        click(workspace,"project.run");
        DWORD parent_pid=0,child_pid=0;
        REQUIRE(pump([&]{std::ifstream in(root/"entered.txt");return static_cast<bool>(in>>parent_pid>>child_pid)&&parent_pid&&child_pid;},15000));
        OwnedProcess parent{OpenProcess(SYNCHRONIZE,FALSE,parent_pid)},child{OpenProcess(SYNCHRONIZE,FALSE,child_pid)};
        REQUIRE(parent.handle);REQUIRE(child.handle);CHECK(WaitForSingleObject(parent.handle,0)==WAIT_TIMEOUT);CHECK(WaitForSingleObject(child.handle,0)==WAIT_TIMEOUT);
        struct Heartbeat final:juce::Timer{
            editor::RunLogTextView* logs=nullptr;std::chrono::steady_clock::time_point previous;std::vector<double> gaps;std::size_t maximum_bytes=0;
            void timerCallback()override{auto now=std::chrono::steady_clock::now();gaps.push_back(std::chrono::duration<double,std::milli>(now-previous).count());previous=now;maximum_bytes=std::max(maximum_bytes,logs->getText().getNumBytesAsUTF8());}
        } heartbeat;
        const auto started=std::chrono::steady_clock::now();heartbeat.logs=logs;heartbeat.previous=started;heartbeat.startTimer(20);
        {std::ofstream(root/"start.txt")<<"release";}
        if(mode==1){
            REQUIRE(pump([&]{return std::chrono::steady_clock::now()-started>=std::chrono::seconds(10);},11000));
            CHECK(std::filesystem::is_regular_file(root/"burst.txt"));CHECK_FALSE(status->status_text().contains("Completed run"));
        }else{REQUIRE(pump([&]{return status->status_text().contains("log_limit");},5000));}
        heartbeat.stopTimer();const auto finished=std::chrono::steady_clock::now();
        heartbeat.gaps.push_back(std::chrono::duration<double,std::milli>(finished-heartbeat.previous).count());
        std::sort(heartbeat.gaps.begin(),heartbeat.gaps.end());REQUIRE_FALSE(heartbeat.gaps.empty());
        const auto p95=heartbeat.gaps[static_cast<std::size_t>(.95*(heartbeat.gaps.size()-1))],maximum=heartbeat.gaps.back();
        const auto duration=std::chrono::duration<double,std::milli>(finished-started).count();
        const auto terminal=status->status_text();
        CHECK(heartbeat.maximum_bytes<=256*1024);CHECK(logs->getText().getNumBytesAsUTF8()<=256*1024);
        // The text editor owns the ID; retrieve the production panel via parent.
        auto* log_panel=dynamic_cast<editor::RunLogPanel*>(logs->getParentComponent());REQUIRE(log_panel);
        CHECK(log_panel->text_for_copy().getNumBytesAsUTF8()<=256*1024);
        CHECK(logs->getText().contains("stream tail discarded"));CHECK(logs->getText().contains("pc04-flood"));
        if(mode!=1)CHECK(duration<=5000);
        if(mode==1){CHECK(duration>=10000);CHECK(p95<=100);CHECK(maximum<=500);std::ofstream(root/"finish.txt")<<"release";
            REQUIRE(pump([&]{return status->status_text().contains("Completed run");},5000));
        }else{CHECK_FALSE(status->status_text().contains("Completed run"));click(workspace,"project.play");CHECK(preview->calls==0);CHECK(status->status_text().contains("Run the graph before previewing audio"));}
        REQUIRE(pump([&]{return WaitForSingleObject(parent.handle,0)==WAIT_OBJECT_0&&WaitForSingleObject(child.handle,0)==WAIT_OBJECT_0;},5000));
        CHECK(WaitForSingleObject(canary.handle,0)==WAIT_TIMEOUT);
        std::ofstream(root/("ui-"+std::to_string(mode)+".log"),std::ios::binary)<<logs->getText().toStdString();
        juce::DynamicObject::Ptr receipt=new juce::DynamicObject;
        receipt->setProperty("mode",mode);receipt->setProperty("window_ms",duration);receipt->setProperty("interval_ms",20);receipt->setProperty("samples",static_cast<int>(heartbeat.gaps.size()));
        receipt->setProperty("p95_ms",p95);receipt->setProperty("max_ms",maximum);receipt->setProperty("maximum_ui_bytes",static_cast<juce::int64>(heartbeat.maximum_bytes));
        receipt->setProperty("parent_pid",static_cast<juce::int64>(parent_pid));receipt->setProperty("child_pid",static_cast<juce::int64>(child_pid));receipt->setProperty("terminal",mode==1?status->status_text():terminal);receipts.add(juce::var(receipt.get()));
        if(mode!=1){gain->setValue(.5,juce::sendNotificationSync);click(workspace,"project.run");REQUIRE(pump([&]{return status->status_text().contains("Completed run");},5000));}
        for(const auto& dir:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(dir.path()/"run.json"))successes.insert(dir.path());
        CHECK(successes.size()==static_cast<std::size_t>(mode));
    }
    for(const auto& run:successes){auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);REQUIRE(output.frame_count()==513);
        for(std::size_t n=0;n<513;++n)CHECK(output.channel(0)[n]==input.channel(0)[n]);}
    std::ofstream(root/"flood-demo.json")<<juce::JSON::toString(receipts,false).toStdString();
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));
#endif
}

TEST_CASE("PC-04.d bounded log rendering preserves retained copy and node filters", "[PC-04.d][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel;panel.setSize(720,240);
    audioalgo::NodeLogEvent event;event.identity={"project",1,"request",1};event.node_id="first";event.source="stdout";event.level="info";
    event.text="retained-first-prefix\n"+std::string(65536,'F');panel.append(event);
    auto* text=control<audioalgo::editor::RunLogTextView>(panel,"project.run_logs");REQUIRE(text);
    CHECK(text->getText().contains("retained-first-prefix"));CHECK(text->getText().contains("request"));CHECK(text->getText().getNumBytesAsUTF8()<=256*1024);
    const auto first=panel.text_for_copy();CHECK(first.contains("retained-first-prefix"));CHECK(first.contains(event.text));CHECK_FALSE(first.contains("UI visible tail omitted"));
    event.node_id="second";event.text="second-only";panel.append(event);
    auto* filter=control<juce::ComboBox>(panel,"project.run_logs.node_filter");REQUIRE(filter);
    int second_id=0;for(int i=0;i<filter->getNumItems();++i)if(filter->getItemText(i)=="second")second_id=filter->getItemId(i);REQUIRE(second_id>1);
    filter->setSelectedId(second_id,juce::sendNotificationSync);CHECK(text->getText().contains("second-only"));CHECK_FALSE(panel.text_for_copy().contains("retained-first-prefix"));
    filter->setSelectedId(1,juce::sendNotificationSync);CHECK(panel.text_for_copy().contains("retained-first-prefix"));CHECK(panel.text_for_copy().contains("second-only"));
}

TEST_CASE("PC-04.d flood layout diagnostic isolates long paragraphs", "[PC0405-diagnostic][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    for(bool line_breaks:{false,true}){
        audioalgo::editor::RunLogPanel panel;panel.setSize(720,240);
        audioalgo::NodeLogEvent event;event.identity={"diagnostic",1,"request",1};event.node_id="node";event.source="stdout";event.level="info";
        event.text=std::string(65536,'F');if(line_breaks)for(std::size_t n=63;n<event.text.size();n+=64)event.text[n]='\n';
        for(int i=0;i<4;++i){auto started=std::chrono::steady_clock::now();panel.append(event);
            std::cout<<"line_breaks="<<line_breaks<<" append="<<i<<" ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'\n';}
        REQUIRE(pump([&]{return control<audioalgo::editor::RunLogTextView>(panel,"project.run_logs")->getText().isNotEmpty();}));
    }
}

TEST_CASE("PC-04.c production twenty success error cancel rounds drain queues processes and log views",
          "[PC-04.c][PC0408][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled production Run");
#else
    using namespace audioalgo;using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0408-gui-cycles/artifacts"/juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);std::cout<<"PC0408_ARTIFACT_ROOT="<<root.generic_string()<<'\n';
    auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));auto service=std::make_shared<PythonAuthoring>(root/"library");
    auto draft=python_draft("pc04-gui-cycles");const auto literal=juce::JSON::toString(juce::var(juce::String(root.generic_string())),true).toStdString();
    draft.source="import os\nimport pathlib\nimport subprocess\nimport sys\nimport time\ndef process(inputs, params, state):\n    if not state.get('entered'):\n        state['entered']=True\n        root=pathlib.Path("+literal+")\n        child=None\n        if params['gain']==-2: child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(120)'],creationflags=0x08000000)\n        temporary=root/'processes.tmp'\n        temporary.write_text(str(os.getpid())+' '+str(child.pid if child else 0),encoding='ascii')\n        temporary.replace(root/'processes.txt')\n        while (root/'entry-hold.txt').exists(): time.sleep(.005)\n        print('cycle-log gain='+str(params['gain'])+' pid='+str(os.getpid()),flush=True)\n        if params['gain']==-1: raise RuntimeError('cycle-failure')\n        if params['gain']==-2:\n            while True: time.sleep(.005)\n    return {'audio_out':inputs['audio_in']}\n";
    auto cancel=std::make_shared<std::atomic<bool>>(false);const auto checked=service->check(draft,cancel);REQUIRE(std::holds_alternative<ReadyPackage>(checked));
    const auto applied=service->apply(std::get<ReadyPackage>(checked),draft,registry,cancel);REQUIRE(std::holds_alternative<AppliedPackage>(applied));*registry=*std::get<AppliedPackage>(applied).catalog;
    AudioBuffer input(16000,{"ch0"},513);for(std::size_t n=0;n<513;++n)input.mutable_channel(0)[n]=static_cast<float>(.2*std::sin(.03*(n+1)));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc04-gui-cycles";project.selected_output_node_id="output";
    auto custom=editor::make_node_instance(*registry,{},"user.custom_node","custom",240,100);REQUIRE(custom);
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},40,100},*custom,{"output","builtin.output",{},640,100}};
    project.connections={{"file","audio","custom","audio_in"},{"custom","audio_out","output","audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"project.aaproj",project)));
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<FileDraftStorage>(root/"drafts"),false,service);
    workspace.open_project_file(root/"project.aaproj");REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));
    control<editor::GraphCanvas>(workspace,"project.canvas")->select("custom");auto* gain=control<juce::Slider>(workspace,"parameter.gain");REQUIRE(gain);
    auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);
    struct Process {HANDLE handle=nullptr;bool terminate=false;~Process(){if(handle){if(terminate)TerminateProcess(handle,0);CloseHandle(handle);}}};
    Process canary;wchar_t system_directory[MAX_PATH]{};REQUIRE(GetSystemDirectoryW(system_directory,MAX_PATH)>0);const auto ping=std::filesystem::path(system_directory)/"ping.exe";
    auto command=L"\""+ping.wstring()+L"\" -t 127.0.0.1";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(ping.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi));CloseHandle(pi.hThread);canary.handle=pi.hProcess;canary.terminate=true;
    struct Entered {DWORD parent_pid=0,child_pid=0;Process parent,child;};
    std::set<std::filesystem::path> seen;juce::Array<juce::var> receipts,rounds;std::size_t maximum_ui_bytes=0;
    const auto idle=[&]{const auto counts=workspace.background_run_counts();return counts.active==0&&counts.pending==0&&!counts.worker_claimed&&workspace.background_job_count()==0;};
    const auto observe=[&]{const auto counts=workspace.background_run_counts();CHECK(counts.active<=1);CHECK(counts.pending<=1);CHECK(workspace.background_job_count()<=1);maximum_ui_bytes=std::max(maximum_ui_bytes,logs->getText().getNumBytesAsUTF8());CHECK(maximum_ui_bytes<=256*1024);};
    const auto begin=[&](double value,Entered* old=nullptr){
        std::filesystem::remove(root/"processes.txt");{std::ofstream(root/"entry-hold.txt")<<"hold";}
        gain->setValue(value,juce::sendNotificationSync);const auto started=std::chrono::steady_clock::now();click(workspace,"project.run");
        if(old){REQUIRE(pump([&]{observe();return WaitForSingleObject(old->parent.handle,0)==WAIT_OBJECT_0&&WaitForSingleObject(old->child.handle,0)==WAIT_OBJECT_0;},5000));CHECK(std::chrono::steady_clock::now()-started<=std::chrono::seconds(5));}
        auto entry=std::make_unique<Entered>();REQUIRE(pump([&]{std::ifstream in(root/"processes.txt");observe();return static_cast<bool>(in>>entry->parent_pid>>entry->child_pid)&&entry->parent_pid!=0;}));
        entry->parent.handle=OpenProcess(SYNCHRONIZE,FALSE,entry->parent_pid);REQUIRE(entry->parent.handle);CHECK(WaitForSingleObject(entry->parent.handle,0)==WAIT_TIMEOUT);
        if(value==-2){REQUIRE(entry->child_pid!=0);entry->child.handle=OpenProcess(SYNCHRONIZE,FALSE,entry->child_pid);REQUIRE(entry->child.handle);CHECK(WaitForSingleObject(entry->child.handle,0)==WAIT_TIMEOUT);}
        const auto counts=workspace.background_run_counts();CHECK(counts.active==1);CHECK(counts.pending==0);REQUIRE(std::filesystem::remove(root/"entry-hold.txt"));return entry;
    };
    const auto record=[&](int round,const std::string& phase,const Entered& entry){
        std::filesystem::path archive;for(const auto& dir:std::filesystem::directory_iterator(root/"run-logs"))if(!seen.contains(dir.path())&&std::filesystem::is_regular_file(dir.path()/"events.jsonl")){REQUIRE(archive.empty());archive=dir.path();}
        REQUIRE_FALSE(archive.empty());seen.insert(archive);std::ifstream in(archive/"events.jsonl");std::string line;REQUIRE(static_cast<bool>(std::getline(in,line)));const auto row=juce::JSON::parse(line);
        CHECK(row["text"].toString().contains("pid="+juce::String(entry.parent_pid)));
        juce::DynamicObject::Ptr receipt=new juce::DynamicObject;receipt->setProperty("round",round);receipt->setProperty("phase",juce::String(phase));receipt->setProperty("parent_pid",static_cast<juce::int64>(entry.parent_pid));receipt->setProperty("child_pid",static_cast<juce::int64>(entry.child_pid));receipt->setProperty("request_id",row["request_id"]);receipt->setProperty("archive",juce::String((archive/"events.jsonl").lexically_relative(root).generic_string()));receipts.add(juce::var(receipt.get()));
    };
    const auto finish=[&](bool error){REQUIRE(pump([&]{observe();return error?status->status_text().contains("cycle-failure"):status->status_text().contains("Completed run");}));REQUIRE(pump(idle));observe();CHECK(WaitForSingleObject(canary.handle,0)==WAIT_TIMEOUT);};
    {auto warmup=begin(.5);finish(false);CHECK(WaitForSingleObject(warmup->parent.handle,0)==WAIT_OBJECT_0);record(-1,"warmup",*warmup);}
    DWORD baseline=0;REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&baseline));
    for(int round=0;round<20;++round){INFO(round);
        {auto success=begin(.5);finish(false);CHECK(WaitForSingleObject(success->parent.handle,0)==WAIT_OBJECT_0);record(round,"success",*success);}
        {auto error=begin(-1);finish(true);CHECK(WaitForSingleObject(error->parent.handle,0)==WAIT_OBJECT_0);CHECK(logs->getText().contains("cycle-log"));CHECK_FALSE(status->status_text().contains("Completed run"));record(round,"error",*error);}
        {auto cancelled=begin(-2);REQUIRE(pump([&]{observe();return logs->getText().contains("cycle-log");}));record(round,"cancel",*cancelled);
            auto recovery=begin(.5,cancelled.get());finish(false);CHECK(WaitForSingleObject(recovery->parent.handle,0)==WAIT_OBJECT_0);record(round,"recovery",*recovery);}
        REQUIRE(idle());DWORD handles=0;REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&handles));CHECK(handles<=baseline);
        juce::DynamicObject::Ptr receipt=new juce::DynamicObject;receipt->setProperty("round",round);receipt->setProperty("handles",static_cast<juce::int64>(handles));receipt->setProperty("active",static_cast<int>(workspace.background_run_counts().active));receipt->setProperty("pending",static_cast<int>(workspace.background_run_counts().pending));receipt->setProperty("worker_jobs",workspace.background_job_count());receipt->setProperty("ui_bytes",static_cast<juce::int64>(logs->getText().getNumBytesAsUTF8()));rounds.add(juce::var(receipt.get()));
        std::ofstream(root/"cycles-progress.json")<<juce::JSON::toString(rounds,false).toStdString();
    }
    std::size_t success_count=0;for(const auto& dir:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(dir.path()/"run.json")){
        ++success_count;const auto read=read_wav_file(dir.path()/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);REQUIRE(output.frame_count()==513);for(std::size_t n=0;n<513;++n)CHECK(output.channel(0)[n]==input.channel(0)[n]);}
    CHECK(success_count==41);CHECK(receipts.size()==81);CHECK(WaitForSingleObject(canary.handle,0)==WAIT_TIMEOUT);
    std::ofstream(root/"cycles.json")<<juce::JSON::toString(receipts,false).toStdString();
    std::ofstream(root/"cycle-limits.json")<<"{\"rounds\":20,\"tasks\":81,\"baseline_handles\":"<<baseline<<",\"maximum_ui_bytes\":"<<maximum_ui_bytes<<",\"success_outputs\":"<<success_count<<"}";
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(workspace.background_job_count()==0);
#endif
}

namespace {
struct MixedFormulaProfile {int direction,sample_rate,channels,frames,host;};
void mixed_formula_scenario(const std::vector<MixedFormulaProfile>& profiles,const char* slice,int gui_repeats=1) {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled editor and fresh CLI");
#else
    using namespace audioalgo;using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops"/slice/"artifacts"/juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);std::cout<<"PC05_FORMULA_ROOT="<<root.generic_string()<<'\n';
    auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));auto service=std::make_shared<PythonAuthoring>(root/"library");
    auto draft=python_draft("pc05-stateful-formula",PythonTemplate::stateful_ramp);
    auto definition=juce::JSON::parse(draft.declaration);definition.getDynamicObject()->setProperty("type_id","user.pc05_transform");
    draft.declaration=juce::JSON::toString(definition,false).toStdString();
    draft.source="import numpy as np\ndef process(inputs, params, state):\n    audio=inputs['audio_in']\n    start=int(state.get('frame',0))\n    if start==0: print('pc05-start gain='+str(params['gain'])+' step='+str(params['step']),flush=True)\n    n=np.arange(start,start+audio.shape[1],dtype=np.float64)\n    ramp=np.asarray(n*float(params['step']),dtype=np.float32)\n    state['frame']=start+audio.shape[1]\n    state['calls']=int(state.get('calls',0))+1\n    state['maximum_block']=max(int(state.get('maximum_block',0)),audio.shape[1])\n    return {'audio_out':np.asarray(audio*np.float32(params['gain'])+ramp[None,:],dtype=np.float32)}\n\ndef reset(state,reason):\n    print('pc05-end frames='+str(state.get('frame',0))+' calls='+str(state.get('calls',0))+' max_block='+str(state.get('maximum_block',0))+' reason='+str(reason),flush=True)\n    state.clear()\n";
    const auto test_host=juce::File::getSpecialLocation(juce::File::currentExecutableFile);const auto host_hash=juce::SHA256(test_host).toHexString();
    auto cancel=std::make_shared<std::atomic<bool>>(false);const auto checked=service->check(draft,cancel);REQUIRE(std::holds_alternative<ReadyPackage>(checked));
    const auto applied=service->apply(std::get<ReadyPackage>(checked),draft,registry,cancel);REQUIRE(std::holds_alternative<AppliedPackage>(applied));*registry=*std::get<AppliedPackage>(applied).catalog;
    CHECK(juce::SHA256(test_host).toHexString()==host_hash);
    const auto previous=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT",{}).toStdString();
    struct RestoreEnv{std::string value;~RestoreEnv(){_putenv_s("AUDIOALGO_AUTHORING_ROOT",value.c_str());}}restore{previous};
    REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT",(root/"library").string().c_str())==0);
    const auto cli=test_host.getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto cli_hash=juce::SHA256(cli).toHexString();
    juce::Array<juce::var> receipts;
    for(std::size_t index=0;index<profiles.size();++index){
        const auto profile=profiles[index];INFO(index);INFO(profile.direction);
        const auto folder=root/("case-"+std::to_string(index));std::filesystem::create_directories(folder);
        std::vector<std::string> channels;for(int c=0;c<profile.channels;++c)channels.push_back("ch"+std::to_string(c));
        AudioBuffer input(profile.sample_rate,channels,profile.frames);
        for(int c=0;c<profile.channels;++c)for(int n=0;n<profile.frames;++n)input.mutable_channel(c)[n]=static_cast<float>(.2*std::sin(.03*(n+1))+.02*c);
        REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(folder/"input.wav",input,WavEncoding::float32)));
        ProjectModel project;project.schema_version=2;project.project_id="pc05-formula-"+std::to_string(index);project.revision=7;project.selected_output_node_id="output";
        project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100}};
        const auto python_node=[&](const char* id,double gain,double step){auto node=editor::make_node_instance(*registry,{},"user.pc05_transform",id,220+200*(id[0]-'A'),100);REQUIRE(node);node->parameters["gain"]=gain;node->parameters["step"]=step;return *node;};
        constexpr double twice_db=6.020599913279624;
        if(profile.direction==0){project.nodes.push_back({"A","builtin.gain",{{"gain_db",twice_db}},220,100});project.nodes.push_back(python_node("B",.25,.00001));project.nodes.push_back({"C","builtin.gain",{{"gain_db",-twice_db}},620,100});}
        else{project.nodes.push_back(python_node("A",.5,.00001));project.nodes.push_back({"B","builtin.gain",{{"gain_db",twice_db}},420,100});project.nodes.push_back(python_node("C",.25,0));}
        project.nodes.push_back({"output","builtin.output",{},820,100});
        project.connections={{"file","audio","A","audio_in"},{"A","audio_out","B","audio_in"},{"B","audio_out","C","audio_in"},{"C","audio_out","output","audio"}};
        const auto project_file=folder/"project.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_file,project)));
        std::set<std::filesystem::path> seen;std::set<std::string> requests;
        const auto verify=[&](const std::filesystem::path& run,const char* origin,int repeat){
            const auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);
            REQUIRE(output.frame_count()==static_cast<std::size_t>(profile.frames));CHECK(output.sample_rate_hz()==profile.sample_rate);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.valid_interval()==input.valid_interval());
            for(int c=0;c<profile.channels;++c)for(int n=0;n<profile.frames;++n){
                const float x=input.channel(c)[n],state=static_cast<float>(1e-5*static_cast<double>(n));float expected=0;
                if(profile.direction==0){const float a=x*2.f;const float b=static_cast<float>(a*.25f)+state;expected=b*.5f;}
                else{const float a=static_cast<float>(x*.5f)+state;const float b=a*2.f;expected=b*.25f;}
                CHECK(std::abs(output.channel(c)[n]-expected)<=2e-6+1e-5*std::abs(expected));
            }
            const auto record=juce::JSON::parse(juce::File((run/"run.json").generic_string()));REQUIRE(record.isObject());
            CHECK(static_cast<int>(record["block_size"])==profile.host);CHECK(static_cast<int>(record["frames"])==profile.frames);CHECK(static_cast<int>(record["valid_start_frame"])==0);CHECK(static_cast<int>(record["valid_frame_count"])==profile.frames);
            CHECK(record["project_id"].toString()==juce::String(project.project_id));CHECK(requests.insert(record["request_id"].toString().toStdString()).second);
            juce::DynamicObject::Ptr receipt=new juce::DynamicObject;receipt->setProperty("profile",static_cast<int>(index));receipt->setProperty("direction",profile.direction);receipt->setProperty("sample_rate",profile.sample_rate);receipt->setProperty("channels",profile.channels);receipt->setProperty("frames",profile.frames);receipt->setProperty("host",profile.host);receipt->setProperty("origin",origin);receipt->setProperty("repeat",repeat);receipt->setProperty("project_file",juce::String(project_file.lexically_relative(root).generic_string()));receipt->setProperty("run",juce::String(run.lexically_relative(root).generic_string()));receipt->setProperty("request_id",record["request_id"]);receipts.add(juce::var(receipt.get()));
        };
        {
            editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<FileDraftStorage>(root/"drafts"),false,service,{},false,{},{},{},editor::RunConfiguration{profile.host});
            workspace.open_project_file(project_file);REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));
            for(int repeat=0;repeat<gui_repeats;++repeat){
                click(workspace,"project.run");auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);
                const auto finished=pump([&]{return status->status_text().contains("Completed run");},10000);std::ofstream(folder/("gui-status-"+std::to_string(repeat)+".txt"))<<status->status_text().toStdString();INFO(status->status_text().toStdString());REQUIRE(finished);
                auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);CHECK(logs->getText().contains("pc05-start"));
                std::filesystem::path run;for(const auto& dir:std::filesystem::directory_iterator(folder/"runs"))if(std::filesystem::is_regular_file(dir.path()/"run.json")&&!seen.contains(dir.path())){REQUIRE(run.empty());run=dir.path();}
                REQUIRE_FALSE(run.empty());seen.insert(run);verify(run,"gui",repeat);std::ofstream(folder/("gui-log-"+std::to_string(repeat)+".txt"),std::ios::binary)<<logs->getText().toStdString();
            }
            int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));
        }
        const auto cli_run=folder/"cli-runs/run";
        std::filesystem::create_directories(cli_run.parent_path());
        struct CliResult{bool started=false,finished=false;int code=-1;juce::String output;};
        auto future=std::async(std::launch::async,[&]{CliResult result;juce::ChildProcess child;result.started=child.start(juce::StringArray{cli.getFullPathName(),"run",juce::String(project_file.generic_string()),juce::String(cli_run.generic_string()),juce::String(profile.host)});if(!result.started)return result;auto reader=std::async(std::launch::async,[&]{return child.readAllProcessOutput();});result.finished=child.waitForProcessToFinish(10000);if(!result.finished)child.kill();result.output=reader.get();result.code=child.getExitCode();return result;});
        REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));const auto outcome=future.get();
        std::ofstream(folder/"cli-stdout.txt",std::ios::binary)<<outcome.output.toStdString();INFO(outcome.output.toStdString());REQUIRE(outcome.started);REQUIRE(outcome.finished);REQUIRE(outcome.code==0);verify(cli_run,"cli",0);
        std::ofstream(root/"formula-progress.json")<<juce::JSON::toString(receipts,false).toStdString();
    }
    CHECK(juce::SHA256(test_host).toHexString()==host_hash);CHECK(juce::SHA256(cli).toHexString()==cli_hash);
    std::ofstream(root/"formula-demo.json")<<juce::JSON::toString(receipts,false).toStdString();
    std::ofstream(root/"host-hashes.json")<<"{\"test_host\":\""<<host_hash<<"\",\"cli\":\""<<cli_hash<<"\"}";
#endif
}
} // namespace

TEST_CASE("PC-05.a production Run and fresh CLI execute both documented mixed formulas", "[PC-05.a][node_authoring][component]") {
    mixed_formula_scenario({{0,48000,2,513,127},{1,48000,2,513,127}},"PC0501-mixed-formulas",2);
}

TEST_CASE("PC-05.a GUI CLI mixed formula matrix preserves state across formats frames and host partitions", "[PC-05.a][PC0502][node_authoring][component]") {
    std::vector<MixedFormulaProfile> profiles;
    for(int direction:{0,1})for(int rate:{16000,48000})for(int channels:{1,2,4})for(int frames:{1,513,16037})profiles.push_back({direction,rate,channels,frames,127});
    // host127 is already present above; do not duplicate or add its count.
    for(int direction:{0,1})for(int host:{1,512,1024})profiles.push_back({direction,48000,2,16037,host});
    mixed_formula_scenario(profiles,"PC0502-mixed-matrix");
}

TEST_CASE("PC-05.b production Run and fresh CLI select named outputs with one shared upstream execution", "[PC-05.b][PC0503][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled editor and fresh CLI");
#else
    using namespace audioalgo;using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0503-named-shared/artifacts"/juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);std::cout<<"PC05_NAMED_ROOT="<<root.generic_string()<<'\n';
    auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));auto service=std::make_shared<PythonAuthoring>(root/"library");
    auto shared=python_draft("pc05-shared",PythonTemplate::stateful_ramp);
    auto declaration=juce::JSON::parse(shared.declaration);declaration.getDynamicObject()->setProperty("type_id","user.pc05_shared");shared.declaration=juce::JSON::toString(declaration,false).toStdString();
    shared.source=R"PY(import numpy as np
def process(inputs,params,state):
    audio=inputs['audio_in']
    print('pc05-shared frame='+str(state.get('frame',0))+' count='+str(audio.shape[1]),flush=True)
    state['frame']=int(state.get('frame',0))+audio.shape[1]
    state['calls']=int(state.get('calls',0))+1
    return {'audio_out':audio*np.float32(params['gain'])}
def reset(state,reason):
    print('pc05-shared-reset frames='+str(state.get('frame',0))+' calls='+str(state.get('calls',0))+' reason='+str(reason),flush=True)
    state.clear()
)PY";
    auto named=python_draft("pc05-named",PythonTemplate::named_router);
    declaration=juce::JSON::parse(named.declaration);declaration.getDynamicObject()->setProperty("type_id","user.pc05_named");named.declaration=juce::JSON::toString(declaration,false).toStdString();
    named.source="import numpy as np\ndef process(inputs,params,state):\n    primary=inputs['primary']\n    reference=inputs['reference']\n    print('pc05-join count='+str(primary.shape[1]),flush=True)\n    return {'main':(np.float32(2)*primary+np.float32(3)*reference)*np.float32(params['gain']), 'side':primary-reference}\n";
    auto cancel=std::make_shared<std::atomic<bool>>(false);
    const auto install=[&](const Draft& draft){const auto checked=service->check(draft,cancel);if(auto* e=std::get_if<SourceDiagnostic>(&checked))INFO(e->code+": "+e->message);REQUIRE(std::holds_alternative<ReadyPackage>(checked));const auto applied=service->apply(std::get<ReadyPackage>(checked),draft,registry,cancel);REQUIRE(std::holds_alternative<AppliedPackage>(applied));*registry=*std::get<AppliedPackage>(applied).catalog;};
    const auto host=juce::File::getSpecialLocation(juce::File::currentExecutableFile);const auto cli=host.getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto host_hash=juce::SHA256(host).toHexString(),cli_hash=juce::SHA256(cli).toHexString();
    install(shared);install(named);
    const auto previous=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT",{}).toStdString();struct RestoreNamedEnv{std::string value;~RestoreNamedEnv(){_putenv_s("AUDIOALGO_AUTHORING_ROOT",value.c_str());}}restore{previous};REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT",(root/"library").string().c_str())==0);
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.2*std::sin(.03*(n+1))+.02*c);
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc05-named-shared";project.revision=7;project.selected_output_node_id="named";
    const auto shared_node=editor::make_node_instance(*registry,{},"user.pc05_shared","shared",220,100),named_node=editor::make_node_instance(*registry,{},"user.pc05_named","named",640,100);REQUIRE(shared_node);REQUIRE(named_node);
    constexpr double db=6.020599913279624;
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},*shared_node,{"primary-gain","builtin.gain",{{"gain_db",db}},430,40},{"reference-gain","builtin.gain",{{"gain_db",-db}},430,240},*named_node};
    project.connections={{"file","audio","shared","audio_in"},{"shared","audio_out","primary-gain","audio_in"},{"shared","audio_out","reference-gain","audio_in"},{"primary-gain","audio_out","named","primary"},{"reference-gain","audio_out","named","reference"}};
    juce::Array<juce::var> receipts;std::set<std::filesystem::path> seen;std::set<std::string> requests;
    const auto verify=[&](const std::filesystem::path& run,const std::string& port,const char* origin,int repeat){
        const auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& audio=std::get<AudioBuffer>(read);REQUIRE(audio.frame_count()==513);CHECK(audio.sample_rate_hz()==48000);CHECK(audio.channel_ids()==input.channel_ids());CHECK(audio.valid_interval()==input.valid_interval());
        for(int c=0;c<2;++c)for(int n=0;n<513;++n){const float upstream=input.channel(c)[n]*.5f,primary=upstream*2.f,reference=upstream*.5f;const float expected=port=="main"?(primary*2.f+reference*3.f)*.5f:primary-reference;CHECK(std::abs(audio.channel(c)[n]-expected)<=2e-6+1e-5*std::abs(expected));}
        const auto record=juce::JSON::parse(juce::File((run/"run.json").generic_string()));CHECK(static_cast<int>(record["block_size"])==127);const auto saved=load_project_file(run/record["project_snapshot"].toString().toStdString());REQUIRE(std::holds_alternative<ProjectModel>(saved));CHECK(std::get<ProjectModel>(saved).selected_output_port_id==port);CHECK(requests.insert(record["request_id"].toString().toStdString()).second);
        REQUIRE(record["task_artifacts"].getArray());REQUIRE(record["task_artifacts"].getArray()->size()==2);
        for(const auto& artifact:*record["task_artifacts"].getArray()){const auto p=artifact["provenance"];const auto raw=juce::File(p["logs"]["complete_directory"].toString()).getChildFile("stdout.log").loadFileAsString();if(p["node_id"].toString()=="shared"){CHECK(raw.contains("pc05-shared frame=0 count=127"));CHECK(raw.contains("pc05-shared frame=508 count=5"));CHECK(raw.contains("pc05-shared-reset frames=513 calls=5 reason=run_complete"));CHECK(juce::StringArray::fromLines(raw).joinIntoString("|").contains("pc05-shared frame=381 count=127"));}else{CHECK(p["node_id"].toString()=="named");CHECK(raw.contains("pc05-join count=5"));}}
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("port",juce::String(port));row->setProperty("origin",origin);row->setProperty("repeat",repeat);row->setProperty("run",juce::String(run.lexically_relative(root).generic_string()));row->setProperty("project",juce::String(port+".aaproj"));row->setProperty("request_id",record["request_id"]);receipts.add(juce::var(row.get()));
    };
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<FileDraftStorage>(root/"drafts"),false,service,{},false,{},{},{},editor::RunConfiguration{127});
    for(const std::string port:{"main","side"}){
        project.selected_output_port_id=port;const auto file=root/(port+".aaproj");REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));workspace.open_project_file(file);REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id&&workspace.controller().project().selected_output_port_id==port;}));
        for(int repeat=0;repeat<2;++repeat){click(workspace,"project.run");auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);const auto finished=pump([&]{return status->status_text().contains("Completed run");},10000);std::ofstream(root/(port+"-status-"+std::to_string(repeat)+".txt"))<<status->status_text().toStdString();INFO(status->status_text().toStdString());REQUIRE(finished);std::filesystem::path run;for(const auto& entry:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(entry.path()/"run.json")&&!seen.contains(entry.path())){REQUIRE(run.empty());run=entry.path();}REQUIRE_FALSE(run.empty());seen.insert(run);verify(run,port,"gui",repeat);auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);CHECK(logs->getText().contains("pc05-shared"));CHECK(logs->getText().contains("pc05-join"));std::ofstream(root/(port+"-gui-log-"+std::to_string(repeat)+".txt"))<<logs->getText().toStdString();}
        const auto destination=root/"cli-runs"/port;std::filesystem::create_directories(destination.parent_path());
        auto future=std::async(std::launch::async,[&]{juce::ChildProcess child;if(!child.start(juce::StringArray{cli.getFullPathName(),"run",juce::String(file.generic_string()),juce::String(destination.generic_string()),"127"}))return -1;auto reader=std::async(std::launch::async,[&]{return child.readAllProcessOutput();});const auto finished=child.waitForProcessToFinish(10000);if(!finished)child.kill();const auto text=reader.get();std::ofstream(root/(port+"-cli-stdout.txt"))<<text.toStdString();return finished?static_cast<int>(child.getExitCode()):-2;});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));REQUIRE(future.get()==0);verify(destination,port,"cli",0);
        std::ofstream(root/"named-progress.json")<<juce::JSON::toString(receipts,false).toStdString();
    }
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(juce::SHA256(host).toHexString()==host_hash);CHECK(juce::SHA256(cli).toHexString()==cli_hash);std::ofstream(root/"named-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"host-hashes.json")<<"{\"test_host\":\""<<host_hash<<"\",\"cli\":\""<<cli_hash<<"\"}";
#endif
}

TEST_CASE("PC-05.c published v1 rejects unsupported timing capabilities before loading a module", "[PC-05.c][PC0504][node_authoring]") {
    using namespace audioalgo;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0504-timing-contracts/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC05_CAPABILITY_ROOT="<<root.generic_string()<<'\n';
    const auto original=juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("examples/plugins/native_stateful/manifest.json.in").loadFileAsString().replace("@PLUGIN_REVISION@","1");REQUIRE(original.isNotEmpty());
    juce::Array<juce::var> receipts;
    for(const std::string kind:{"stream","offline","tail","control"}){
        const auto path=root/kind;std::filesystem::create_directories(path);const auto manifest=juce::JSON::parse(original);REQUIRE(manifest["nodes"].getArray());auto* node=(*manifest["nodes"].getArray())[0].getDynamicObject();REQUIRE(node);
        if(kind=="stream")node->setProperty("uses_streaming_frame_api",true);
        if(kind=="offline")node->setProperty("requires_offline_task",true);
        if(kind=="tail")node->setProperty("tail_frames",1);
        const std::string sentinel="PC0504 not a DLL\n";std::ofstream(path/"external_stateful.dll",std::ios::binary)<<sentinel;std::ofstream(path/"manifest.json")<<juce::JSON::toString(manifest,false).toStdString();
        NodeRegistry registry;REQUIRE(register_builtin_nodes(registry));const auto result=load_native_plugins(registry,{path});CHECK(result.loaded_plugin_ids.empty());REQUIRE(result.diagnostics.size()==1);
#ifdef _WIN32
        const auto control_error="plugin_library_load_failed";
#else
        const auto control_error="plugin_platform_incompatible";
#endif
        const auto expected=kind=="control"?control_error:"plugin_capability_unsupported";INFO(kind);CHECK(result.diagnostics[0].code==expected);CHECK_FALSE(registry.find("example.external_stateful"));CHECK(registry.find("builtin.gain"));CHECK(juce::File((path/"external_stateful.dll").generic_string()).loadFileAsString().toStdString()==sentinel);
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("kind",juce::String(kind));row->setProperty("code",juce::String(result.diagnostics[0].code));row->setProperty("message",juce::String(result.diagnostics[0].message));receipts.add(juce::var(row.get()));
    }
    std::ofstream(root/"capability-probe.json")<<juce::JSON::toString(receipts,false).toStdString();
}

TEST_CASE("PC-06.b native SDK logs reach production mixed Run before terminal with exact instance identity", "[PC-06.b][PC0603][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows SDK DLL and Python-enabled production workspace");
#else
    using namespace audioalgo;using namespace audioalgo::authoring;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0603-native-live/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC06_NATIVE_ROOT="<<root.generic_string()<<'\n';
    const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0602-log-negotiation/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build/good";REQUIRE(std::filesystem::is_regular_file(package/"native_logging.dll"));
    const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));const auto found=load_native_plugins(*registry,{package});REQUIRE(found.diagnostics.empty());REQUIRE(registry->find("example.sdk_logging"));
    auto service=std::make_shared<PythonAuthoring>(root/"library");auto draft=python_draft("pc06-mixed",PythonTemplate::stateful_ramp);auto definition=juce::JSON::parse(draft.declaration);definition.getDynamicObject()->setProperty("type_id","user.pc06_transform");definition["parameters"].getArray()->add(juce::JSON::parse(R"({"id":"hold","type":"bool","default":false})"));draft.declaration=juce::JSON::toString(definition,false).toStdString();
    const auto gate=root/"gate";const auto gate_literal=juce::JSON::toString(juce::var(juce::String(gate.generic_string())),true).toStdString();
    draft.source="import numpy as np\nfrom pathlib import Path\nimport time\ndef process(inputs,params,state):\n    audio=inputs['audio_in']\n    start=int(state.get('frame',0))\n    if start==0:\n        print('pc06-python-start',flush=True)\n        if params['hold']:\n            gate=Path("+gate_literal+")\n            gate.with_suffix('.entered').write_text('ready',encoding='utf-8')\n            while not gate.with_suffix('.release').is_file(): time.sleep(.005)\n    ramp=np.asarray(np.arange(start,start+audio.shape[1],dtype=np.float64)*float(params['step']),dtype=np.float32)\n    state['frame']=start+audio.shape[1]\n    return {'audio_out':audio*np.float32(params['gain'])+ramp[None,:]}\ndef reset(state,reason):\n    state.clear()\n";
    auto cancel=std::make_shared<std::atomic<bool>>(false);const auto checked=service->check(draft,cancel);REQUIRE(std::holds_alternative<ReadyPackage>(checked));const auto applied=service->apply(std::get<ReadyPackage>(checked),draft,registry,cancel);REQUIRE(std::holds_alternative<AppliedPackage>(applied));*registry=*std::get<AppliedPackage>(applied).catalog;
    const auto previous=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT",{}).toStdString();struct RestoreNativeEnv{std::string value;~RestoreNativeEnv(){_putenv_s("AUDIOALGO_AUTHORING_ROOT",value.c_str());}}restore{previous};REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT",(root/"library").string().c_str())==0);
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.2*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc06-sdk-mixed";project.revision=7;project.selected_output_node_id="output";const auto python=editor::make_node_instance(*registry,{},"user.pc06_transform","python",420,100);REQUIRE(python);auto python_node=*python;python_node.parameters["gain"]=.25;python_node.parameters["step"]=.00001;python_node.parameters["hold"]=true;
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native-A","example.sdk_logging",{},220,100},python_node,{"native-C","example.sdk_logging",{},620,100},{"output","builtin.output",{},820,100}};project.connections={{"file","audio","native-A","audio_in"},{"native-A","audio_out","python","audio_in"},{"python","audio_out","native-C","audio_in"},{"native-C","audio_out","output","audio"}};const auto file=root/"project.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));
    const auto host=juce::File::getSpecialLocation(juce::File::currentExecutableFile),cli=host.getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto host_hash=juce::SHA256(host).toHexString(),cli_hash=juce::SHA256(cli).toHexString();
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<FileDraftStorage>(root/"drafts"),false,service,{},false,{},{},{},editor::RunConfiguration{127});
    struct ReleaseNativeGate{std::filesystem::path gate;~ReleaseNativeGate(){std::ofstream(gate.replace_extension(".release"))<<"release";}}release{gate};
    workspace.open_project_file(file);REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));std::set<std::filesystem::path> seen;juce::Array<juce::var> receipts;
    const auto verify=[&](const std::filesystem::path& run,const char* origin,int repeat){const auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);REQUIRE(output.frame_count()==513);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.valid_interval()==input.valid_interval());CHECK(output.sample_rate_hz()==48000);for(int c=0;c<2;++c)for(int n=0;n<513;++n){const float a=input.channel(c)[n]*2.f;const float b=a*.25f+static_cast<float>(1e-5*static_cast<double>(n));const float expected=b*2.f;CHECK(std::abs(output.channel(c)[n]-expected)<=2e-6+1e-5*std::abs(expected));}const auto record=juce::JSON::parse(juce::File((run/"run.json").generic_string()));
        std::map<std::string,std::vector<juce::var>> native_events;
        for(const auto& directory:std::filesystem::directory_iterator(root/"run-logs")){const auto events=directory.path()/"events.jsonl";if(!std::filesystem::is_regular_file(events))continue;std::ifstream input_events(events);for(std::string line;std::getline(input_events,line);){const auto event=juce::JSON::parse(line);if(event["request_id"]==record["request_id"]&&event["source"].toString()=="native")native_events[event["node_id"].toString().toStdString()].push_back(event);}}
        for(const std::string node:{"native-A","native-C"}){INFO(node);REQUIRE(native_events.contains(node));const auto& events=native_events.at(node);CHECK(events.back()["text"].toString()=="sdk-log reset");CHECK(std::count_if(events.begin(),events.end(),[](const auto& event){return event["text"].toString()=="sdk-log prepare";})==1);CHECK(std::count_if(events.begin(),events.end(),[](const auto& event){return event["text"].toString()=="sdk-log process";})==5);}
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("run",juce::String(run.lexically_relative(root).generic_string()));row->setProperty("origin",origin);row->setProperty("repeat",repeat);row->setProperty("request",record["request_id"]);receipts.add(juce::var(row.get()));};
    for(int repeat=0;repeat<2;++repeat){click(workspace,"project.run");auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(logs);REQUIRE(status);if(repeat==0){auto entered=gate;entered.replace_extension(".entered");REQUIRE(pump([&]{return std::filesystem::is_regular_file(entered);},5000));REQUIRE(pump([&]{return logs->getText().contains("pc06-python-start");},5000));CHECK_FALSE(status->status_text().contains("Completed run"));CHECK(logs->getText().contains("sdk-log prepare"));CHECK(logs->getText().contains("node=native-A"));std::ofstream(root/"live-native-log.txt")<<logs->getText().toStdString();auto released=gate;released.replace_extension(".release");std::ofstream(released)<<"release";}const auto finished=pump([&]{return status->status_text().contains("Completed run");},10000);std::ofstream(root/("status-"+std::to_string(repeat)+".txt"))<<status->status_text().toStdString();INFO(status->status_text().toStdString());REQUIRE(finished);CHECK(logs->getText().contains("node=native-A"));CHECK(logs->getText().contains("node=native-C"));CHECK(logs->getText().contains("sdk-log process"));std::ofstream(root/("ui-log-"+std::to_string(repeat)+".txt"))<<logs->getText().toStdString();std::filesystem::path run;for(const auto& entry:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(entry.path()/"run.json")&&!seen.contains(entry.path())){REQUIRE(run.empty());run=entry.path();}REQUIRE_FALSE(run.empty());seen.insert(run);verify(run,"gui",repeat);}
    const auto destination=root/"cli-runs/run";std::filesystem::create_directories(destination.parent_path());auto future=std::async(std::launch::async,[&]{juce::ChildProcess child;if(!child.start(juce::StringArray{cli.getFullPathName(),"run",juce::String(file.generic_string()),juce::String(destination.generic_string()),"127","--plugin-dir",juce::String(package.generic_string())}))return -1;auto reader=std::async(std::launch::async,[&]{return child.readAllProcessOutput();});const auto finished=child.waitForProcessToFinish(10000);if(!finished)child.kill();std::ofstream(root/"cli-stdout.txt")<<reader.get().toStdString();return finished?static_cast<int>(child.getExitCode()):-2;});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));REQUIRE(future.get()==0);verify(destination,"cli",0);
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(juce::SHA256(host).toHexString()==host_hash);CHECK(juce::SHA256(cli).toHexString()==cli_hash);CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"native-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"native-dll.json")<<"{\"package\":"<<juce::JSON::toString(juce::var(juce::String(package.generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\",\"test_host_sha256\":\""<<host_hash<<"\",\"cli_sha256\":\""<<cli_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.c bound native SDK callback and reset allocate nothing while only the worker consumes logs", "[PC-06.c][PC0603][node_authoring]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0603-native-live/allocations"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC06_ALLOC_ROOT="<<root.generic_string()<<'\n';
    const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0602-log-negotiation/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build/good";NodeRegistry registry;REQUIRE(load_native_plugins(registry,{package}).diagnostics.empty());auto made=registry.create_processor("example.sdk_logging","allocation-probe");REQUIRE(made.processor);auto& processor=*made.processor;
    registry=NodeRegistry{};CHECK_FALSE(registry.find("example.sdk_logging"));
    NodeExecutionContext context;context.node_id="allocation-probe";context.identity={"pc06-allocation",1,"pc06-allocation-request",1};context.worker_log_io_allowed=true;context.diagnostic_directory=(root/"logs").generic_string();std::vector<NodeLogEvent> events;bool worker_callbacks=true;const auto main_thread=std::this_thread::get_id();context.on_log=[&](NodeLogEvent event){worker_callbacks=worker_callbacks&&std::this_thread::get_id()!=main_thread;events.push_back(std::move(event));};
    std::promise<std::optional<Diagnostic>> configured;auto ready=configured.get_future();std::promise<void> finished;auto may_drain=finished.get_future();auto worker=std::async(std::launch::async,[&]{configured.set_value(processor.freeze_task(context));may_drain.wait();return processor.drain_logs(context,true);});
    struct FinishLogWorker{std::promise<void>& promise;bool sent=false;~FinishLogWorker(){if(!sent)promise.set_value();}}finish{finished};
    REQUIRE_FALSE(ready.get());NodeParameters parameters;REQUIRE_FALSE(processor.prepare({{48000,1},127,&parameters}));std::array<float,127> input{},output{};input.fill(.125f);const float* input_pointer=input.data();float* output_pointer=output.data();std::array<AudioInputPortBlock,1> inputs{{{"audio_in",{&input_pointer,1}}}};std::array<AudioOutputPortBlock,1> outputs{{{"audio_out",{&output_pointer,1}}}};bool processed=true;
    test::begin_allocation_count();for(int n=0;n<10;++n){const auto result=processor.process_named_block({48000,127,inputs,outputs});processed=processed&&std::holds_alternative<AudioBlockResult>(result);}processor.reset();const auto allocations=test::end_allocation_count();CHECK(allocations==0);CHECK(processed);CHECK(std::all_of(output.begin(),output.end(),[](float value){return value==.25f;}));CHECK(events.empty());CHECK_FALSE(std::filesystem::exists(root/"logs"));
    finished.set_value();finish.sent=true;const auto error=worker.get();if(error)INFO(error->code+": "+error->message);REQUIRE_FALSE(error);CHECK(worker_callbacks);REQUIRE(events.size()==12);CHECK(events.front().text=="sdk-log prepare");CHECK(events.back().text=="sdk-log reset");for(std::size_t n=0;n<events.size();++n){CHECK(events[n].sequence==n+1);CHECK(events[n].identity==context.identity);CHECK(events[n].node_id==context.node_id);CHECK(events[n].source=="native");}
    juce::DynamicObject::Ptr receipt=new juce::DynamicObject;receipt->setProperty("allocations",static_cast<juce::int64>(allocations));receipt->setProperty("events",static_cast<int>(events.size()));receipt->setProperty("callbacks_worker_only",worker_callbacks);receipt->setProperty("raw_file",juce::String(events.front().raw_log_file));receipt->setProperty("dll",juce::String((package/"native_logging.dll").generic_string()));std::ofstream(root/"allocation-probe.json")<<juce::JSON::toString(juce::var(receipt.get()),false).toStdString();
#endif
}

TEST_CASE("PC-06.b real SDK Unicode levels and embedded NUL survive production Run archive history and copy", "[PC-06.b][PC-04.a][PC0604][node_authoring][component]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0604-native-text/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC0604_TEXT_ROOT="<<root.generic_string()<<'\n';
    const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0604-native-text/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";
    auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));const auto discovery=load_native_plugins(*registry,{package});REQUIRE(discovery.diagnostics.empty());REQUIRE(registry->find("example.sdk_logging"));
    const auto host=juce::File::getSpecialLocation(juce::File::currentExecutableFile),cli=host.getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto host_hash=juce::SHA256(host).toHexString(),cli_hash=juce::SHA256(cli).toHexString(),dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc0604-native-text";project.revision=9;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native","example.sdk_logging",{},220,100},{"output","builtin.output",{},420,100}};project.connections={{"file","audio","native","audio_in"},{"native","audio_out","output","audio"}};const auto file=root/"project.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));
    constexpr char info_bytes[]="info before\0after \"' $() `literal` \x01\x1b" "END\r\n";
    const std::array<std::string,4> texts{"debug \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8e\xa7\r\n",std::string(info_bytes,sizeof(info_bytes)-1),"warning <b>plain</b>\n","error terminal\n"};const std::array<std::string,4> levels{"debug","info","warning","error"};juce::Array<juce::var> receipts;
    const auto verify=[&](const std::filesystem::path& run,const char* origin){const auto record=juce::JSON::parse(juce::File((run/"run.json").generic_string()));REQUIRE(record.isObject());const auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);CHECK(output.frame_count()==513);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.sample_rate_hz()==48000);CHECK(output.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(output.channel(c)[n]==input.channel(c)[n]*2.f);
        std::optional<editor::RunLogArchiveReference> archive;for(const auto& entry:std::filesystem::directory_iterator(root/"run-logs")){const auto events=entry.path()/"events.jsonl";if(!std::filesystem::is_regular_file(events))continue;std::ifstream in(events);std::string line;std::getline(in,line);const auto event=juce::JSON::parse(line);if(event["request_id"]==record["request_id"]){REQUIRE_FALSE(archive);archive=editor::RunLogArchiveReference{{project.project_id,static_cast<std::uint64_t>(static_cast<juce::int64>(record["session_epoch"])),record["request_id"].toString().toStdString(),9},events,1};}}
        REQUIRE(archive);const auto tail=std::async(std::launch::async,[&]{return editor::RunLogArchiveReader::load(*archive,[]{return false;});}).get();REQUIRE(tail.notice.empty());REQUIRE(tail.events.size()==4);std::string raw_expected;std::uint64_t bytes=0;
        for(std::size_t n=0;n<4;++n){INFO(n);const auto& event=tail.events[n];CHECK(event.text==texts[n]);CHECK(event.level==levels[n]);CHECK(event.source=="native");CHECK(event.identity==archive->identity);CHECK(event.node_id=="native");CHECK(event.package_id=="example.sdk_logging.text");CHECK(event.content_sha256==dll_hash.toStdString());CHECK(event.sequence==n+1);bytes+=texts[n].size();CHECK(event.total_bytes==bytes);CHECK(event.discarded_bytes==0);raw_expected+=texts[n];}
        std::ifstream raw(std::filesystem::u8path(tail.events.front().raw_log_file),std::ios::binary);const std::string actual((std::istreambuf_iterator<char>(raw)),{});CHECK(actual==raw_expected);
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("run",juce::String(run.lexically_relative(root).generic_string()));row->setProperty("origin",origin);row->setProperty("request",record["request_id"]);row->setProperty("archive",juce::String(archive->file.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));};
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<authoring::FileDraftStorage>(root/"drafts"),false,std::make_shared<authoring::PythonAuthoring>(root/"library"),{},false,{},{},{},editor::RunConfiguration{127});
    workspace.open_project_file(file);REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));click(workspace,"project.run");auto* status=control<editor::RunStatusPanel>(workspace,"project.status");REQUIRE(status);REQUIRE(pump([&]{return status->status_text().contains("Completed run");}));auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(logs);auto* panel=control<editor::RunLogPanel>(workspace,"project.run_log_panel");
    // Locate the containing production panel without depending on a new UI hook.
    if(!panel)for(auto* parent=logs->getParentComponent();parent;parent=parent->getParentComponent())if((panel=dynamic_cast<editor::RunLogPanel*>(parent)))break;REQUIRE(panel);
    const auto assert_display=[&]{const auto displayed=logs->getText();CHECK(displayed.contains(editor::utf8_literal("debug 中文 🎧\n")));CHECK(displayed.contains("info before\\0after \"' $() `literal` \\x01\\x1bEND\n"));CHECK(displayed.contains("warning <b>plain</b>"));CHECK(displayed.contains("error terminal"));CHECK(panel->text_for_copy()==displayed);};
    assert_display();std::ofstream(root/"ui-live.txt",std::ios::binary)<<logs->getText().toStdString();std::filesystem::path gui_run;for(const auto& entry:std::filesystem::directory_iterator(root/"runs"))if(std::filesystem::is_regular_file(entry.path()/"run.json")){REQUIRE(gui_run.empty());gui_run=entry.path();}REQUIRE_FALSE(gui_run.empty());verify(gui_run,"gui");
    auto* filter=control<juce::ComboBox>(workspace,"project.run_logs.level_filter");REQUIRE(filter);for(const auto& level:levels){int id=0;for(int n=0;n<filter->getNumItems();++n)if(filter->getItemText(n)==juce::String(level))id=filter->getItemId(n);REQUIRE(id>1);filter->setSelectedId(id,juce::sendNotificationSync);CHECK(logs->getText().contains(juce::String(level)));for(const auto& other:levels)if(other!=level)CHECK_FALSE(logs->getText().contains(juce::String(other)+" "));std::ofstream(root/("filter-"+level+".txt"),std::ios::binary)<<logs->getText().toStdString();}filter->setSelectedId(1,juce::sendNotificationSync);assert_display();
    click(workspace,"project.run_logs.history_refresh");auto* history=control<juce::ComboBox>(workspace,"project.run_logs.history");REQUIRE(history);REQUIRE(pump([&]{return history->getNumItems()==2&&history->isEnabled();}));auto* history_status=control<juce::Label>(workspace,"project.run_logs.history_status");REQUIRE(history_status);history->setSelectedId(history->getItemId(1),juce::sendNotificationSync);REQUIRE(pump([&]{return history->isEnabled()&&history_status->getText().contains("Archived Run");}));assert_display();std::ofstream(root/"ui-history.txt",std::ios::binary)<<logs->getText().toStdString();history->setSelectedId(1,juce::sendNotificationSync);REQUIRE(pump([&]{return history->isEnabled()&&history_status->getText()=="Current Run";}));assert_display();
    const auto destination=root/"cli-runs/run";std::filesystem::create_directories(destination.parent_path());auto child=std::async(std::launch::async,[&]{juce::ChildProcess process;if(!process.start(juce::StringArray{cli.getFullPathName(),"run",juce::String(file.generic_string()),juce::String(destination.generic_string()),"127","--plugin-dir",juce::String(package.generic_string())}))return -1;auto reader=std::async(std::launch::async,[&]{return process.readAllProcessOutput();});const auto finished=process.waitForProcessToFinish(10000);if(!finished)process.kill();std::ofstream(root/"cli-stdout.txt")<<reader.get().toStdString();return finished?static_cast<int>(process.getExitCode()):-2;});REQUIRE(pump([&]{return child.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));REQUIRE(child.get()==0);verify(destination,"cli");
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(juce::SHA256(host).toHexString()==host_hash);CHECK(juce::SHA256(cli).toHexString()==cli_hash);CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"text-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\",\"test_host_sha256\":\""<<host_hash<<"\",\"cli_sha256\":\""<<cli_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.a real legacy and SDK DLL nodes explain log capability in library Inspector and fresh CLI", "[PC-06.a][PC0605][node_authoring][component]") {
#if !defined(_WIN32)
    SKIP("Requires actual Windows SDK and preserved legacy DLLs");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0605-native-capability/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC0605_CAP_ROOT="<<root.generic_string()<<'\n';const auto reports=juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops");
    const auto external=juce::JSON::parse(reports.getChildFile("PC0602-log-negotiation/external-path.json"));REQUIRE(external.isObject());const auto sdk=std::filesystem::path(external["external"].toString().toStdString())/"build";const auto baseline=juce::JSON::parse(reports.getChildFile("PC0601-native-log-queue/baseline-identity.json"));REQUIRE(baseline.isObject());const auto old_dll=std::filesystem::path(baseline["artifacts"][1]["path"].toString().toStdString());const auto old_hash=baseline["artifacts"][1]["sha256"].toString();CHECK(juce::SHA256(juce::File(old_dll.generic_string())).toHexString()==old_hash);
    const auto cli=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto cli_hash=juce::SHA256(cli).toHexString();juce::Array<juce::var> receipts;
    for(const std::string mode:{"legacy","absent","good"}) {
        INFO(mode);const auto package=mode=="legacy"?old_dll.parent_path():sdk/mode;const auto dll=mode=="legacy"?old_dll:package/"native_logging.dll";const auto digest=juce::SHA256(juce::File(dll.generic_string())).toHexString();const auto type=mode=="legacy"?"example.external_stateful":"example.sdk_logging";const auto capability=mode=="good"?"sdk_v1":"unavailable";const auto caption=mode=="good"?"User logs: available.":"User logs: unavailable in this version.";
        NodeRegistry registry;REQUIRE(register_builtin_nodes(registry));const auto loaded=load_native_plugins(registry,{package});REQUIRE(loaded.diagnostics.empty());const auto* descriptor=registry.find(type);REQUIRE(descriptor);const auto metadata=juce::JSON::parse(descriptor->backend_metadata_json);CHECK(metadata["user_logs"].toString()==capability);
        editor::EditorController controller(registry);std::optional<editor::ProjectEditOutcome> added;editor::NodeLibrary library(registry,[&](std::string id){added=controller.add_node(id,"capability-node",200,100);},[]{return std::uint64_t{1};});library.setSize(220,600);auto* button=control<editor::NodeLibraryButton>(library,"library."+juce::String(type));REQUIRE(button);CHECK(button->getTooltip().contains(caption));click(library,("library."+std::string(type)).c_str());REQUIRE(added);REQUIRE(added->status==editor::ProjectEditStatus::applied);CHECK(controller.project().nodes.size()==1);
        editor::NodeSelectionState selection;editor::NodeInspector inspector(controller,selection,[]{});inspector.set_descriptor_registry(registry);inspector.setSize(280,500);inspector.show_node("capability-node");
        std::function<juce::Label*(juce::Component&)> details=[&](juce::Component& parent)->juce::Label*{if(auto* label=dynamic_cast<juce::Label*>(&parent);label&&label->getText().contains("ID capability-node"))return label;for(auto* child:parent.getChildren())if(auto* label=details(*child))return label;return nullptr;};auto* label=details(inspector);REQUIRE(label);CHECK(label->getText().contains(caption));CHECK(label->getText().contains(type));const auto revision=controller.revision();inspector.show_node("capability-node");CHECK(controller.revision()==revision);
        auto* builtin=control<editor::NodeLibraryButton>(library,"library.builtin.gain");REQUIRE(builtin);CHECK_FALSE(builtin->getTooltip().contains("unavailable"));library.refresh(registry);button=control<editor::NodeLibraryButton>(library,"library."+juce::String(type));REQUIRE(button);CHECK(button->getTooltip().contains(caption));
        auto future=std::async(std::launch::async,[&]{juce::ChildProcess child;if(!child.start(juce::StringArray{cli.getFullPathName(),"--plugin-dir",juce::String(package.generic_string()),"--list-nodes"}))return std::pair<int,juce::String>{-1,{}};const auto text=child.readAllProcessOutput();const auto finished=child.waitForProcessToFinish(10000);if(!finished)child.kill();return std::pair<int,juce::String>{finished?static_cast<int>(child.getExitCode()):-2,text};});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));const auto [exit,text]=future.get();REQUIRE(exit==0);std::ofstream(root/("cli-"+mode+".json"))<<text.toStdString();const auto catalog=juce::JSON::parse(text);REQUIRE(catalog["nodes"].isArray());bool found=false;for(const auto& node:*catalog["nodes"].getArray())if(node["type_id"].toString()==type){found=true;CHECK(node["backend_descriptor"]["user_logs"].toString()==capability);CHECK(node["library_sha256"].toString()==digest);}CHECK(found);CHECK(juce::SHA256(juce::File(dll.generic_string())).toHexString()==digest);
        juce::DynamicObject::Ptr receipt=new juce::DynamicObject;receipt->setProperty("mode",juce::String(mode));receipt->setProperty("dll",juce::String(dll.generic_string()));receipt->setProperty("dll_sha256",digest);receipt->setProperty("capability",capability);receipt->setProperty("library_tooltip",button->getTooltip());receipt->setProperty("inspector_text",label->getText());receipts.add(juce::var(receipt.get()));
    }
    CHECK(juce::SHA256(cli).toHexString()==cli_hash);std::ofstream(root/"capability-demo.json")<<juce::JSON::toString(receipts,false).toStdString();
#endif
}

TEST_CASE("PC-04.b real native task shares exact sixteen MiB log budget across instances and recovers", "[PC-04.b][PC-06.c][PC0606][node_authoring]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;constexpr std::int64_t limit=16*1024*1024;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0606-native-budget/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC0606_BUDGET_ROOT="<<root.generic_string()<<'\n';std::filesystem::create_directories(root/"runs");const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0606-native-budget/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";NodeRegistry registry;REQUIRE(register_builtin_nodes(registry));const auto discovery=load_native_plugins(registry,{package});REQUIRE(discovery.diagnostics.empty());RunService service(registry);
    const auto cli=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto cli_hash=juce::SHA256(cli).toHexString(),dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    const auto graph=[&](const std::vector<std::int64_t>& bytes){ProjectModel p;p.schema_version=2;p.project_id="pc0606-native-budget";p.revision=11;p.selected_output_node_id="output";p.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native-A","example.sdk_logging",{{"log_bytes",bytes[0]}},220,100},{"output","builtin.output",{},620,100}};p.connections={{"file","audio","native-A","audio_in"}};if(bytes.size()==2){p.nodes.push_back({"native-B","example.sdk_logging",{{"log_bytes",bytes[1]}},420,100});p.connections.push_back({"native-A","audio_out","native-B","audio_in"});}p.connections.push_back({bytes.size()==2?"native-B":"native-A","audio_out","output","audio"});return p;};
    const auto numerical=[&](const std::filesystem::path& run,std::size_t instances){const auto read=read_wav_file(run/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);CHECK(output.frame_count()==513);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.sample_rate_hz()==48000);CHECK(output.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(output.channel(c)[n]==input.channel(c)[n]*(instances==2?4.f:2.f));};
    const auto perform=[&](const ProjectModel& project,const std::string& name,std::set<std::string>& raw,std::set<std::string>& reports){RunRequest request{project,root/"project.aaproj",root/"runs"/name,1};request.identity={project.project_id,23,juce::Uuid().toString().toStdString(),project.revision};request.on_log=[&](NodeLogEvent event){if(!event.raw_log_file.empty())raw.insert(event.raw_log_file);if(!event.report_file.empty())reports.insert(event.report_file);};auto future=std::async(std::launch::async,[&]{return service.run(request);});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},30000));auto outcome=future.get();const auto* failure=std::get_if<Diagnostic>(&outcome);std::ofstream(root/(name+"-outcome.txt"))<<(failure?failure->code+": "+failure->message:std::string("success"));return outcome;};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"project.aaproj",graph({0,0}))));
    std::set<std::string> unused_raw,unused_reports;const auto baseline=perform(graph({0,0}),"baseline",unused_raw,unused_reports);REQUIRE(std::holds_alternative<RunSuccess>(baseline));numerical(root/"runs/baseline",2);const auto prior_hash=juce::SHA256(juce::File((root/"runs/baseline/output.wav").generic_string())).toHexString();juce::Array<juce::var> receipts;
    const std::vector<std::pair<std::string,std::vector<std::int64_t>>> cases{{"single-exact",{limit}},{"single-plus-one",{limit+1}},{"shared-exact",{limit/2,limit/2}},{"shared-plus-one",{limit/2,limit/2+1}},{"shared-flood",{limit,limit}}};
    for(const auto& [name,bytes]:cases){INFO(name);const bool succeeds=name=="single-exact"||name=="shared-exact";const auto project=graph(bytes);REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"project.aaproj",project)));std::set<std::string> raw,reports;const auto result=perform(project,name,raw,reports);std::uint64_t captured=0;for(const auto& file:raw)captured+=std::filesystem::file_size(std::filesystem::u8path(file));CHECK(captured<=static_cast<std::uint64_t>(limit));const auto* error=std::get_if<Diagnostic>(&result);CHECK(std::holds_alternative<RunSuccess>(result)==succeeds);
        if(succeeds){REQUIRE(std::holds_alternative<RunSuccess>(result));numerical(root/"runs"/name,bytes.size());CHECK(captured==static_cast<std::uint64_t>(limit));}else{CHECK(error!=nullptr);if(error){CHECK(error->code=="log_limit");CHECK((error->node_id=="native-A"||error->node_id=="native-B"));}CHECK_FALSE(std::filesystem::exists(root/"runs"/name));CHECK_FALSE(std::filesystem::exists(root/"runs"/name/"complete.json"));}
        CHECK(juce::SHA256(juce::File((root/"runs/baseline/output.wav").generic_string())).toHexString()==prior_hash);CHECK(std::filesystem::is_regular_file(root/"runs/baseline/complete.json"));REQUIRE(reports.size()==1);juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("name",juce::String(name));row->setProperty("success",std::holds_alternative<RunSuccess>(result));row->setProperty("code",error?juce::String(error->code):juce::String{});row->setProperty("captured_bytes",static_cast<juce::int64>(captured));row->setProperty("archive",juce::String(*reports.begin()));juce::Array<juce::var> targets;for(auto value:bytes)targets.add(static_cast<juce::int64>(value));row->setProperty("log_bytes",targets);receipts.add(juce::var(row.get()));
        auto recovered_project=graph(std::vector<std::int64_t>(bytes.size(),0));std::set<std::string> recovered_raw,recovered_reports;const auto recovered=perform(recovered_project,"recovery-"+name,recovered_raw,recovered_reports);REQUIRE(std::holds_alternative<RunSuccess>(recovered));numerical(root/"runs"/("recovery-"+name),bytes.size());CHECK(recovered_raw.empty());}
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"cli-shared-plus.aaproj",graph({limit/2,limit/2+1}))));std::filesystem::create_directories(root/"cli-runs");auto future=std::async(std::launch::async,[&]{juce::ChildProcess child;if(!child.start(juce::StringArray{cli.getFullPathName(),"run",juce::String((root/"cli-shared-plus.aaproj").generic_string()),juce::String((root/"cli-runs/shared-plus-one").generic_string()),"1","--plugin-dir",juce::String(package.generic_string())}))return std::pair<int,juce::String>{-1,{}};const auto text=child.readAllProcessOutput();const auto finished=child.waitForProcessToFinish(30000);if(!finished)child.kill();return std::pair<int,juce::String>{finished?static_cast<int>(child.getExitCode()):-2,text};});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},31000));const auto [exit,text]=future.get();CHECK(exit==1);CHECK(text.startsWith("log_limit [node="));CHECK_FALSE(std::filesystem::exists(root/"cli-runs/shared-plus-one"));std::ofstream(root/"cli-stdout.txt")<<text.toStdString();std::ofstream(root/"budget-demo.json")<<juce::JSON::toString(receipts,false).toStdString();CHECK(juce::SHA256(cli).toHexString()==cli_hash);CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\",\"cli_sha256\":\""<<cli_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.c real native queue and message rejection preserve prefix fail safely and recover through production controls", "[PC-06.c][PC0607][node_authoring][component]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0607-native-rejection/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root);std::cout<<"PC0607_REJECT_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0607-native-rejection/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));const auto discovery=load_native_plugins(*registry,{package});REQUIRE(discovery.diagnostics.empty());
    const auto cli=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");REQUIRE(cli.existsAsFile());const auto cli_hash=juce::SHA256(cli).toHexString(),dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();
    const auto previous=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT",{}).toStdString();struct RestoreFaultEnv{std::string value;~RestoreFaultEnv(){_putenv_s("AUDIOALGO_AUTHORING_ROOT",value.c_str());}}restore{previous};REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT",(root/"library").string().c_str())==0);
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc0607-native-rejection";project.revision=13;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native","example.sdk_logging",{{"mode",std::string("ok")}},220,100},{"output","builtin.output",{},420,100}};project.connections={{"file","audio","native","audio_in"},{"native","audio_out","output","audio"}};const auto file=root/"project.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));
    editor::EditorComponent workspace(registry,std::make_shared<FileDocuments>(),std::make_shared<ProjectResourceService>(),std::make_shared<Preview>(),std::make_shared<authoring::FileDraftStorage>(root/"drafts"),false,std::make_shared<authoring::PythonAuthoring>(root/"library"),{},false,{},{},{},editor::RunConfiguration{127});workspace.open_project_file(file);REQUIRE(pump([&]{return workspace.controller().project().project_id==project.project_id;}));auto* canvas=control<editor::GraphCanvas>(workspace,"project.canvas");REQUIRE(canvas);canvas->select("native");
    auto* status=control<editor::RunStatusPanel>(workspace,"project.status");auto* logs=control<editor::RunLogTextView>(workspace,"project.run_logs");REQUIRE(status);REQUIRE(logs);std::set<std::filesystem::path> archives,outputs;std::map<std::filesystem::path,juce::String> old_outputs;juce::Array<juce::var> receipts;const std::map<std::string,int> codes{{"ok",0},{"empty",0},{"full",1},{"utf8",4},{"length",3},{"size",2},{"version",2},{"level",2},{"null",2}};
    const auto run=[&](const std::string& mode,const std::string& label){const bool success=mode=="ok"||mode=="empty";const auto code=success?"":mode=="full"?"native_log_queue_full":"native_log_message_invalid";auto* choice=control<juce::ComboBox>(workspace,"parameter.mode");REQUIRE(choice);int id=0;for(int n=0;n<choice->getNumItems();++n)if(choice->getItemText(n)==juce::String(mode))id=choice->getItemId(n);REQUIRE(id>0);choice->setSelectedId(id,juce::sendNotificationSync);click(workspace,"project.save");REQUIRE(pump([&]{return !workspace.controller().dirty()&&status->status_text().startsWith("Saved ")&&control<juce::TextButton>(workspace,"project.run")->isEnabled();}));REQUIRE(std::filesystem::copy_file(file,root/(label+".aaproj")));click(workspace,"project.run");const auto terminal=pump([&]{return control<juce::TextButton>(workspace,"project.run")->isEnabled()&&(success?status->status_text().contains("Completed run"):status->status_text().contains(code));},10000);std::ofstream(root/(label+"-status.txt"))<<status->status_text().toStdString();INFO(mode);INFO(status->status_text().toStdString());REQUIRE(terminal);CHECK(logs->getText().contains("fault-prefix"));CHECK(logs->getText().contains("node=native"));CHECK(logs->getText().contains(dll_hash));if(!success){CHECK_FALSE(status->status_text().contains("Completed run"));CHECK(logs->getText().contains(code));}if(mode=="full")CHECK(logs->getText().contains("1 messages / 1024 bytes rejected"));else CHECK(logs->getText().contains("fault-status="+juce::String(codes.at(mode))));std::ofstream(root/(label+"-ui.txt"),std::ios::binary)<<logs->getText().toStdString();
        std::filesystem::path archive;for(const auto& entry:std::filesystem::directory_iterator(root/"run-logs"))if(std::filesystem::is_regular_file(entry.path()/"events.jsonl")&&!archives.contains(entry.path()/"events.jsonl")){REQUIRE(archive.empty());archive=entry.path()/"events.jsonl";}REQUIRE_FALSE(archive.empty());archives.insert(archive);std::ifstream events(archive);std::vector<juce::var> native,diagnostic;std::string request;for(std::string line;std::getline(events,line);){const auto event=juce::JSON::parse(line);REQUIRE(event.isObject());if(request.empty())request=event["request_id"].toString().toStdString();CHECK(event["request_id"].toString()==juce::String(request));CHECK(event["node_id"].toString()=="native");CHECK(event["package_id"].toString()=="example.sdk_logging.faults");CHECK(event["content_sha256"].toString()==dll_hash);if(event["source"].toString()=="native")native.push_back(event);else diagnostic.push_back(event);}REQUIRE_FALSE(native.empty());CHECK(native.front()["text"].toString()=="fault-prefix\n");CHECK(native.size()==(mode=="full"?64:mode=="empty"?3:2));CHECK(diagnostic.size()==(success?0:1));if(!success)CHECK(diagnostic.front()["text"].toString().startsWith(juce::String(code)+":"));
        const auto raw=std::filesystem::u8path(native.front()["raw_log_file"].toString().toStdString());std::ifstream input_raw(raw,std::ios::binary);const std::string raw_text((std::istreambuf_iterator<char>(input_raw)),{});const auto expected=mode=="full"?std::string("fault-prefix\n")+std::string(63*1024,'F'):std::string("fault-prefix\nfault-status=")+std::to_string(codes.at(mode))+"\n";CHECK(raw_text==expected);CHECK(static_cast<juce::int64>(native.back()["total_bytes"])==static_cast<juce::int64>(mode=="full"?65549:28));CHECK(static_cast<juce::int64>(native.back()["discarded_bytes"])==(mode=="full"?1024:0));if(mode=="empty")CHECK(native[1]["text"].toString().isEmpty());CHECK(logs->getText().contains(juce::String(request)));
        std::filesystem::path created;for(const auto& entry:std::filesystem::directory_iterator(root/"runs")){REQUIRE(std::filesystem::is_regular_file(entry.path()/"complete.json"));if(!outputs.contains(entry.path())){REQUIRE(created.empty());created=entry.path();}}CHECK(created.empty()!=success);if(success){REQUIRE_FALSE(created.empty());outputs.insert(created);const auto read=read_wav_file(created/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);CHECK(output.frame_count()==513);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.sample_rate_hz()==48000);CHECK(output.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(output.channel(c)[n]==input.channel(c)[n]*2.f);old_outputs[created/"output.wav"]=juce::SHA256(juce::File((created/"output.wav").generic_string())).toHexString();}for(const auto& [path,hash]:old_outputs)CHECK(juce::SHA256(juce::File(path.generic_string())).toHexString()==hash);
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("mode",juce::String(mode));row->setProperty("label",juce::String(label));row->setProperty("success",success);row->setProperty("code",code);row->setProperty("request",juce::String(request));row->setProperty("archive",juce::String(archive.lexically_relative(root).generic_string()));row->setProperty("run",juce::String(created.empty()?std::string{}:created.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));};
    run("ok","baseline");run("empty","positive-empty");for(const std::string mode:{"full","utf8","length","size","version","level","null"}){run(mode,"failure-"+mode);run("ok","recovery-"+mode);}CHECK(outputs.size()==9);CHECK(archives.size()==16);
    std::filesystem::create_directories(root/"cli-runs");for(const std::string mode:{"full","utf8"}){auto future=std::async(std::launch::async,[&]{juce::ChildProcess child;if(!child.start(juce::StringArray{cli.getFullPathName(),"run",juce::String((root/("failure-"+mode+".aaproj")).generic_string()),juce::String((root/"cli-runs"/mode).generic_string()),"127","--plugin-dir",juce::String(package.generic_string())}))return std::pair<int,juce::String>{-1,{}};const auto text=child.readAllProcessOutput();const auto finished=child.waitForProcessToFinish(10000);if(!finished)child.kill();return std::pair<int,juce::String>{finished?static_cast<int>(child.getExitCode()):-2,text};});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},11000));const auto [exit,text]=future.get();CHECK(exit==1);CHECK(text.startsWith(mode=="full"?"native_log_queue_full [node=native]":"native_log_message_invalid [node=native]"));CHECK_FALSE(std::filesystem::exists(root/"cli-runs"/mode));std::ofstream(root/("cli-"+mode+".txt"))<<text.toStdString();}
    int drained=0;workspace.begin_shutdown([&]{++drained;});REQUIRE(pump([&]{return drained==1;}));CHECK(juce::SHA256(cli).toHexString()==cli_hash);CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"rejection-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\",\"cli_sha256\":\""<<cli_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.c independent native regions retain separate sixteen MiB budgets through actual mixed Run and recovery", "[PC-06.c][PC0608][node_authoring]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires actual Windows SDK DLL and enabled Python runtime");
#else
    using namespace audioalgo;using namespace audioalgo::authoring;juce::ScopedJuceInitialiser_GUI gui;constexpr std::int64_t limit=16*1024*1024;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0608-region-budget/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root/"runs");std::cout<<"PC0608_REGION_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0606-native-budget/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));const auto discovery=load_native_plugins(*registry,{package});REQUIRE(discovery.diagnostics.empty());const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();
    auto checks=std::make_shared<PythonAuthoring>(root/"library");auto draft=python_draft("pc0608-regions");auto declaration=juce::JSON::parse(draft.declaration);declaration.getDynamicObject()->setProperty("type_id","user.pc0608_quarter");draft.declaration=juce::JSON::toString(declaration,false).toStdString();draft.source="import numpy as np\ndef process(inputs,params,state):\n    return {'audio_out':inputs['audio_in']*np.float32(.25)}\n";auto cancel=std::make_shared<std::atomic<bool>>(false);const auto checked=checks->check(draft,cancel);REQUIRE(std::holds_alternative<ReadyPackage>(checked));const auto applied=checks->apply(std::get<ReadyPackage>(checked),draft,registry,cancel);REQUIRE(std::holds_alternative<AppliedPackage>(applied));*registry=*std::get<AppliedPackage>(applied).catalog;RunService service(registry);
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));const auto python=editor::make_node_instance(*registry,{},"user.pc0608_quarter","python",420,100);REQUIRE(python);ProjectModel project;project.schema_version=2;project.project_id="pc0608-region-budget";project.revision=14;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native-A","example.sdk_logging",{{"log_bytes",limit}},220,100},*python,{"native-C","example.sdk_logging",{{"log_bytes",limit}},620,100},{"output","builtin.output",{},820,100}};project.connections={{"file","audio","native-A","audio_in"},{"native-A","audio_out","python","audio_in"},{"python","audio_out","native-C","audio_in"},{"native-C","audio_out","output","audio"}};juce::Array<juce::var> receipts;std::filesystem::path baseline;juce::String baseline_hash;
    for(int phase=0;phase<3;++phase){const std::string name=phase==0?"exact":phase==1?"overflow-C":"recovery";project.revision=14+phase;project.nodes[3].parameters["log_bytes"]=limit+(phase==1?1:0);const auto file=root/(name+".aaproj");REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));RunRequest request{project,file,root/"runs"/name,1};request.identity={project.project_id,31,juce::Uuid().toString().toStdString(),project.revision};std::map<std::string,std::string> raw;std::set<std::string> archives;request.on_log=[&](NodeLogEvent event){if(event.source=="native")raw[event.node_id]=event.raw_log_file;if(!event.report_file.empty())archives.insert(event.report_file);};auto future=std::async(std::launch::async,[&]{return service.run(request);});REQUIRE(pump([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},30000));const auto result=future.get();const auto* error=std::get_if<Diagnostic>(&result);std::ofstream(root/(name+"-outcome.txt"))<<(error?error->code+": "+error->message:std::string("success"));INFO(name);if(error)INFO(error->code+": "+error->message);CHECK(std::holds_alternative<RunSuccess>(result)==(phase!=1));REQUIRE(raw.size()==2);CHECK(std::filesystem::file_size(std::filesystem::u8path(raw.at("native-A")))==static_cast<std::uint64_t>(limit));CHECK(std::filesystem::file_size(std::filesystem::u8path(raw.at("native-C")))==static_cast<std::uint64_t>(limit));REQUIRE(archives.size()==1);
        if(phase==1){REQUIRE(error);CHECK(error->code=="log_limit");CHECK(error->node_id=="native-C");CHECK_FALSE(std::filesystem::exists(request.output_directory));}else{REQUIRE(std::holds_alternative<RunSuccess>(result));const auto read=read_wav_file(request.output_directory/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& output=std::get<AudioBuffer>(read);CHECK(output.frame_count()==513);CHECK(output.channel_ids()==input.channel_ids());CHECK(output.sample_rate_hz()==48000);CHECK(output.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(output.channel(c)[n]==input.channel(c)[n]);const auto record=juce::JSON::parse(juce::File((request.output_directory/"run.json").generic_string()));REQUIRE(record["tasks"].isArray());REQUIRE(record["tasks"].size()==5);CHECK(record["tasks"][1]["kind"].toString()=="native_region");CHECK(record["tasks"][2]["kind"].toString()=="offline");CHECK(record["tasks"][3]["kind"].toString()=="native_region");if(phase==0){baseline=request.output_directory/"output.wav";baseline_hash=juce::SHA256(juce::File(baseline.generic_string())).toHexString();}}
        CHECK(juce::SHA256(juce::File(baseline.generic_string())).toHexString()==baseline_hash);CHECK(std::filesystem::is_regular_file(baseline.parent_path()/"complete.json"));juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("name",juce::String(name));row->setProperty("request",juce::String(request.identity.request_id));row->setProperty("revision",static_cast<juce::int64>(project.revision));row->setProperty("success",std::holds_alternative<RunSuccess>(result));row->setProperty("code",error?juce::String(error->code):juce::String{});row->setProperty("node",error?juce::String(error->node_id):juce::String{});row->setProperty("archive",juce::String(*archives.begin()));row->setProperty("run",juce::String(request.output_directory.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));}
    CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"region-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.c parallel native consumers own separate near-limit budgets and one failure cannot consume another quota", "[PC-06.c][PC0609][node_authoring]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;constexpr std::int64_t limit=16*1024*1024;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0609-parallel-budget/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root/"runs");std::cout<<"PC0609_PARALLEL_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0606-native-budget/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));REQUIRE(load_native_plugins(*registry,{package}).diagnostics.empty());const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();std::array<RunService,2> services{RunService(registry),RunService(registry)};
    std::array<AudioBuffer,2> inputs{AudioBuffer(48000,{"ch0","ch1"},513),AudioBuffer(48000,{"ch0","ch1"},513)};for(int i=0;i<2;++i){for(int c=0;c<2;++c)for(int n=0;n<513;++n)inputs[i].mutable_channel(c)[n]=static_cast<float>(i==0?.125*std::sin(.03*(n+1))+.02*c:.075*std::cos(.017*(n+1))+.01*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/(i==0?"input-A.wav":"input-B.wav"),inputs[i],WavEncoding::float32)));}juce::Array<juce::var> receipts,gates;std::map<std::filesystem::path,juce::String> previous;
    for(int pair=0;pair<2;++pair){std::array<RunRequest,2> requests;std::array<std::set<std::string>,2> raw,archives;std::array<std::atomic<bool>,2> entered{};std::array<bool,2> observed{};std::array<NodeLogEvent,2> first;std::array<std::thread::id,2> workers;std::promise<void> release;const auto allowed=release.get_future().share();std::array<std::future<RunOutcome>,2> jobs;
        struct DrainParallelJobs{std::promise<void>& release;std::array<std::future<RunOutcome>,2>& jobs;bool sent=false;void proceed(){if(!sent){sent=true;release.set_value();}}~DrainParallelJobs(){proceed();for(;;){bool ready=true;for(auto& job:jobs)if(job.valid()&&job.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)ready=false;if(ready)break;juce::MessageManager::getInstance()->runDispatchLoopUntil(10);}}}drain{release,jobs};
        for(int i=0;i<2;++i){ProjectModel project;project.schema_version=2;project.project_id="pc0609-parallel-native";project.revision=19;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string(i==0?"input-A.wav":"input-B.wav")}},20,100},{"native","example.sdk_logging",{{"log_bytes",limit+(pair==1&&i==0?1:0)}},220,100},{"output","builtin.output",{},420,100}};project.connections={{"file","audio","native","audio_in"},{"native","audio_out","output","audio"}};const auto label="pair-"+std::to_string(pair)+(i==0?"-A":"-B");const auto file=root/(label+".aaproj");REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));requests[i]={project,file,root/"runs"/label,1};requests[i].identity={project.project_id,41,juce::Uuid().toString().toStdString(),19};requests[i].on_log=[&,i](NodeLogEvent event){if(!event.raw_log_file.empty())raw[i].insert(event.raw_log_file);if(!event.report_file.empty())archives[i].insert(event.report_file);if(event.source=="native"&&!observed[i]){observed[i]=true;first[i]=event;workers[i]=std::this_thread::get_id();entered[i].store(true);allowed.wait();}};}
        for(int i=0;i<2;++i)jobs[i]=std::async(std::launch::async,[&,i]{RunOutcome result;try{result=services[i].run(requests[i]);}catch(const std::exception& e){result=Diagnostic{"test_uncaught_exception",e.what()};}const auto* error=std::get_if<Diagnostic>(&result);std::ofstream(root/(requests[i].output_directory.filename().string()+"-outcome.txt"))<<(error?error->code+": "+error->message:std::string("success"));return result;});
        REQUIRE(pump([&]{return(entered[0].load()&&entered[1].load())||jobs[0].wait_for(std::chrono::milliseconds(0))==std::future_status::ready||jobs[1].wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},5000));REQUIRE(entered[0].load());REQUIRE(entered[1].load());CHECK(workers[0]!=workers[1]);juce::DynamicObject::Ptr gate=new juce::DynamicObject;gate->setProperty("pair",pair);juce::Array<juce::var> blocked;
        for(int i=0;i<2;++i){const bool pending=jobs[i].wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;CHECK(pending);CHECK(first[i].identity==requests[i].identity);CHECK(first[i].node_id=="native");CHECK(first[i].source=="native");CHECK(first[i].total_bytes==1024);juce::DynamicObject::Ptr item=new juce::DynamicObject;item->setProperty("request",juce::String(requests[i].identity.request_id));item->setProperty("entered",entered[i].load());item->setProperty("pending_before_release",pending);item->setProperty("raw_file",juce::String(first[i].raw_log_file));item->setProperty("first_bytes",static_cast<juce::int64>(first[i].total_bytes));blocked.add(juce::var(item.get()));}gate->setProperty("blocked",blocked);gates.add(juce::var(gate.get()));drain.proceed();REQUIRE(pump([&]{return jobs[0].wait_for(std::chrono::milliseconds(0))==std::future_status::ready&&jobs[1].wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},30000));
        for(int i=0;i<2;++i){const auto result=jobs[i].get();const bool succeeds=pair==0||i==1;const auto* error=std::get_if<Diagnostic>(&result);CHECK(std::holds_alternative<RunSuccess>(result)==succeeds);REQUIRE(raw[i].size()==1);REQUIRE(archives[i].size()==1);CHECK(std::filesystem::file_size(std::filesystem::u8path(*raw[i].begin()))==static_cast<std::uint64_t>(limit));if(succeeds){REQUIRE(std::holds_alternative<RunSuccess>(result));CHECK(std::get<RunSuccess>(result).identity==requests[i].identity);const auto output=requests[i].output_directory/"output.wav";const auto read=read_wav_file(output);REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& audio=std::get<AudioBuffer>(read);CHECK(audio.frame_count()==513);CHECK(audio.channel_ids()==inputs[i].channel_ids());CHECK(audio.sample_rate_hz()==48000);CHECK(audio.valid_interval()==inputs[i].valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(audio.channel(c)[n]==inputs[i].channel(c)[n]*2.f);previous[output]=juce::SHA256(juce::File(output.generic_string())).toHexString();}else{REQUIRE(error);CHECK(error->code=="log_limit");CHECK(error->node_id=="native");CHECK_FALSE(std::filesystem::exists(requests[i].output_directory));}
            juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("pair",pair);row->setProperty("input",i==0?"A":"B");row->setProperty("request",juce::String(requests[i].identity.request_id));row->setProperty("success",std::holds_alternative<RunSuccess>(result));row->setProperty("code",error?juce::String(error->code):juce::String{});row->setProperty("archive",juce::String(*archives[i].begin()));row->setProperty("run",juce::String(requests[i].output_directory.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));}for(const auto& [file,hash]:previous){CHECK(juce::SHA256(juce::File(file.generic_string())).toHexString()==hash);CHECK(std::filesystem::is_regular_file(file.parent_path()/"complete.json"));}}
    CHECK(previous.size()==3);CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"parallel-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"barriers.json")<<juce::JSON::toString(gates,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\"}";
#endif
}

TEST_CASE("PC-04.b native stream keeps exact bounded UTF8 capture tail and independent persisted counters", "[PC-04.b][PC0610][node_authoring]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0610-native-tail/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root/"runs");std::cout<<"PC0610_TAIL_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0610-native-tail/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));REQUIRE(load_native_plugins(*registry,{package}).diagnostics.empty());RunService service(registry);const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();
    AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    struct Scenario{const char* label;const char* pattern;std::int64_t bytes;};const std::array<Scenario,4> scenarios{{{"ascii-exact","ascii",65536},{"ascii-plus-one","ascii",65537},{"euro-plus-two","euro",65538},{"nul-plus-one","nul",65537}}};juce::Array<juce::var> receipts;std::map<std::filesystem::path,juce::String> old_outputs;
    for(const auto& scenario:scenarios){CAPTURE(scenario.label);ProjectModel project;project.schema_version=2;project.project_id="pc0610-native-tail";project.revision=23;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native","example.sdk_logging",{{"log_bytes",scenario.bytes},{"pattern",std::string(scenario.pattern)}},220,100},{"output","builtin.output",{},420,100}};project.connections={{"file","audio","native","audio_in"},{"native","audio_out","output","audio"}};const auto file=root/(std::string(scenario.label)+".aaproj");REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));RunRequest request{project,file,root/"runs"/scenario.label,1};request.identity={project.project_id,43,juce::Uuid().toString().toStdString(),23};std::set<std::string> raw,archives;request.on_log=[&](NodeLogEvent event){CHECK(event.identity==request.identity);CHECK(event.node_id=="native");CHECK(event.source=="native");CHECK(event.content_sha256==dll_hash.toStdString());if(!event.raw_log_file.empty())raw.insert(event.raw_log_file);if(!event.report_file.empty())archives.insert(event.report_file);};
        auto job=std::async(std::launch::async,[&]{return service.run(request);});const bool done=pump([&]{return job.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},30000);REQUIRE(done);auto result=job.get();INFO((std::holds_alternative<Diagnostic>(result)?std::get<Diagnostic>(result).message:std::string("success")));REQUIRE(std::holds_alternative<RunSuccess>(result));REQUIRE(raw.size()==1);REQUIRE(archives.size()==1);const auto raw_path=std::filesystem::u8path(*raw.begin());std::string expected;if(std::string(scenario.pattern)=="euro")for(std::int64_t i=0;i<scenario.bytes/3;++i)expected+="\xe2\x82\xac";else expected.assign(static_cast<std::size_t>(scenario.bytes),std::string(scenario.pattern)=="nul"?'\0':'F');std::ifstream stream(raw_path,std::ios::binary);const std::string actual((std::istreambuf_iterator<char>(stream)),{});CHECK(actual==expected);
        const auto tail=expected.substr(std::string(scenario.pattern)=="euro"?3:static_cast<std::size_t>(scenario.bytes-65536));auto summary_path=raw_path;summary_path.replace_extension(".summary.json");const auto summary=juce::JSON::parse(juce::File(summary_path.generic_string()));CHECK(summary.isObject());CHECK(static_cast<int>(summary["schema_version"])==1);CHECK(summary["project_id"].toString()=="pc0610-native-tail");CHECK(summary["request_id"].toString()==juce::String(request.identity.request_id));CHECK(static_cast<juce::int64>(summary["revision"])==23);CHECK(static_cast<juce::int64>(summary["session_epoch"])==43);CHECK(summary["node_id"].toString()=="native");CHECK(summary["type_id"].toString()=="example.sdk_logging");CHECK(summary["package_id"].toString()=="example.sdk_logging.tail");CHECK(summary["version"].toString()=="1.0.0");CHECK(summary["content_sha256"].toString()==dll_hash);CHECK(summary["source"].toString()=="native");CHECK(summary["raw_log_file"].toString()==juce::String(*raw.begin()));CHECK(static_cast<juce::int64>(summary["persisted_bytes"])==scenario.bytes);CHECK(static_cast<juce::int64>(summary["accepted_bytes"])==scenario.bytes);CHECK(static_cast<juce::int64>(summary["attempted_valid_bytes"])==scenario.bytes);CHECK(static_cast<juce::int64>(summary["tail_limit_bytes"])==65536);CHECK(static_cast<juce::int64>(summary["tail_bytes"])==static_cast<juce::int64>(tail.size()));CHECK(static_cast<juce::int64>(summary["tail_discarded_bytes"])==scenario.bytes-static_cast<juce::int64>(tail.size()));CHECK(summary["tail_utf8_base64"].toString()==juce::Base64::toBase64(tail.data(),tail.size()));const auto chunk=std::string(scenario.pattern)=="euro"?1023:1024;const auto messages=(scenario.bytes+chunk-1)/chunk;CHECK(static_cast<juce::int64>(summary["persisted_messages"])==messages);CHECK(static_cast<juce::int64>(summary["accepted_messages"])==messages);CHECK(static_cast<juce::int64>(summary["queue_full_messages"])==0);CHECK(static_cast<juce::int64>(summary["queue_full_bytes"])==0);CHECK(static_cast<juce::int64>(summary["rejected_messages"])==0);CHECK(static_cast<juce::int64>(summary["closed_messages"])==0);CHECK(static_cast<bool>(summary["raw_complete"]));CHECK(static_cast<bool>(summary["all_messages_accepted"]));CHECK(summary["failure_code"].toString().isEmpty());
        const auto output=request.output_directory/"output.wav";const auto read=read_wav_file(output);REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& audio=std::get<AudioBuffer>(read);CHECK(audio.sample_rate_hz()==48000);CHECK(audio.channel_ids()==input.channel_ids());CHECK(audio.frame_count()==513);CHECK(audio.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(audio.channel(c)[n]==2.f*input.channel(c)[n]);old_outputs[output]=juce::SHA256(juce::File(output.generic_string())).toHexString();for(const auto&[path,hash]:old_outputs)CHECK(juce::SHA256(juce::File(path.generic_string())).toHexString()==hash);
        juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("name",scenario.label);row->setProperty("pattern",scenario.pattern);row->setProperty("bytes",static_cast<juce::int64>(scenario.bytes));row->setProperty("request",juce::String(request.identity.request_id));row->setProperty("archive",juce::String(*archives.begin()));row->setProperty("summary",juce::String(summary_path.generic_string()));row->setProperty("raw",juce::String(raw_path.generic_string()));row->setProperty("run",juce::String(request.output_directory.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));
    }
    CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"tail-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\"}";
#endif
}

TEST_CASE("PC-06.d accepted synchronous detach logs drain before queue closure for both native instances", "[PC-06.d][PC0611][node_authoring]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0611-detach-drain/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root/"runs");std::cout<<"PC0611_DETACH_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0611-detach-drain/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));REQUIRE(load_native_plugins(*registry,{package}).diagnostics.empty());RunService service(registry);const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc0611-detach-drain";project.revision=29;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native-A","example.sdk_logging",{{"log_bytes",std::int64_t{16}}},220,100},{"native-B","example.sdk_logging",{{"log_bytes",std::int64_t{16}}},420,100},{"output","builtin.output",{},620,100}};project.connections={{"file","audio","native-A","audio_in"},{"native-A","audio_out","native-B","audio_in"},{"native-B","audio_out","output","audio"}};const auto file=root/"detach.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));juce::Array<juce::var> receipts;juce::String previous;
    for(int attempt=0;attempt<2;++attempt){CAPTURE(attempt);const auto label="attempt-"+std::to_string(attempt);RunRequest request{project,file,root/"runs"/label,127};request.identity={project.project_id,47,juce::Uuid().toString().toStdString(),29};std::vector<NodeLogEvent> events;request.on_log=[&](NodeLogEvent event){events.push_back(std::move(event));};auto job=std::async(std::launch::async,[&]{return service.run(request);});REQUIRE(pump([&]{return job.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},30000));auto result=job.get();INFO((std::holds_alternative<Diagnostic>(result)?std::get<Diagnostic>(result).message:std::string("success")));REQUIRE(std::holds_alternative<RunSuccess>(result));CHECK(std::get<RunSuccess>(result).identity==request.identity);std::map<std::string,std::vector<NodeLogEvent>> by_node;for(const auto& event:events){CHECK(event.identity==request.identity);CHECK(event.source=="native");CHECK(event.type_id=="example.sdk_logging");CHECK(event.package_id=="example.sdk_logging.detach");CHECK(event.content_sha256==dll_hash.toStdString());by_node[event.node_id].push_back(event);}CHECK(by_node.size()==2);std::set<std::string> raw_paths,archives;
        for(const auto* id:{"native-A","native-B"}){const auto& node=by_node[id];REQUIRE_FALSE(node.empty());const auto raw=std::filesystem::u8path(node.front().raw_log_file);CHECK(raw_paths.insert(node.front().raw_log_file).second);archives.insert(node.front().report_file);std::ifstream stream(raw,std::ios::binary);const std::string actual((std::istreambuf_iterator<char>(stream)),{});const auto expected=std::string(16,'F')+"sdk-detach accepted\n";CHECK(actual==expected);CHECK(node.size()==2);CHECK(node.back().text=="sdk-detach accepted\n");CHECK(node.back().sequence==2);CHECK(node.back().total_bytes==expected.size());CHECK(node.back().discarded_bytes==0);auto summary_path=raw;summary_path.replace_extension(".summary.json");const auto summary=juce::JSON::parse(juce::File(summary_path.generic_string()));CHECK(summary.isObject());CHECK(summary["request_id"].toString()==juce::String(request.identity.request_id));CHECK(summary["node_id"].toString()==id);CHECK(summary["content_sha256"].toString()==dll_hash);CHECK(static_cast<juce::int64>(summary["accepted_bytes"])==static_cast<juce::int64>(expected.size()));CHECK(static_cast<juce::int64>(summary["persisted_bytes"])==static_cast<juce::int64>(expected.size()));CHECK(static_cast<juce::int64>(summary["accepted_messages"])==2);CHECK(static_cast<juce::int64>(summary["persisted_messages"])==2);CHECK(static_cast<bool>(summary["raw_complete"]));CHECK(static_cast<bool>(summary["all_messages_accepted"]));CHECK(summary["failure_code"].toString().isEmpty());CHECK(summary["tail_utf8_base64"].toString()==juce::Base64::toBase64(expected.data(),expected.size()));}
        REQUIRE(archives.size()==1);const auto read=read_wav_file(request.output_directory/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& audio=std::get<AudioBuffer>(read);CHECK(audio.frame_count()==513);CHECK(audio.sample_rate_hz()==48000);CHECK(audio.channel_ids()==input.channel_ids());CHECK(audio.valid_interval()==input.valid_interval());for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(audio.channel(c)[n]==4.f*input.channel(c)[n]);const auto hash=juce::SHA256(juce::File((request.output_directory/"output.wav").generic_string())).toHexString();if(attempt==0)previous=hash;else CHECK(juce::SHA256(juce::File((root/"runs/attempt-0/output.wav").generic_string())).toHexString()==previous);juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("attempt",attempt);row->setProperty("request",juce::String(request.identity.request_id));row->setProperty("archive",juce::String(*archives.begin()));row->setProperty("run",juce::String(request.output_directory.lexically_relative(root).generic_string()));receipts.add(juce::var(row.get()));
    }
    CHECK(juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString()==dll_hash);std::ofstream(root/"detach-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\"}";
#endif
}

TEST_CASE("PC-03.c active native production queue cancellation drains final logs removes payload and recovers", "[PC-03.c][PC0612][node_authoring][component]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI gui;const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/reports/loops/PC0612-native-cancel/artifacts"/juce::Uuid().toString().toStdString();std::filesystem::create_directories(root/"runs");std::cout<<"PC0612_CANCEL_ROOT="<<root.generic_string()<<'\n';const auto external=juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR)).getChildFile("build/reports/loops/PC0611-detach-drain/external-path.json"));REQUIRE(external.isObject());const auto package=std::filesystem::path(external["external"].toString().toStdString())/"build";auto registry=std::make_shared<NodeRegistry>();REQUIRE(register_builtin_nodes(*registry));REQUIRE(load_native_plugins(*registry,{package}).diagnostics.empty());const auto dll_hash=juce::SHA256(juce::File((package/"native_logging.dll").generic_string())).toHexString();AudioBuffer input(48000,{"ch0","ch1"},513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)input.mutable_channel(c)[n]=static_cast<float>(.125*std::sin(.03*(n+1))+.02*c);REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root/"input.wav",input,WavEncoding::float32)));
    ProjectModel project;project.schema_version=2;project.project_id="pc0612-native-cancel";project.revision=31;project.selected_output_node_id="output";project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}},20,100},{"native-A","example.sdk_logging",{{"log_bytes",std::int64_t{16}}},220,100},{"native-B","example.sdk_logging",{{"log_bytes",std::int64_t{16}}},420,100},{"output","builtin.output",{},620,100}};project.connections={{"file","audio","native-A","audio_in"},{"native-A","audio_out","native-B","audio_in"},{"native-B","audio_out","output","audio"}};const auto file=root/"native-cancel.aaproj";REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));
    juce::Component target;auto dispatch=std::make_shared<editor::ComponentDispatchState<juce::Component>>(target);std::optional<RunOutcome> outcome;RunIdentity delivered;editor::WaveformChannels waveform;std::vector<NodeLogEvent> ui_events;std::thread::id completion_thread;std::promise<void> release;const auto allowed=release.get_future().share();std::atomic<bool> entered{false};NodeLogEvent first;editor::EditorWorkerPool pool(1);editor::EditorRunQueue<juce::Component> queue(pool,dispatch,registry,std::make_shared<ProjectResourceService>(),[&](juce::Component&,RunOutcome result,editor::WaveformChannels audio,RunIdentity identity){outcome=std::move(result);waveform=std::move(audio);delivered=std::move(identity);completion_thread=std::this_thread::get_id();},[&](juce::Component&,NodeLogEvent event){ui_events.push_back(std::move(event));});
    struct DrainQueue{editor::EditorWorkerPool&pool;editor::EditorRunQueue<juce::Component>&queue;std::promise<void>&release;bool sent=false;void proceed(){if(!sent){sent=true;release.set_value();}}~DrainQueue(){proceed();queue.close();bool done=false;if(pool.shutdown_async(5000,[&](bool){done=true;})){while(!done)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);}pool.join_shutdown();}}drain{pool,queue,release};juce::Array<juce::var> receipts;juce::String baseline;
    for(int phase=0;phase<3;++phase){CAPTURE(phase);const auto label=phase==0?"baseline":phase==1?"cancelled":"recovery";outcome.reset();ui_events.clear();waveform.clear();std::vector<NodeLogEvent> worker_events;RunRequest request{project,file,root/"runs"/label,127};request.identity={project.project_id,53,juce::Uuid().toString().toStdString(),31};const auto identity=request.identity;request.on_log=[&](NodeLogEvent event){worker_events.push_back(event);if(phase==1&&event.source=="native"&&!entered.load()){first=event;entered.store(true);allowed.wait();}};REQUIRE(queue.submit(std::move(request),identity));double cancellation_ms=0;
        if(phase==1){REQUIRE(pump([&]{return entered.load()||outcome.has_value();},5000));REQUIRE(entered.load());CHECK_FALSE(outcome.has_value());const auto active=queue.counts();CHECK(active.active==1);CHECK(active.pending==0);CHECK(active.worker_claimed);CHECK(pool.get_num_jobs()==1);CHECK(first.identity==identity);CHECK(first.node_id=="native-A");CHECK(first.text==std::string(16,'F'));CHECK(first.total_bytes==16);CHECK(std::filesystem::file_size(std::filesystem::u8path(first.raw_log_file))==16);const auto started=std::chrono::steady_clock::now();queue.cancel();drain.proceed();REQUIRE(pump([&]{const auto c=queue.counts();return outcome.has_value()&&c.active==0&&c.pending==0&&!c.worker_claimed&&pool.get_num_jobs()==0;},5000));cancellation_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();CHECK(cancellation_ms<=5000);}
        else REQUIRE(pump([&]{const auto c=queue.counts();return outcome.has_value()&&c.active==0&&c.pending==0&&!c.worker_claimed&&pool.get_num_jobs()==0;},30000));
        REQUIRE(outcome.has_value());CHECK(delivered==identity);CHECK(completion_thread==std::this_thread::get_id());const auto* error=std::get_if<Diagnostic>(&*outcome);if(phase==1){REQUIRE(error);CHECK(error->code=="run_cancelled");CHECK(waveform.empty());CHECK_FALSE(std::filesystem::exists(root/"runs/cancelled"));}else{REQUIRE(std::holds_alternative<RunSuccess>(*outcome));CHECK_FALSE(waveform.empty());const auto read=read_wav_file(root/"runs"/label/"output.wav");REQUIRE(std::holds_alternative<AudioBuffer>(read));const auto& audio=std::get<AudioBuffer>(read);CHECK(audio.sample_rate_hz()==48000);CHECK(audio.channel_ids()==input.channel_ids());CHECK(audio.valid_interval()==input.valid_interval());CHECK(audio.frame_count()==513);for(int c=0;c<2;++c)for(int n=0;n<513;++n)CHECK(audio.channel(c)[n]==4.f*input.channel(c)[n]);}if(phase==0)baseline=juce::SHA256(juce::File((root/"runs/baseline/output.wav").generic_string())).toHexString();CHECK(juce::SHA256(juce::File((root/"runs/baseline/output.wav").generic_string())).toHexString()==baseline);CHECK(worker_events.size()==4);CHECK(ui_events.size()==worker_events.size());std::set<std::string> archives;std::map<std::string,std::vector<NodeLogEvent>> by_node;
        for(std::size_t i=0;i<worker_events.size();++i){const auto& event=worker_events[i];REQUIRE(i<ui_events.size());CHECK(ui_events[i].identity==identity);CHECK(ui_events[i].node_id==event.node_id);CHECK(ui_events[i].text==event.text);CHECK(ui_events[i].sequence==event.sequence);CHECK(event.identity==identity);CHECK(event.source=="native");CHECK(event.content_sha256==dll_hash.toStdString());by_node[event.node_id].push_back(event);archives.insert(event.report_file);}CHECK(by_node.size()==2);for(const auto* id:{"native-A","native-B"}){const auto& node=by_node[id];REQUIRE(node.size()==2);CHECK(node[0].text==std::string(16,'F'));CHECK(node[1].text=="sdk-detach accepted\n");CHECK(node[1].sequence==2);CHECK(node[1].total_bytes==36);const auto raw=std::filesystem::u8path(node[0].raw_log_file);std::ifstream stream(raw,std::ios::binary);CHECK(std::string((std::istreambuf_iterator<char>(stream)),{})==std::string(16,'F')+"sdk-detach accepted\n");auto summary_path=raw;summary_path.replace_extension(".summary.json");const auto summary=juce::JSON::parse(juce::File(summary_path.generic_string()));CHECK(static_cast<bool>(summary["raw_complete"]));CHECK(static_cast<juce::int64>(summary["persisted_bytes"])==36);CHECK(static_cast<juce::int64>(summary["accepted_bytes"])==36);}REQUIRE(archives.size()==1);const auto counts=queue.counts();CHECK(counts.active==0);CHECK(counts.pending==0);CHECK_FALSE(counts.worker_claimed);CHECK(pool.get_num_jobs()==0);juce::DynamicObject::Ptr row=new juce::DynamicObject;row->setProperty("name",label);row->setProperty("request",juce::String(identity.request_id));row->setProperty("code",error?juce::String(error->code):juce::String{});row->setProperty("archive",juce::String(*archives.begin()));row->setProperty("run",juce::String((std::filesystem::path("runs")/label).generic_string()));row->setProperty("active",static_cast<int>(counts.active));row->setProperty("pending",static_cast<int>(counts.pending));row->setProperty("worker_claimed",counts.worker_claimed);row->setProperty("jobs",pool.get_num_jobs());row->setProperty("worker_events",static_cast<int>(worker_events.size()));row->setProperty("ui_events",static_cast<int>(ui_events.size()));row->setProperty("cancel_ms",cancellation_ms);row->setProperty("first_log_barrier",phase==1&&entered.load());receipts.add(juce::var(row.get()));
    }
    dispatch->invalidate();std::ofstream(root/"cancel-demo.json")<<juce::JSON::toString(receipts,false).toStdString();std::ofstream(root/"dll-identity.json")<<"{\"dll\":"<<juce::JSON::toString(juce::var(juce::String((package/"native_logging.dll").generic_string())),true)<<",\"sha256\":\""<<dll_hash<<"\"}";
#endif
}

TEST_CASE("PC-03.c active native View and queue close reject queued logs drain owned modules and permit recovery",
          "[PC-03.c][PC-06.d][PC0613][node_authoring][component]") {
#if !defined(_WIN32)
    SKIP("Requires Windows SDK DLL");
#else
    using namespace audioalgo;
    juce::ScopedJuceInitialiser_GUI gui;
    using Queue = editor::EditorRunQueue<juce::Component>;
    using Dispatch = editor::ComponentDispatchState<juce::Component>;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/PC0613-native-close/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root / "runs");
    std::cout << "PC0613_CLOSE_ROOT=" << root.generic_string() << '\n';
    const auto external = juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR))
        .getChildFile("build/reports/loops/PC0611-detach-drain/external-path.json"));
    REQUIRE(external.isObject());
    const auto package = std::filesystem::path(external["external"].toString().toStdString()) / "build";
    const auto dll = package / "native_logging.dll";
    const auto dll_hash = juce::SHA256(juce::File(dll.generic_string())).toHexString();
    auto load_registry = [&] {
        auto nodes = std::make_shared<NodeRegistry>();
        REQUIRE(register_builtin_nodes(*nodes));
        REQUIRE(load_native_plugins(*nodes, {package}).diagnostics.empty());
        return nodes;
    };
    AudioBuffer input(48000, {"ch0", "ch1"}, 513);
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 513; ++n)
            input.mutable_channel(c)[n] = static_cast<float>(.125 * std::sin(.03 * (n + 1)) + .02 * c);
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root / "input.wav", input, WavEncoding::float32)));
    ProjectModel project;
    project.schema_version = 2; project.project_id = "pc0613-native-close"; project.revision = 37;
    project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 20, 100},
        {"native-A", "example.sdk_logging", {{"log_bytes", std::int64_t{16}}}, 220, 100},
        {"native-B", "example.sdk_logging", {{"log_bytes", std::int64_t{16}}}, 420, 100},
        {"output", "builtin.output", {}, 620, 100}};
    project.connections = {{"file", "audio", "native-A", "audio_in"},
        {"native-A", "audio_out", "native-B", "audio_in"}, {"native-B", "audio_out", "output", "audio"}};
    const auto file = root / "native-close.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file, project)));
    juce::Array<juce::var> receipts;
    for (const bool application_shutdown : {false, true}) {
        CAPTURE(application_shutdown);
        const std::string mode = application_shutdown ? "application" : "view";
        auto nodes = load_registry();
        std::weak_ptr<const NodeRegistry> weak_nodes = nodes;
        auto target = std::make_unique<juce::Component>();
        auto dispatch = std::make_shared<Dispatch>(*target);
        std::weak_ptr<Dispatch> weak_dispatch = dispatch;
        auto callback_lease = std::make_shared<int>(0);
        std::weak_ptr<int> weak_lease = callback_lease;
        int old_logs = 0, old_completions = 0, replacement_logs = 0, replacement_completions = 0;
        std::vector<NodeLogEvent> events;
        auto replacement = std::unique_ptr<juce::Component>{};
        std::shared_ptr<Dispatch> replacement_dispatch;
        int shutdown_callbacks = 0;
        bool shutdown_timed_out = false;
        std::thread::id shutdown_thread;
        std::optional<RunOutcome> recovered;
        editor::WaveformChannels waveform;
        std::vector<NodeLogEvent> recovery_events;
        std::atomic<int> barrier{0};
        std::promise<void> first_release, second_release;
        const auto first_allowed = first_release.get_future().share();
        const auto second_allowed = second_release.get_future().share();
        auto pool = std::make_unique<editor::EditorWorkerPool>(1);
        auto queue = std::make_unique<Queue>(*pool, dispatch, nodes, std::make_shared<ProjectResourceService>(),
            [&, lease = callback_lease](juce::Component&, RunOutcome, editor::WaveformChannels, RunIdentity) {
                ++old_completions;
            }, Queue::LogCompletion{}, [&, lease = callback_lease](juce::Component&, std::deque<NodeLogEvent> batch) {
                old_logs += static_cast<int>(batch.size());
            });
        // Release both observer gates and drain even if a REQUIRE aborts the scenario.
        struct Cleanup {
            std::unique_ptr<editor::EditorWorkerPool>& pool;
            std::unique_ptr<Queue>& queue;
            std::promise<void>& first;
            std::promise<void>& second;
            std::shared_ptr<Dispatch>& dispatch;
            std::shared_ptr<Dispatch>& replacement_dispatch;
            bool first_sent = false, second_sent = false;
            void release_first() { if (!first_sent) { first_sent = true; first.set_value(); } }
            void release_second() { if (!second_sent) { second_sent = true; second.set_value(); } }
            ~Cleanup() {
                if (dispatch) dispatch->invalidate();
                if (replacement_dispatch) replacement_dispatch->invalidate();
                if (queue) queue->close();
                release_first(); release_second();
                bool done = false;
                const bool started = pool && pool->shutdown_async(5000, [&](bool) { done = true; });
                while (pool && (pool->get_num_jobs() != 0 || (started && !done)))
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                if (pool) pool->join_shutdown();
                queue.reset();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            }
        } cleanup{pool, queue, first_release, second_release, dispatch, replacement_dispatch};
        RunRequest request{project, file, root / "runs" / (mode + "-closed"), 127};
        request.identity = {project.project_id, 59, juce::Uuid().toString().toStdString(), project.revision};
        const auto identity = request.identity;
        request.on_log = [&](NodeLogEvent event) {
            events.push_back(std::move(event));
            if (events.size() == 1) { barrier.store(1); first_allowed.wait(); }
            else if (events.size() == 2) { barrier.store(2); second_allowed.wait(); }
        };
        REQUIRE(queue->submit(std::move(request), identity));
        REQUIRE(pump([&] { return barrier.load() == 1; }, 5000));
        CHECK(old_logs == 0); CHECK(old_completions == 0);
        cleanup.release_first();
        // No message dispatch here: the first real native event is already in
        // the production mailbox when the observer reaches its second gate.
        REQUIRE(without_ui([&] { return barrier.load() == 2; }));
        REQUIRE(events.size() == 2);
        CHECK(events[0].node_id == "native-A"); CHECK(events[1].node_id == "native-B");
        for (const auto& event : events) {
            CHECK(event.identity == identity); CHECK(event.text == std::string(16, 'F'));
            CHECK(std::filesystem::file_size(std::filesystem::u8path(event.raw_log_file)) == 16);
        }
        CHECK(old_logs == 0); CHECK(old_completions == 0);
        const auto active = queue->counts();
        CHECK(active.active == 1); CHECK(active.pending == 0); CHECK(active.worker_claimed);
        CHECK(pool->get_num_jobs() == 1);
        CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
        const auto closed_at = std::chrono::steady_clock::now();
        queue->close();
        dispatch->invalidate(); target.reset(); dispatch.reset(); callback_lease.reset(); nodes.reset();
        // The worker retains its catalog, and the already queued callback owns
        // its dispatch/lease until the message thread discards that callback.
        CHECK_FALSE(weak_nodes.expired()); CHECK_FALSE(weak_dispatch.expired()); CHECK_FALSE(weak_lease.expired());
        CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
        CHECK_FALSE(queue->submit(RunRequest{project, file, root / "runs" / (mode + "-rejected"), 127}, identity));
        if (application_shutdown) {
            REQUIRE(pool->shutdown_async(5000, [&](bool timed_out) {
                ++shutdown_callbacks; shutdown_timed_out = timed_out; shutdown_thread = std::this_thread::get_id();
            }));
            CHECK(shutdown_callbacks == 0);
        } else {
            queue.reset();
        }
        replacement = std::make_unique<juce::Component>();
        replacement_dispatch = std::make_shared<Dispatch>(*replacement);
        cleanup.release_second();
        REQUIRE(pump([&] { return pool->get_num_jobs() == 0 && (!application_shutdown || shutdown_callbacks == 1); }, 5000));
        if (application_shutdown) {
            CHECK_FALSE(shutdown_timed_out);
            CHECK(shutdown_thread == std::this_thread::get_id());
            const auto counts = queue->counts();
            CHECK(counts.active == 0); CHECK(counts.pending == 0); CHECK_FALSE(counts.worker_claimed);
            pool->join_shutdown(); queue.reset();
        }
        REQUIRE(pump([&] { return weak_dispatch.expired() && weak_lease.expired() && weak_nodes.expired(); }, 5000));
        const auto close_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - closed_at).count();
        CHECK(close_ms <= 5000);
        CHECK(old_logs == 0); CHECK(old_completions == 0);
        CHECK(replacement_logs == 0); CHECK(replacement_completions == 0);
        CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
        CHECK_FALSE(std::filesystem::exists(root / "runs" / (mode + "-closed")));
        CHECK_FALSE(std::filesystem::exists(root / "runs" / (mode + "-rejected")));
        REQUIRE(events.size() == 4);
        std::set<std::string> archives;
        for (const auto& event : events) {
            CHECK(event.identity == identity); CHECK(event.content_sha256 == dll_hash.toStdString());
            archives.insert(event.report_file);
        }
        REQUIRE(archives.size() == 1);
        juce::DynamicObject::Ptr row = new juce::DynamicObject;
        row->setProperty("mode", juce::String(mode)); row->setProperty("request", juce::String(identity.request_id));
        row->setProperty("archive", juce::String(*archives.begin())); row->setProperty("close_ms", close_ms);
        row->setProperty("old_logs", old_logs); row->setProperty("old_completions", old_completions);
        row->setProperty("expired_dispatch", weak_dispatch.expired()); row->setProperty("expired_callbacks", weak_lease.expired());
        row->setProperty("expired_catalog", weak_nodes.expired()); row->setProperty("module_unloaded", GetModuleHandleW(dll.c_str()) == nullptr);
        row->setProperty("shutdown_callbacks", shutdown_callbacks); row->setProperty("shutdown_timed_out", shutdown_timed_out);
        if (application_shutdown) { pool.reset(); pool = std::make_unique<editor::EditorWorkerPool>(1); }
        auto recovered_nodes = load_registry();
        queue = std::make_unique<Queue>(*pool, replacement_dispatch, recovered_nodes, std::make_shared<ProjectResourceService>(),
            [&](juce::Component&, RunOutcome outcome, editor::WaveformChannels audio, RunIdentity completed) {
                ++replacement_completions; recovered = std::move(outcome); waveform = std::move(audio);
                CHECK(completed.project_id == project.project_id); CHECK(completed.revision == project.revision);
            }, Queue::LogCompletion{}, [&](juce::Component&, std::deque<NodeLogEvent> batch) {
                for (auto& event : batch) { ++replacement_logs; recovery_events.push_back(std::move(event)); }
            });
        RunRequest again{project, file, root / "runs" / (mode + "-recovery"), 127};
        again.identity = {project.project_id, 59, juce::Uuid().toString().toStdString(), project.revision};
        const auto recovery_identity = again.identity;
        REQUIRE(queue->submit(std::move(again), recovery_identity));
        REQUIRE(pump([&] { return recovered.has_value() && pool->get_num_jobs() == 0; }, 30000));
        REQUIRE(std::holds_alternative<RunSuccess>(*recovered));
        CHECK(std::get<RunSuccess>(*recovered).identity == recovery_identity); CHECK_FALSE(waveform.empty());
        CHECK(replacement_completions == 1); CHECK(replacement_logs == 4);
        CHECK(old_logs == 0); CHECK(old_completions == 0);
        const auto counts = queue->counts();
        CHECK(counts.active == 0); CHECK(counts.pending == 0); CHECK_FALSE(counts.worker_claimed);
        const auto audio = read_wav_file(root / "runs" / (mode + "-recovery") / "output.wav");
        REQUIRE(std::holds_alternative<AudioBuffer>(audio));
        const auto& output = std::get<AudioBuffer>(audio);
        CHECK(output.frame_count() == 513); CHECK(output.sample_rate_hz() == 48000);
        CHECK(output.channel_ids() == input.channel_ids()); CHECK(output.valid_interval() == input.valid_interval());
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 513; ++n) CHECK(output.channel(c)[n] == 4.f * input.channel(c)[n]);
        REQUIRE(recovery_events.size() == 4);
        for (const auto& event : recovery_events) CHECK(event.identity == recovery_identity);
        row->setProperty("recovery_request", juce::String(recovery_identity.request_id));
        row->setProperty("recovery_archive", juce::String(recovery_events.front().report_file));
        row->setProperty("recovery_logs", replacement_logs); row->setProperty("recovery_completions", replacement_completions);
        row->setProperty("active", static_cast<int>(counts.active)); row->setProperty("pending", static_cast<int>(counts.pending));
        row->setProperty("worker_claimed", counts.worker_claimed); row->setProperty("jobs", pool->get_num_jobs());
        receipts.add(juce::var(row.get()));
        replacement_dispatch->invalidate(); queue.reset(); recovered_nodes.reset();
        CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    }
    CHECK(juce::SHA256(juce::File(dll.generic_string())).toHexString() == dll_hash);
    std::ofstream(root / "close-demo.json") << juce::JSON::toString(receipts, false).toStdString();
    juce::DynamicObject::Ptr dll_identity = new juce::DynamicObject;
    dll_identity->setProperty("dll", juce::String(dll.generic_string())); dll_identity->setProperty("sha256", dll_hash);
    std::ofstream(root / "dll-identity.json") << juce::JSON::toString(juce::var(dll_identity.get()), false).toStdString();
#endif
}

TEST_CASE("PC-04.d production native log flood obeys UI budgets during a ten second mixed Run and recovers",
          "[PC-04.b][PC-04.d][PC0614][node_authoring][component]") {
#if !defined(_WIN32) || !AUDIOALGO_AUTHORING_PYTHON_ENABLED
    SKIP("Requires Windows Python-enabled production mixed Run");
#else
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    constexpr std::int64_t limit = 16 * 1024 * 1024;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/PC0614-native-ui-flood/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout << "PC0614_FLOOD_ROOT=" << root.generic_string() << '\n';
    const auto external = juce::JSON::parse(juce::File(juce::String(AUDIOALGO_SOURCE_DIR))
        .getChildFile("build/reports/loops/PC0606-native-budget/external-path.json"));
    REQUIRE(external.isObject());
    const auto package = std::filesystem::path(external["external"].toString().toStdString()) / "build";
    const auto dll_hash = juce::SHA256(juce::File((package / "native_logging.dll").generic_string())).toHexString();
    auto registry = std::make_shared<NodeRegistry>();
    REQUIRE(register_builtin_nodes(*registry));
    REQUIRE(load_native_plugins(*registry, {package}).diagnostics.empty());
    auto checks = std::make_shared<PythonAuthoring>(root / "library");
    auto draft = python_draft("pc0614-identity-gate");
    const auto literal = juce::JSON::toString(juce::var(juce::String(root.generic_string())), true).toStdString();
    draft.source = "import pathlib\nimport time\ndef process(inputs, params, state):\n"
        "    if params['gain']==2 and not state.get('waited'):\n"
        "        state['waited']=True\n        root=pathlib.Path(" + literal + ")\n"
        "        print('pc0614-native-burst-finished', flush=True)\n"
        "        (root/'entered.txt').write_text('python gate after native region',encoding='utf-8')\n"
        "        while not (root/'finish.txt').exists(): time.sleep(.005)\n"
        "    return {'audio_out':inputs['audio_in']}\n";
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto checked = checks->check(draft, cancel);
    REQUIRE(std::holds_alternative<ReadyPackage>(checked));
    const auto applied = checks->apply(std::get<ReadyPackage>(checked), draft, registry, cancel);
    REQUIRE(std::holds_alternative<AppliedPackage>(applied));
    *registry = *std::get<AppliedPackage>(applied).catalog;
    AudioBuffer input(48000, {"ch0", "ch1"}, 513);
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 513; ++n)
            input.mutable_channel(c)[n] = static_cast<float>(.125 * std::sin(.03 * (n + 1)) + .02 * c);
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root / "input.wav", input, WavEncoding::float32)));
    auto graph = [&](const std::string& name, std::int64_t bytes, double gate) {
        auto python = editor::make_node_instance(*registry, {}, "user.custom_node", "python", 420, 100);
        REQUIRE(python); python->parameters["gain"] = gate;
        ProjectModel project; project.schema_version = 2; project.project_id = "pc0614-" + name;
        project.revision = 41; project.selected_output_node_id = "output";
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 20, 100},
            {"native", "example.sdk_logging", {{"log_bytes", bytes}}, 220, 100}, *python,
            {"output", "builtin.output", {}, 620, 100}};
        project.connections = {{"file", "audio", "native", "audio_in"},
            {"native", "audio_out", "python", "audio_in"}, {"python", "audio_out", "output", "audio"}};
        return project;
    };
    juce::Array<juce::var> receipts;
    std::set<std::filesystem::path> successes;
    std::filesystem::path first_output;
    juce::String first_hash;
    for (int mode = 0; mode < 3; ++mode) {
        CAPTURE(mode);
        const std::string name = mode == 0 ? "exact" : mode == 1 ? "plus-one" : "flood";
        for (const auto* marker : {"entered.txt", "finish.txt"}) std::filesystem::remove(root / marker);
        const auto project = graph(name, mode == 0 ? limit : mode == 1 ? limit + 1 : 4 * limit, 2);
        const auto project_file = root / (name + ".aaproj");
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_file, project)));
        auto preview = std::make_shared<Preview>();
        editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(),
            std::make_shared<ProjectResourceService>(), preview, std::make_shared<FileDraftStorage>(root / "drafts"),
            false, checks, {}, false, {}, {}, {}, editor::RunConfiguration{1});
        workspace.setSize(1440, 900);
        workspace.open_project_file(project_file);
        REQUIRE(pump([&] { return workspace.controller().project().project_id == project.project_id; }));
        auto* logs = control<editor::RunLogTextView>(workspace, "project.run_logs"); REQUIRE(logs);
        auto* panel = dynamic_cast<editor::RunLogPanel*>(logs->getParentComponent()); REQUIRE(panel);
        auto* status = control<editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        click(workspace, "project.logs");
        REQUIRE(logs->isVisible()); REQUIRE(panel->isVisible());
        struct Shutdown {
            editor::EditorComponent& workspace;
            std::filesystem::path release_file;
            ~Shutdown() {
                std::ofstream(release_file) << "release";
                bool done = false; workspace.begin_shutdown([&] { done = true; });
                while (!done) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            }
        } shutdown{workspace, root / "finish.txt"};
        struct Heartbeat final : juce::Timer {
            editor::RunLogTextView* logs = nullptr;
            std::chrono::steady_clock::time_point previous;
            std::vector<double> gaps;
            std::size_t maximum_bytes = 0;
            void timerCallback() override {
                const auto now = std::chrono::steady_clock::now();
                gaps.push_back(std::chrono::duration<double, std::milli>(now - previous).count()); previous = now;
                maximum_bytes = std::max(maximum_bytes, logs->getText().getNumBytesAsUTF8());
            }
        } heartbeat;
        const auto started = std::chrono::steady_clock::now();
        heartbeat.logs = logs; heartbeat.previous = started; heartbeat.startTimer(20);
        click(workspace, "project.run");
        if (mode == 0) {
            REQUIRE(pump([&] { return std::chrono::steady_clock::now() - started >= std::chrono::seconds(10); }, 11000));
            CHECK(std::filesystem::is_regular_file(root / "entered.txt"));
            CHECK_FALSE(status->status_text().contains("Completed run"));
            CHECK(workspace.background_run_counts().active == 1);
        } else {
            REQUIRE(pump([&] { return status->status_text().contains("log_limit"); }, 15000));
            CHECK_FALSE(std::filesystem::exists(root / "entered.txt"));
        }
        heartbeat.stopTimer();
        const auto finished = std::chrono::steady_clock::now();
        heartbeat.gaps.push_back(std::chrono::duration<double, std::milli>(finished - heartbeat.previous).count());
        std::sort(heartbeat.gaps.begin(), heartbeat.gaps.end()); REQUIRE_FALSE(heartbeat.gaps.empty());
        const auto p95 = heartbeat.gaps[static_cast<std::size_t>(.95 * (heartbeat.gaps.size() - 1))];
        const auto maximum = heartbeat.gaps.back();
        const auto duration = std::chrono::duration<double, std::milli>(finished - started).count();
        CHECK(heartbeat.maximum_bytes <= 256 * 1024);
        CHECK(logs->getText().getNumBytesAsUTF8() <= 256 * 1024);
        CHECK(panel->text_for_copy().getNumBytesAsUTF8() <= 256 * 1024);
        CHECK(logs->getText().contains("native"));
        std::ofstream(root / (name + "-ui.log"), std::ios::binary) << logs->getText().toStdString();
        std::ofstream(root / (name + "-copy.log"), std::ios::binary) << panel->text_for_copy().toStdString();
        const auto terminal = status->status_text();
        if (mode == 0) {
            CHECK(duration >= 10000); CHECK(p95 <= 100); CHECK(maximum <= 500);
            std::ofstream(root / "finish.txt") << "release";
            REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }, 15000));
        } else {
            CHECK_FALSE(status->status_text().contains("Completed run"));
            click(workspace, "project.play"); CHECK(preview->played.empty());
            const auto recovery = graph(name + "-recovery", 0, 1);
            const auto path = root / (name + "-recovery.aaproj");
            REQUIRE(std::holds_alternative<std::monostate>(save_project_file(path, recovery)));
            workspace.open_project_file(path);
            REQUIRE(pump([&] { return workspace.controller().project().project_id == recovery.project_id; }));
            click(workspace, "project.run");
            REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }, 15000));
        }
        REQUIRE(pump([&] { const auto c = workspace.background_run_counts(); return c.active == 0 && c.pending == 0 && !c.worker_claimed && workspace.background_job_count() == 0; }));
        for (const auto& run : std::filesystem::directory_iterator(root / "runs"))
            if (std::filesystem::is_regular_file(run.path() / "run.json")) successes.insert(run.path());
        CHECK(successes.size() == static_cast<std::size_t>(mode + 1));
        if (mode == 0) { first_output = *successes.begin() / "output.wav"; first_hash = juce::SHA256(juce::File(first_output.generic_string())).toHexString(); }
        CHECK(juce::SHA256(juce::File(first_output.generic_string())).toHexString() == first_hash);
        juce::DynamicObject::Ptr row = new juce::DynamicObject;
        row->setProperty("mode", juce::String(name)); row->setProperty("window_ms", duration); row->setProperty("interval_ms", 20);
        row->setProperty("samples", static_cast<int>(heartbeat.gaps.size())); row->setProperty("p95_ms", p95); row->setProperty("max_ms", maximum);
        row->setProperty("maximum_ui_bytes", static_cast<juce::int64>(heartbeat.maximum_bytes)); row->setProperty("terminal", terminal);
        receipts.add(juce::var(row.get()));
    }
    for (const auto& run : successes) {
        const auto audio = read_wav_file(run / "output.wav"); REQUIRE(std::holds_alternative<AudioBuffer>(audio));
        const auto& output = std::get<AudioBuffer>(audio);
        CHECK(output.frame_count() == 513); CHECK(output.sample_rate_hz() == 48000);
        CHECK(output.channel_ids() == input.channel_ids()); CHECK(output.valid_interval() == input.valid_interval());
        for (int c = 0; c < 2; ++c) for (int n = 0; n < 513; ++n) CHECK(output.channel(c)[n] == 2.f * input.channel(c)[n]);
    }
    std::ofstream(root / "flood-demo.json") << juce::JSON::toString(receipts, false).toStdString();
    juce::DynamicObject::Ptr identity = new juce::DynamicObject;
    identity->setProperty("dll", juce::String((package / "native_logging.dll").generic_string())); identity->setProperty("sha256", dll_hash);
    std::ofstream(root / "dll-identity.json") << juce::JSON::toString(juce::var(identity.get()), false).toStdString();
#endif
}

namespace {
void drag_splitter(juce::Component& splitter, int dx, int dy) {
    const auto start = splitter.getLocalBounds().getCentre().toFloat();
    const auto screen = splitter.localPointToGlobal(start);
    auto event = [&](juce::Point<float> local, bool dragged) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), local,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 0, 0, 0, 0, 0,
            &splitter, &splitter, juce::Time(100), start, juce::Time(0), 1, dragged);
    };
    splitter.mouseDown(event(start, false));
    splitter.mouseDrag(event(splitter.getLocalPoint(nullptr, screen + juce::Point<float>(dx, dy)), true));
    splitter.mouseUp(event(splitter.getLocalBounds().getCentre().toFloat(), true));
}
void save_component_image(juce::Component& component, const std::filesystem::path& path) {
    struct PreviewWindow final : juce::DocumentWindow {
        audioalgo::editor::EditorLookAndFeel theme;
        PreviewWindow(juce::Component& content)
            : juce::DocumentWindow("AudioAlgo Editor", audioalgo::editor::background, juce::DocumentWindow::allButtons, false) {
            setLookAndFeel(&theme); audioalgo::editor::style_editor_window(*this);
            setContentNonOwned(&content, true);
        }
        ~PreviewWindow() override { clearContentComponent(); setLookAndFeel(nullptr); }
        void closeButtonPressed() override {}
    } frame(component);
    juce::FileOutputStream stream(juce::File(path.generic_string()));
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat format;
    CHECK(format.writeImageToStream(frame.createComponentSnapshot(frame.getLocalBounds()), stream));
}
void reset_splitter(juce::Component& splitter) {
    const auto point = splitter.getLocalBounds().getCentre().toFloat();
    splitter.mouseDoubleClick(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), point,
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 0, 0, 0, 0, 0,
        &splitter, &splitter, juce::Time(100), point, juce::Time(0), 2, false));
}
}

TEST_CASE("NED-01 NI0015 dock splitters resize real panes without editing the project", "[NI0015][component]") {
    using namespace audioalgo;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NI0015-resizable-workspace/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root); std::cout << "NI0015_WORKSPACE_ROOT=" << root.generic_string() << '\n';
    auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
    editor::EditorComponent workspace(registry, std::make_shared<Documents>(), std::make_shared<ProjectResourceService>(),
        std::make_shared<Preview>(), std::make_shared<authoring::FileDraftStorage>(root / "drafts"), false);
    workspace.setSize(1440, 900);
    click(workspace, "library.builtin.gain");
    const auto project = workspace.controller().project();
    const auto dirty = workspace.controller().dirty();
    auto* canvas = control<editor::GraphCanvas>(workspace, "project.canvas"); REQUIRE(canvas);
    auto* left = control<juce::Component>(workspace, "workspace.splitter.library"); REQUIRE(left);
    auto* right = control<juce::Component>(workspace, "workspace.splitter.inspector"); REQUIRE(right);
    auto* bottom = control<juce::Component>(workspace, "workspace.splitter.results"); REQUIRE(bottom);
    auto before = canvas->getBounds();
    drag_splitter(*left, 100, 0); CHECK(canvas->getX() == before.getX() + 100); CHECK(canvas->getWidth() == before.getWidth() - 100);
    before = canvas->getBounds(); drag_splitter(*right, -80, 0); CHECK(canvas->getWidth() == before.getWidth() - 80);
    before = canvas->getBounds(); drag_splitter(*bottom, 0, -100); CHECK(canvas->getHeight() == before.getHeight() - 100);
    save_component_image(workspace, root / "workspace-1440.png");
    for (const auto [dx, dy] : {std::pair{-10000, 0}, std::pair{10000, 0}}) {
        drag_splitter(*left, dx, dy); drag_splitter(*right, dx, dy);
        CHECK(canvas->getWidth() >= 240); CHECK(left->getX() >= 148); CHECK(right->getRight() <= workspace.getWidth() - 220);
    }
    for (int dy : {-10000, 10000}) { drag_splitter(*bottom, 0, dy); CHECK(canvas->getHeight() >= 160); CHECK(bottom->getBottom() < workspace.getHeight() - 100); }
    workspace.setSize(1100, 700);
    CHECK(canvas->getRight() <= right->getX()); CHECK(canvas->getBottom() <= bottom->getY());
    CHECK(canvas->getWidth() >= 240); CHECK(canvas->getHeight() >= 160);
    CHECK(workspace.controller().project().project_id == project.project_id);
    CHECK(workspace.controller().project().revision == project.revision);
    CHECK(workspace.controller().project().nodes.size() == project.nodes.size());
    CHECK(workspace.controller().dirty() == dirty);
    reset_splitter(*left); reset_splitter(*right); reset_splitter(*bottom);
    CHECK(canvas->getX() == 172); CHECK(right->getRight() == 830);
    save_component_image(workspace, root / "workspace-1100.png");
    int drained = 0; workspace.begin_shutdown([&] { ++drained; }); REQUIRE(pump([&] { return drained == 1; }));
}

TEST_CASE("NA-14 NI0015 authoring splitters preserve source and expose all real probe prints", "[NI0015][component][node_authoring]") {
    using namespace audioalgo;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NI0015-resizable-workspace/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root); std::cout << "NI0015_AUTHORING_ROOT=" << root.generic_string() << '\n';
    auto draft = authoring::python_draft("ni0015-print");
    draft.source = "def process(inputs, params, state):\n    print('ni0015-block', flush=True)\n    return {'audio_out': inputs['audio_in']}\n";
    auto checks = std::make_shared<authoring::PythonAuthoring>(root / "library");
    editor::NodeAuthoringPanel panel(draft, false, std::make_shared<authoring::FileDraftStorage>(root / "drafts"), [] {}, checks);
    panel.setSize(1404, 864);
    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    auto* definition = control<juce::CodeEditorComponent>(panel, "authoring.definition"); REQUIRE(definition);
    auto* logs = control<editor::RunLogTextView>(panel, "authoring.check_logs"); REQUIRE(logs);
    auto* between = control<juce::Component>(panel, "authoring.splitter.definition"); REQUIRE(between);
    auto* log_bar = control<juce::Component>(panel, "authoring.splitter.logs"); REQUIRE(log_bar);
    const auto generation = panel.session().document().generation;
    const auto previous = definition->getHeight(); drag_splitter(*between, 0, 65);
    CHECK(definition->getHeight() == previous + 65);
    const auto source_height = source->getHeight(); REQUIRE(source_height >= 120);
    drag_splitter(*log_bar, 0, -20); CHECK(source->getHeight() == source_height - 20);
    CHECK(panel.session().document().generation == generation); CHECK(panel.session().document().source == draft.source);
    CHECK(panel.session().document().declaration == draft.declaration);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    click(panel, "authoring.check"); REQUIRE(pump([&] { return panel.ready().has_value(); }, 15000));
    std::string stdout_text;
    for (const auto& row : panel.ready()->logs) if (row.phase == "contract" && row.event.source == "contract.stdout") stdout_text += row.event.text;
    const auto normalized_stdout = juce::String(stdout_text).replace("\r\n", "\n");
    CHECK(normalized_stdout == "ni0015-block\nni0015-block\nni0015-block\nni0015-block\nni0015-block\n");
    CHECK(logs->getText().replace("\r\n", "\n").contains(normalized_stdout));
    std::ofstream(root / "probe-stdout.log", std::ios::binary) << stdout_text;
    const auto ready_hash = panel.ready()->source_sha256;
    drag_splitter(*between, 0, -35); drag_splitter(*log_bar, 0, 30);
    REQUIRE(panel.ready()); CHECK(panel.ready()->source_sha256 == ready_hash);
#endif
    save_component_image(panel, root / "authoring-1404.png");
    for (int height : {664, 864}) {
        panel.setSize(1064, height);
        drag_splitter(*between, 0, 10000); drag_splitter(*log_bar, 0, -10000);
        CHECK(source->getHeight() >= 100); CHECK(definition->getHeight() >= 72); CHECK(logs->getHeight() >= 48);
        CHECK(panel.session().document().generation == generation);
    }
    reset_splitter(*between); reset_splitter(*log_bar);
    save_component_image(panel, root / "authoring-1064.png");
}

TEST_CASE("NA-08.d reopen and fresh CLI resolve the saved v1 after v2 becomes default",
          "[NA-08.d][NA-09.a][NA-11.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0018-exact-reopen" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring");
    auto draft = audioalgo::authoring::python_draft("reopen-old"); auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied)); const auto old = std::get<audioalgo::authoring::AppliedPackage>(applied);
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "reopen-v1"; project.selected_output_node_id = "output";
    const auto node = audioalgo::editor::make_node_instance(*old.catalog, {}, "user.custom_node", "custom", 340, 100); REQUIRE(node);
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, *node, {"output", "builtin.output", {}, 640, 100}};
    project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    draft.source = "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n"; ++draft.generation;
    checked = service->check(draft, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, old.catalog, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied)); const auto latest = std::get<audioalgo::authoring::AppliedPackage>(applied);
    REQUIRE(latest.content_sha256 != old.content_sha256);
    SECTION("production open resolves before adoption and actual Run") {
        auto nodes = std::make_shared<audioalgo::NodeRegistry>(*latest.catalog);
        audioalgo::editor::EditorComponent workspace(nodes, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
            std::make_shared<Preview>(), std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts"), false, service);
        workspace.open_project_file(root / "project.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "reopen-v1"; }));
        REQUIRE(nodes->find("user.custom_node")); REQUIRE(nodes->find("user.custom_node")->plugin_origin->managed_content_sha256 == old.content_sha256);
        click(workspace, "project.run"); auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status");
        const auto finished = pump([&] { return status->status_text().contains("Completed run"); }); INFO(status->status_text().toStdString()); REQUIRE(finished);
    }
    SECTION("new CLI uses old hash and refuses missing or corrupt old bytes then recovers") {
        const auto previous = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {}).toStdString();
        struct Restore { std::string value; ~Restore() { _putenv_s("AUDIOALGO_AUTHORING_ROOT", value.c_str()); } } restore{previous};
        REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT", (root / "authoring").string().c_str()) == 0);
        const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
        auto run = [&](const char* name, bool success) {
            juce::ChildProcess child; REQUIRE(child.start(juce::StringArray{exe.getFullPathName(), "run", juce::String((root / "project.aaproj").generic_string()), juce::String((root / name).generic_string()), "127"}));
            auto reader = std::async(std::launch::async, [&] { return child.readAllProcessOutput(); });
            const auto done = child.waitForProcessToFinish(10000); if (!done) child.kill(); const auto output = reader.get(); INFO(output.toStdString()); REQUIRE(done);
            std::ofstream(root / (std::string(name) + ".log"), std::ios::binary) << output.toStdString();
            REQUIRE((child.getExitCode() == 0) == success);
            if (!success) { CHECK(output.contains("managed_version_unavailable")); CHECK_FALSE(std::filesystem::exists(root / name / "complete.json")); return; }
            auto read = audioalgo::read_wav_file(root / name / "output.wav"); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
            const auto& audio = std::get<audioalgo::AudioBuffer>(read); REQUIRE(audio.frame_count() == 513);
            for (std::size_t n = 0; n < 513; ++n) { const auto expected = .1 * std::sin(.03 * (n + 1)); CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected)); }
        };
        run("v1", true);
        const auto source = old.package_root / "user_source.py"; std::ifstream file(source, std::ios::binary); const std::string bytes(std::istreambuf_iterator<char>(file), {}); file.close();
        std::filesystem::remove(source); run("missing-v1", false);
        std::ofstream(source, std::ios::binary) << "raise RuntimeError('corrupted old package must never execute')\n"; run("corrupt-v1", false);
        std::ofstream(source, std::ios::binary) << bytes; run("recovered-v1", true);
    }
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-08.b late committed Apply cannot install an old graph catalog into a new project",
          "[NA-08.b][NA-09.a][NA-14.b][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0019-apply-project-switch" / juce::Uuid().toString().toStdString();
    struct Barrier { std::atomic<bool> enabled{false}, entered{false}, release{false}; };
    const auto barrier = std::make_shared<Barrier>();
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root, std::filesystem::path{}, std::filesystem::path{},
        [barrier](audioalgo::authoring::ApplyPhase phase) {
            if (phase == audioalgo::authoring::ApplyPhase::index_replace && barrier->enabled) {
                barrier->entered = true; while (!barrier->release) std::this_thread::yield();
            }
            return true;
        });
    auto nodes = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*nodes));
    auto draft = audioalgo::authoring::python_draft("switch-during-commit"); const auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    auto applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, nodes, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied)); const auto old = std::get<audioalgo::authoring::AppliedPackage>(applied);
    *nodes = *old.catalog;
    audioalgo::ProjectModel old_project; old_project.schema_version = 2; old_project.project_id = "before-switch";
    const auto instance = audioalgo::editor::make_node_instance(*nodes, {}, "user.custom_node", "old-custom", 300, 100); REQUIRE(instance);
    old_project.nodes = {*instance};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "old.aaproj", old_project)));
    draft.source = "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n"; ++draft.generation;
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts"); REQUIRE_FALSE(storage->save(draft));
    audioalgo::editor::EditorComponent workspace(nodes, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, service);
    struct Release { std::shared_ptr<Barrier> barrier; ~Release() { barrier->release = true; } } release{barrier};
    workspace.open_project_file(root / "old.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "before-switch"; }));
    click(workspace, "authoring.open"); REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") != nullptr; }));
    click(workspace, "authoring.check"); REQUIRE(pump([&] { return control<juce::TextButton>(workspace, "authoring.apply")->isEnabled(); }));
    const auto version = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel")->ready()->package_sha256;
    REQUIRE(version != old.content_sha256); barrier->enabled = true; click(workspace, "authoring.apply");
    REQUIRE(pump([&] { return barrier->entered.load(); }));
    click(workspace, "project.new"); REQUIRE(workspace.controller().project().nodes.empty());
    const auto session = workspace.controller().session_identity(); const auto generation = workspace.controller().document_generation();
    barrier->release = true;
    REQUIRE(pump([&] { return control<juce::Label>(workspace, "authoring.check_status")->getText().contains("Available"); }));
    CHECK(workspace.controller().session_identity() == session); CHECK(workspace.controller().document_generation() == generation);
    CHECK_FALSE(workspace.controller().dirty()); REQUIRE(nodes->find("user.custom_node"));
    REQUIRE(nodes->find("user.custom_node")->plugin_origin->managed_content_sha256 == version);
    click(workspace, "authoring.close"); REQUIRE(pump([&] { return !control<juce::Component>(workspace, "authoring.panel"); }));
    click(workspace, "library.user.custom_node"); REQUIRE(workspace.controller().project().nodes.size() == 1);
    REQUIRE(workspace.controller().project().nodes.front().managed_version);
    CHECK(workspace.controller().project().nodes.front().managed_version->content_sha256 == version);
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "new.aaproj", workspace.controller().project())));
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-09.b production upgrade preview cancels cleanly and upgrades all package instances once",
          "[NA-09.b][NA-10.a][NA-10.b][NA-10.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0020-upgrade-preview" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring"); auto draft = audioalgo::authoring::python_draft("upgrade-two");
    const auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked)); auto applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied)); const auto old = std::get<audioalgo::authoring::AppliedPackage>(applied);
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "upgrade-two"; project.selected_output_node_id = "output";
    auto first = audioalgo::editor::make_node_instance(*old.catalog, {}, "user.custom_node", "first", -80, -60); REQUIRE(first);
    auto second = *first; second.id = "second"; second.x = 320; second.y = 100; first->parameters["gain"] = .6; second.parameters["gain"] = .8;
    first->serialized_extensions = {{"future_node", juce::JSON::toString(juce::JSON::parse("{\"preserve\":true}"), false).toStdString()}};
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, *first, second, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "first", "audio_in"}, {"first", "audio_out", "second", "audio_in"}, {"second", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513); for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    bool incompatible = false;
    SECTION("compatible code update preserves values layout connections selection and opaque fields") {}
    SECTION("one invalid instance rejects the entire package upgrade") { incompatible = true; }
    draft.source = "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n"; ++draft.generation;
    if (incompatible) { auto json = juce::JSON::parse(draft.declaration); auto* parameters = json["parameters"].getArray(); REQUIRE(parameters); (*parameters)[0].getDynamicObject()->setProperty("maximum", .7); draft.declaration = juce::JSON::toString(json, false).toStdString(); }
    checked = service->check(draft, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, old.catalog, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied));
    const auto latest = std::get<audioalgo::authoring::AppliedPackage>(applied); auto nodes = std::make_shared<audioalgo::NodeRegistry>(*latest.catalog);
    audioalgo::editor::EditorComponent workspace(nodes, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(),
        std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts"), false, service);
    workspace.open_project_file(root / "project.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "upgrade-two"; }));
    const auto generation = workspace.controller().document_generation(); const auto revision = workspace.controller().revision();
    REQUIRE(control<juce::TextButton>(workspace, "authoring.upgrade")); click(workspace, "authoring.upgrade");
    auto* summary = control<juce::Label>(workspace, "upgrade.summary"); REQUIRE(summary);
    CHECK(summary->getText().contains("first")); CHECK(summary->getText().contains("second")); CHECK(summary->getText().contains("user.custom_node"));
    CHECK(workspace.controller().document_generation() == generation); CHECK_FALSE(workspace.controller().dirty()); CHECK(workspace.controller().history().undo_size() == 0);
    if (incompatible) { CHECK_FALSE(control<juce::TextButton>(workspace, "upgrade.confirm")->isEnabled()); CHECK(summary->getText().contains("second")); click(workspace, "upgrade.cancel"); }
    else {
        click(workspace, "upgrade.cancel"); CHECK(workspace.controller().document_generation() == generation);
        click(workspace, "authoring.upgrade"); click(workspace, "upgrade.confirm");
        REQUIRE(workspace.controller().revision() == revision + 1); CHECK(workspace.controller().history().undo_size() == 1); CHECK(workspace.controller().results_stale()); CHECK(workspace.controller().dirty());
        const auto& updated = workspace.controller().project(); CHECK(updated.selected_output_node_id == "output"); CHECK(updated.connections.size() == 3);
        for (std::size_t i = 0; i < updated.nodes.size(); ++i) {
            CHECK(updated.nodes[i].parameters == project.nodes[i].parameters); CHECK(updated.nodes[i].x == project.nodes[i].x); CHECK(updated.nodes[i].y == project.nodes[i].y);
            CHECK(updated.nodes[i].serialized_extensions == project.nodes[i].serialized_extensions);
            if (updated.nodes[i].managed_version) CHECK(updated.nodes[i].managed_version->content_sha256 == latest.content_sha256);
        }
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "upgraded.aaproj", updated)));
        click(workspace, "project.run"); auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status"); const auto finished = pump([&] { return status->status_text().contains("Completed run"); }); INFO(status->status_text().toStdString()); REQUIRE(finished);
    }
    if (incompatible) {
        CHECK(workspace.controller().revision() == revision); CHECK(workspace.controller().document_generation() == generation); CHECK(workspace.controller().history().undo_size() == 0); CHECK_FALSE(workspace.controller().dirty());
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "rejected.aaproj", workspace.controller().project())));
        auto bytes = [](const auto& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
        CHECK(bytes(root / "project.aaproj") == bytes(root / "rejected.aaproj"));
    }
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-09.d production upgrade undo redo restores exact factories and real audio",
          "[NA-09.d][NA-09.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0021-upgrade-history" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring"); auto draft = audioalgo::authoring::python_draft("history-versions");
    const auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked)); auto applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied)); const auto old = std::get<audioalgo::authoring::AppliedPackage>(applied);
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "history-versions"; project.selected_output_node_id = "output";
    const auto custom = audioalgo::editor::make_node_instance(*old.catalog, {}, "user.custom_node", "custom", 320, 100); REQUIRE(custom);
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, *custom, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513); for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    draft.source = "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 1.5}\n"; ++draft.generation;
    checked = service->check(draft, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked));
    applied = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, old.catalog, cancel); REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(applied));
    const auto latest = std::get<audioalgo::authoring::AppliedPackage>(applied); auto nodes = std::make_shared<audioalgo::NodeRegistry>(*latest.catalog);
    audioalgo::editor::EditorComponent workspace(nodes, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(),
        std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "drafts"), false, service);
    workspace.open_project_file(root / "project.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "history-versions"; }));
    std::set<std::filesystem::path> seen;
    auto run = [&](const char* name, double amplitude, const std::string& version) {
        REQUIRE(workspace.controller().project().nodes[1].managed_version->content_sha256 == version);
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / (std::string(name) + ".aaproj"), workspace.controller().project())));
        const auto resolved = pump([&] { return nodes->find("user.custom_node") && nodes->find("user.custom_node")->plugin_origin->managed_content_sha256 == version; });
        REQUIRE(resolved);
        click(workspace, "project.run"); auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status");
        const auto done = pump([&] { return status->status_text().contains("Completed run"); }); INFO(status->status_text().toStdString()); REQUIRE(done);
        std::vector<std::filesystem::path> added;
        for (const auto& p : std::filesystem::directory_iterator(root / "runs")) if (std::filesystem::is_regular_file(p.path() / "output.wav") && !seen.contains(p.path())) added.push_back(p.path());
        REQUIRE(added.size() == 1); seen.insert(added.front());
        auto result = audioalgo::read_wav_file(added.front() / "output.wav"); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(result));
        const auto& audio = std::get<audioalgo::AudioBuffer>(result); REQUIRE(audio.frame_count() == 513);
        for (std::size_t n = 0; n < 513; ++n) { const auto expected = amplitude * std::sin(.03 * (n + 1)); CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected)); }
        std::ofstream(root / (std::string(name) + ".run"), std::ios::binary) << added.front().filename().string();
    };
    run("v1-before", .1, old.content_sha256); const auto revision = workspace.controller().revision();
    click(workspace, "authoring.upgrade"); click(workspace, "upgrade.confirm"); REQUIRE(workspace.controller().revision() == revision + 1);
    run("v2", .15, latest.content_sha256);
    REQUIRE(workspace.keyPressed(juce::KeyPress('z', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), 0)));
    REQUIRE(workspace.controller().revision() == revision + 2); CHECK(workspace.controller().history().redo_size() == 1);
    run("undo-v1", .1, old.content_sha256);
    REQUIRE(workspace.keyPressed(juce::KeyPress('y', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), 0)));
    REQUIRE(workspace.controller().revision() == revision + 3); CHECK(workspace.controller().history().redo_size() == 0);
    run("redo-v2", .15, latest.content_sha256);
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-11.c production Save carries the exact package and refuses conflicting destination bytes",
          "[NA-11.c][NA-11.d][NA-07.d][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0022-portable-save" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring"); auto draft = audioalgo::authoring::python_draft("portable-save");
    const auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked)); const auto result = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(result)); const auto installed = std::get<audioalgo::authoring::AppliedPackage>(result);
    const auto directory = root / "original"; std::filesystem::create_directories(directory);
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "portable-save"; project.selected_output_node_id = "output";
    const auto node = audioalgo::editor::make_node_instance(*installed.catalog, {}, "user.custom_node", "custom", 320, 100); REQUIRE(node);
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, *node, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(directory / "project.aaproj", project)));
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513); for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(directory / "input.wav", input, audioalgo::WavEncoding::float32)));
    auto native = [](const std::filesystem::path& path) {
#ifdef _WIN32
        return std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(path).wstring());
#else
        return path;
#endif
    };
    auto bytes = [&](const auto& path) { std::ifstream f(native(path), std::ios::binary); REQUIRE(f.is_open()); return std::string(std::istreambuf_iterator<char>(f), {}); };
    const auto original = bytes(directory / "project.aaproj"); const auto lock = *node->managed_version;
    std::ofstream(root / "original-project.bytes", std::ios::binary) << original;
    const auto carried = directory / std::filesystem::u8path(lock.package_path);
    bool conflict = false;
    SECTION("saved package survives moved project and unavailable global source") {}
    SECTION("preexisting different destination is preserved and save fails without replacing project") { conflict = true; }
    if (conflict) { std::filesystem::create_directories(native(carried)); std::ofstream(native(carried / "user_source.py"), std::ios::binary) << "user owned bytes must not be replaced"; }
    {
        auto nodes = std::make_shared<audioalgo::NodeRegistry>(*installed.catalog);
        audioalgo::editor::EditorComponent workspace(nodes, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(),
            std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "authoring/drafts"), false, service);
        workspace.open_project_file(directory / "project.aaproj"); REQUIRE(pump([&] { return workspace.controller().project().project_id == "portable-save"; }));
        click(workspace, "project.save"); auto* status = control<audioalgo::editor::RunStatusPanel>(workspace, "project.status");
        if (conflict) {
            REQUIRE(pump([&] { return status->status_text().contains("authoring_carry_failed"); }));
            CHECK(bytes(directory / "project.aaproj") == original); CHECK(bytes(carried / "user_source.py") == "user owned bytes must not be replaced"); return;
        }
        REQUIRE(pump([&] { return status->status_text().contains("Saved"); }));
        REQUIRE(std::filesystem::is_regular_file(native(carried / "snapshot.json")));
        CHECK(bytes(carried / "user_source.py") == draft.source);
    }
    const auto moved = root / std::filesystem::u8path("移动 中文 空格 工程"); std::filesystem::rename(directory, moved);
    // Only this test-owned source tree is made unavailable; no user package directory is touched.
    REQUIRE((root / "authoring").lexically_relative(root) == std::filesystem::path("authoring"));
    std::filesystem::rename(root / "authoring", root / "unavailable-authoring");
    auto empty = std::make_shared<audioalgo::NodeRegistry>(*base); auto isolated = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "empty-authoring");
    audioalgo::editor::EditorComponent reopened(empty, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(),
        std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "empty-drafts"), false, isolated);
    reopened.open_project_file(moved / "project.aaproj"); REQUIRE(pump([&] { return reopened.controller().project().project_id == "portable-save"; }));
    REQUIRE(empty->find("user.custom_node")); CHECK(empty->find("user.custom_node")->plugin_origin->managed_content_sha256 == lock.content_sha256);
    click(reopened, "project.run"); auto* status = control<audioalgo::editor::RunStatusPanel>(reopened, "project.status");
    const auto done = pump([&] { return status->status_text().contains("Completed run"); }); INFO(status->status_text().toStdString()); REQUIRE(done);
    const auto previous = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {}).toStdString();
    struct Restore { std::string value; ~Restore() { _putenv_s("AUDIOALGO_AUTHORING_ROOT", value.c_str()); } } restore{previous};
    REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT", (root / "empty-authoring").string().c_str()) == 0);
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    auto utf8 = [](const std::filesystem::path& path) { const auto text = path.u8string(); return juce::String::fromUTF8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size())); };
    juce::ChildProcess child; REQUIRE(child.start(juce::StringArray{executable.getFullPathName(), "run", utf8(moved / "project.aaproj"), utf8(moved / "cli"), "127"}));
    auto output = std::async(std::launch::async, [&] { return child.readAllProcessOutput(); });
    const auto finished = child.waitForProcessToFinish(10000); if (!finished) child.kill(); const auto log = output.get(); INFO(log.toStdString()); REQUIRE(finished);
    std::ofstream(root / "cli.log", std::ios::binary) << log.toStdString(); REQUIRE(child.getExitCode() == 0);
    REQUIRE(std::filesystem::is_regular_file(native(moved / "cli/output.wav")));
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NA-08.d relative CLI project path resolves carried exact code without a global package",
          "[NA-08.d][NA-11.c][node_authoring][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0023-relative-cli" / juce::Uuid().toString().toStdString();
    auto base = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*base));
    auto service = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring"); auto draft = audioalgo::authoring::python_draft("relative-cli");
    const auto cancel = std::make_shared<std::atomic<bool>>(false); auto checked = service->check(draft, cancel);
#if AUDIOALGO_AUTHORING_PYTHON_ENABLED
    REQUIRE(std::holds_alternative<audioalgo::authoring::ReadyPackage>(checked)); const auto result = service->apply(std::get<audioalgo::authoring::ReadyPackage>(checked), draft, base, cancel);
    REQUIRE(std::holds_alternative<audioalgo::authoring::AppliedPackage>(result)); const auto installed = std::get<audioalgo::authoring::AppliedPackage>(result);
    const auto directory = root / "project"; std::filesystem::create_directories(directory);
    audioalgo::ProjectModel project; project.schema_version = 2; project.project_id = "relative-cli"; project.selected_output_node_id = "output";
    const auto node = audioalgo::editor::make_node_instance(*installed.catalog, {}, "user.custom_node", "custom", 320, 100); REQUIRE(node);
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}}, *node, {"output", "builtin.output", {}}};
    project.connections = {{"file", "audio", "custom", "audio_in"}, {"custom", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(directory / "project.aaproj", project)));
    REQUIRE_FALSE(service->carry_packages(audioalgo::authoring::managed_selections(project), {}, directory));
    audioalgo::AudioBuffer input(16000, {"ch0"}, 513); for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(.2 * std::sin(.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(directory / "input.wav", input, audioalgo::WavEncoding::float32)));
    const auto previous = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {}).toStdString();
    const auto cwd = std::filesystem::current_path();
    struct Restore { std::string environment; std::filesystem::path cwd; ~Restore() { _putenv_s("AUDIOALGO_AUTHORING_ROOT", environment.c_str()); std::filesystem::current_path(cwd); } } restore{previous, cwd};
    REQUIRE(_putenv_s("AUDIOALGO_AUTHORING_ROOT", (root / "empty-authoring").string().c_str()) == 0);
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    std::filesystem::current_path(directory);
    juce::ChildProcess child; REQUIRE(child.start(juce::StringArray{executable.getFullPathName(), "run", "project.aaproj", "run", "127"}));
    auto output = std::async(std::launch::async, [&] { return child.readAllProcessOutput(); });
    const auto done = child.waitForProcessToFinish(10000); if (!done) child.kill(); const auto log = output.get(); INFO(log.toStdString()); REQUIRE(done);
    std::ofstream(root / "cli.log", std::ios::binary) << log.toStdString(); REQUIRE(child.getExitCode() == 0);
    auto read = audioalgo::read_wav_file(directory / "run/output.wav"); REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
    const auto& audio = std::get<audioalgo::AudioBuffer>(read); REQUIRE(audio.frame_count() == 513);
    for (std::size_t n = 0; n < 513; ++n) { const auto expected = .1 * std::sin(.03 * (n + 1)); CHECK(std::abs(audio.channel(0)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected)); }
#else
    REQUIRE(std::get<audioalgo::authoring::SourceDiagnostic>(checked).code == "python_disabled");
#endif
}

TEST_CASE("NWS-01.a production panel creates a node project folder and reopens identical declaration and source",
          "[NWS-01.a][NWS-01.b][node_workspace][component]") {
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/NWS0001-workspace-create/panel" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    const auto lock_path = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_workspace/expected/workspace_lock.json";
    const auto lock_text = file_bytes(lock_path);
    const auto lock = juce::JSON::parse(juce::String::fromUTF8(lock_text.data(), static_cast<int>(lock_text.size())));
    const auto conversion = lock["conversion"];

    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<FileDraftStorage>(root / "recovery");
    audioalgo::editor::DraftFileActions::Completion draft_selection;
    audioalgo::editor::DraftFileActions::Completion directory_selection;
    std::string directory_purpose;
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<Documents>(),
        std::make_shared<audioalgo::ProjectResourceService>(), std::make_shared<Preview>(), storage, false, {}, {}, true,
        [&](bool, const std::filesystem::path&, auto completion) { draft_selection = std::move(completion); },
        {}, {}, {},
        [&](const std::string& purpose, const std::filesystem::path&, audioalgo::editor::DraftFileActions::Completion completion) {
            directory_purpose = purpose;
            directory_selection = std::move(completion);
        });
    const auto project_revision = workspace.controller().project().revision;
    const auto legacy = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "tests/fixtures/node_workspace/legacy/print_a.aadraft";
    const auto legacy_before = file_bytes(legacy);
    REQUIRE_FALSE(legacy_before.empty());

    // Production Open reads the frozen legacy draft; nothing is rewritten.
    click(workspace, "authoring.open");
    REQUIRE(draft_selection);
    draft_selection(legacy);
    REQUIRE(pump([&] { return control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel") != nullptr; }));
    auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    REQUIRE(panel);
    const auto legacy_draft = panel->session().document();
    CHECK(legacy_draft.id == "print-a");
    CHECK(legacy_draft.language == "python");

    // Production intent creates the directory from that declaration and source.
    const auto target = root / std::filesystem::u8path("Print A 工程");
    click(*panel, "authoring.workspace_create");
    REQUIRE(directory_selection);
    CHECK(directory_purpose.find("Create node project") != std::string::npos);
    directory_selection(target);
    REQUIRE(pump([&] { return !panel->workspace_creating(); }));
    REQUIRE(panel->workspace_created());
    CHECK(panel->workspace_directory() == target);
    CHECK(panel->workspace_content_sha256() == conversion["content_sha256"].toString().toStdString());
    // The directory bytes are the frozen independent expectation, not this run's output.
    CHECK(file_bytes(target / "node.json") == conversion["node_json"].toString().toStdString());
    CHECK(file_bytes(target / "src" / "print_a" / "process.py") == legacy_draft.source);
    const auto* expected_files = conversion["runtime_files"].getArray();
    REQUIRE(expected_files != nullptr);
    REQUIRE(expected_files->size() == 3);
    for (int index = 0; index < expected_files->size(); ++index) {
        const auto& entry = (*expected_files)[index];
        const auto relative = entry["path"].toString().toStdString();
        INFO(relative);
        CHECK(WorkspaceStore::sha256_hex(file_bytes(target / relative)) == entry["sha256"].toString().toStdString());
    }
    CHECK(file_bytes(legacy) == legacy_before);

    // Close and reopen through the production library entry.
    click(workspace, "authoring.close");
    REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") == nullptr; }));
    const auto node_json_bytes = file_bytes(target / "node.json");
    click(workspace, "authoring.open_workspace");
    REQUIRE(directory_selection);
    CHECK(directory_purpose.find("Open node project") != std::string::npos);
    directory_selection(target);
    REQUIRE(pump([&] { return control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel") != nullptr; }));
    auto* reopened = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel");
    REQUIRE(reopened);
    CHECK(reopened->workspace_mode());
    CHECK(reopened->session().document().language == legacy_draft.language);
    CHECK(reopened->session().document().source == legacy_draft.source);
    CHECK(reopened->session().document().declaration == conversion["declaration"].toString().toStdString());
    CHECK(reopened->workspace_directory() == target);
    CHECK(reopened->workspace_content_sha256() == conversion["content_sha256"].toString().toStdString());
    CHECK(file_bytes(target / "node.json") == node_json_bytes);
    CHECK(file_bytes(legacy) == legacy_before);
    // Directory mode is read-only in this host: no Save target and no project mutation.
    auto* save = control<juce::TextButton>(*reopened, "authoring.save");
    REQUIRE(save);
    CHECK_FALSE(save->isVisible());
    CHECK_FALSE(control<juce::TextButton>(workspace, "authoring.workspace_create")->isEnabled());
    CHECK(workspace.controller().project().revision == project_revision);
    click(workspace, "authoring.close");
    REQUIRE(pump([&] { return control<juce::Component>(workspace, "authoring.panel") == nullptr; }));
}
