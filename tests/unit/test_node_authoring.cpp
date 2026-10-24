#include "EditorNodeLibrary.h"
#include "EditorWorkspace.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include <fstream>
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
    audioalgo::AudioPreviewOutcome play(const std::filesystem::path&) override { return std::monostate{}; }
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
    pathlib.Path("pc03-check-processes.txt").write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    print("pc03-check-entered", flush=True)
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
        std::error_code ec;
        const auto staging = checks->work_directory() / "staging";
        if (!std::filesystem::exists(staging, ec) || ec) return false;
        for (std::filesystem::recursive_directory_iterator it(staging, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() == "pc03-check-processes.txt") {
                DWORD observed_parent = 0, observed_child = 0;
                std::ifstream in(it->path());
                if (in >> observed_parent >> observed_child) {
                    marker = it->path(); parent_pid = observed_parent; child_pid = observed_child; return true;
                }
            }
        }
        return false;
    }, 15000));
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
    std::error_code archive_ec;
    for (std::filesystem::recursive_directory_iterator it(checks->work_directory() / "staging",
             std::filesystem::directory_options::skip_permission_denied, archive_ec), end;
         !archive_ec && it != end; it.increment(archive_ec)) {
        if (!it->is_regular_file(archive_ec) || it->path().filename() != "stdout.log") continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(in), {});
        if (bytes.find("pc03-check-entered") != std::string::npos) { archived_marker = true; break; }
    }
    CHECK(archived_marker);

    auto* source = control<juce::CodeEditorComponent>(panel, "authoring.source"); REQUIRE(source);
    source->selectAll();
    source->insertTextAtCaret("def process(inputs, params, state):\n    print('pc03-check-recovered', flush=True)\n    return {'audio_out': inputs['audio_in']}\n");
    click(panel, "authoring.check");
    REQUIRE(pump([&] { return panel.ready().has_value(); }, 15000));
    auto* check_log = control<juce::TextEditor>(panel, "authoring.check_logs"); REQUIRE(check_log);
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
    pathlib.Path("pc03-check-processes.txt").write_text(f"{os.getpid()} {child.pid}\n", encoding="ascii")
    print("pc03-check-entered", flush=True)
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
        std::error_code ec;
        const auto staging = checks->work_directory() / "staging";
        if (!std::filesystem::exists(staging, ec) || ec) return false;
        for (std::filesystem::recursive_directory_iterator it(staging, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() == "pc03-check-processes.txt") {
                DWORD observed_parent = 0, observed_child = 0;
                std::ifstream in(it->path());
                if (in >> observed_parent >> observed_child) {
                    marker = it->path(); parent_pid = observed_parent; child_pid = observed_child; return true;
                }
            }
        }
        return false;
    }, 15000));
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
    auto* check_log = control<juce::TextEditor>(*panel, "authoring.check_logs"); REQUIRE(check_log);
    CHECK(check_log->getText().contains("pc03-check-recovered"));
    panel.reset();
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
void print_workspace_scenario(bool failure_checks, bool check_log_checks = false, bool live_checks = false, bool heartbeat_checks = false, bool filter_checks = false) {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / (filter_checks ? "build/reports/loops/PC0404-node-filter/artifacts" : failure_checks ? "build/reports/loops/PC0301-run-log/artifacts" : "build/reports/loops/PC0201-print/artifacts") / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    audioalgo::AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t n = 0; n < 513; ++n) input.mutable_channel(0)[n] = static_cast<float>(0.2 * std::sin(0.03 * (n + 1)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root / "input.wav", input, audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project; project.project_id = "pc02-print"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 40, 100}, {"output", "builtin.output", {}, 640, 100}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root / "project.aaproj", project)));
    auto registry = std::make_shared<audioalgo::NodeRegistry>(); REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto storage = std::make_shared<audioalgo::authoring::FileDraftStorage>(root / "authoring/drafts");
    auto checks = std::make_shared<audioalgo::authoring::PythonAuthoring>(root / "authoring");
    audioalgo::editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<audioalgo::ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false, checks);
    workspace.open_project_file(root / "project.aaproj");
    REQUIRE(pump([&] { return workspace.controller().project().project_id == project.project_id; }));
    click(workspace, "authoring.new");
    control<juce::TextEditor>(workspace, "authoring.type_id")->setText("user.pc02_print", true);
    control<juce::TextEditor>(workspace, "authoring.display_name")->setText("Print", true);
    auto* editor = control<juce::CodeEditorComponent>(workspace, "authoring.source"); REQUIRE(editor);
    std::string body = "def process(inputs, params, state):\n    print('aaaaaaa', flush=True)\n";
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
        auto* check_logs = control<juce::TextEditor>(workspace, "authoring.check_logs"); REQUIRE(check_logs);
        auto* panel = control<audioalgo::editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(panel);
        const auto text = check_logs->getText();
        CHECK(text.contains("Check")); CHECK(text.contains("contract")); CHECK(text.contains(panel->session().document().id));
        CHECK(text.contains(panel->ready()->source_sha256)); CHECK(text.contains(panel->ready()->declaration_sha256));
        CHECK(text.contains(panel->ready()->request_id));
        const auto raw = text.toStdString(); std::size_t count = 0, offset = 0;
        while ((offset = raw.find("aaaaaaa", offset)) != std::string::npos) { ++count; offset += 7; }
        CHECK(count == 5); // independent probe contract: 513 frames, host127
        auto* run_logs = control<juce::TextEditor>(workspace, "project.run_logs"); REQUIRE(run_logs);
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
        auto* logs = control<juce::TextEditor>(workspace, "project.run_logs"); REQUIRE(logs);
        const auto text = logs->getText();
        CHECK(text.contains(custom)); CHECK(text.contains("user.pc02_print")); CHECK(text.contains("pc02-print"));
        CHECK(text.contains("aaaaaaa\n")); // ADR0039: chunk headers may split the two records
        const auto log_bytes = text.toStdString(); std::size_t occurrences = 0, offset = 0;
        while ((offset = log_bytes.find("aaaaaaa", offset)) != std::string::npos) { ++occurrences; offset += 7; }
        CHECK(occurrences == (filter_checks ? 4 : 2));
        CHECK(text != juce::String(previous)); previous = text.toStdString();
        std::ofstream(root / ("ui-log-" + std::to_string(attempt) + ".txt"), std::ios::binary) << previous;
    }
    if (filter_checks) {
        auto* logs = control<juce::TextEditor>(workspace, "project.run_logs"); REQUIRE(logs);
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
    if (live_checks) {
        auto* gain = control<juce::Slider>(workspace, "parameter.gain"); REQUIRE(gain);
        gain->setValue(heartbeat_checks ? -2 : -1.5, juce::sendNotificationSync); click(workspace, "project.run");
        auto* logs = control<juce::TextEditor>(workspace, "project.run_logs"); REQUIRE(logs);
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
        auto* logs = control<juce::TextEditor>(workspace, "project.run_logs"); REQUIRE(logs);
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
            workspace.run_log_received(std::move(stale)); CHECK(logs->getText() == baseline);
        }
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
TEST_CASE("PC-04.a production log text joins partial stream chunks without invented newlines", "[PC-04.a][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel; panel.setSize(600,150);
    audioalgo::NodeLogEvent event;event.identity={"project",1,"request",2};event.node_id="node";event.source="stdout";event.level="info";
    event.text="part";panel.append(event);event.text="ial without newline";panel.append(event);
    auto* text=control<juce::TextEditor>(panel,"project.run_logs");REQUIRE(text);
    CHECK(text->getText().contains("partial without newline"));
}

TEST_CASE("PC-04.d production run log node filter preserves hidden current run entries", "[PC-04.d][component]") {
    juce::ScopedJuceInitialiser_GUI gui;
    audioalgo::editor::RunLogPanel panel; panel.setSize(720,240);
    auto* text=control<juce::TextEditor>(panel,"project.run_logs"); REQUIRE(text);
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
