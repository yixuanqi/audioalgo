#include "NodeAuthoringPanel.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
template<class T> T* find(juce::Component& parent, const juce::String& id) {
    if (parent.getComponentID() == id) return dynamic_cast<T*>(&parent);
    for (auto* child : parent.getChildren()) if (auto* found = find<T>(*child, id)) return found;
    return nullptr;
}

template<class Predicate> bool pump(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!done() && std::chrono::steady_clock::now() < deadline) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    return done();
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

TEST_CASE("NWS-05.b directory Save As shortcut never serializes a source-empty legacy draft",
          "[NWS-05.b][NWS0016][node_workspace][component]") {
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NWS0016-directory-save-as/keyboard" / juce::Uuid().toString().toStdString();
    const auto created = WorkspaceStore{}.create(root / "author", python_draft("shortcut")); REQUIRE(std::holds_alternative<Workspace>(created));
    const auto workspace = std::get<Workspace>(created);
    bool has_legacy_chooser = false;
    SECTION("The production host supplies a draft chooser") { has_legacy_chooser = true; }
    SECTION("The embedded panel has no draft chooser") {}
    int legacy_choices = 0;
    editor::DraftFileActions actions;
    actions.workspace = editor::DraftFileActions::WorkspaceBinding{workspace.directory, workspace.content_sha256, workspace.files.size(), 0, workspace};
    if (has_legacy_chooser) actions.choose_save = [&](const auto&, auto completion) { ++legacy_choices; completion(root / "incomplete.aadraft"); };
    editor::NodeAuthoringPanel panel(workspace_identity(workspace), true, std::make_shared<FileDraftStorage>(root / "recovery"), [] {}, {}, {}, {}, {}, {}, actions);
    auto* name = find<juce::TextEditor>(panel, "authoring.display_name"); REQUIRE(name);
    name->setText("Keep this directory edit", false); name->onTextChange();
    const auto unsaved = panel.session().document(); REQUIRE(panel.session().dirty());
    const auto source = bytes(root / "author/src/shortcut/process.py");
    REQUIRE(panel.keyPressed(juce::KeyPress('S', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier), 0)));
    REQUIRE(pump([&] { return !panel.file_save_pending(); }));
    CHECK(legacy_choices == 0);
    CHECK_FALSE(std::filesystem::exists(root / "incomplete.aadraft"));
    CHECK(panel.session().dirty()); CHECK(panel.session().document() == unsaved);
    CHECK(panel.file_path().empty()); CHECK(panel.session().document().source.empty());
    CHECK(bytes(root / "author/node.json") == workspace.node_json);
    CHECK(bytes(root / "author/src/shortcut/process.py") == source);
    CHECK(find<juce::Label>(panel, "authoring.check_status")->getText().containsIgnoreCase("Save definition"));
    CHECK_FALSE(std::filesystem::exists(root / "recovery"));
    // A denied Save As must leave the ordinary directory Save shortcut usable.
    REQUIRE(panel.keyPressed(juce::KeyPress('S', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), 0)));
    REQUIRE(pump([&] { return !panel.file_save_pending() && !panel.session().dirty(); }));
    CHECK(bytes(root / "author/node.json").find("Keep this directory edit") != std::string::npos);
    CHECK(panel.file_path().empty()); CHECK(panel.session().document().source.empty());
    CHECK(bytes(root / "author/src/shortcut/process.py") == source);
    CHECK_FALSE(std::filesystem::exists(root / "incomplete.aadraft"));
}
void click(juce::Component& parent, const char* id) {
    auto* button = find<juce::TextButton>(parent, id); REQUIRE(button); REQUIRE(button->isEnabled());
    button->onClick(); // The production intent callback, with async work pumped below.
}
}

TEST_CASE("NWS-05.b production directory form saves node json and retains local edits on disk conflict",
          "[NWS-05.b][NWS0015][node_workspace][component]") {
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NWS0015-declaration-save/panel" / juce::Uuid().toString().toStdString();
    std::cout << "NWS0015_PANEL_ROOT=" << root.generic_string() << '\n';
    const auto draft = python_draft("panel-save");
    const auto outcome = WorkspaceStore{}.create(root / "author", draft); REQUIRE(std::holds_alternative<Workspace>(outcome));
    const auto disk = std::get<Workspace>(outcome);
    editor::DraftFileActions actions; actions.workspace = editor::DraftFileActions::WorkspaceBinding{disk.directory, disk.content_sha256, disk.files.size(), 0, disk};
    struct SaveGate { std::atomic<bool> held{false}, entered{false}, release{false}; };
    const auto gate = std::make_shared<SaveGate>();
    actions.save_workspace_declaration = [gate](const Workspace& expected, const std::string& declaration) -> WorkspaceOutcome {
        if (gate->held.load()) {
            gate->entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!gate->release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!gate->release.load()) return Diagnostic{"test_gate_timeout", "The test did not release its save barrier"};
        }
        return WorkspaceStore{}.save_declaration(expected, declaration);
    };
    auto catalog = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*catalog));
    auto checks = std::make_shared<PythonAuthoring>(root / "library"); int closed = 0;
    editor::NodeAuthoringPanel panel(workspace_identity(disk), true, std::make_shared<FileDraftStorage>(root / "recovery"), [&] { ++closed; }, checks,
        [&] { return catalog; }, [](auto) {}, {}, {}, actions);
    panel.setSize(1100, 700);
    // Release a blocked worker even when a REQUIRE exits its section early.
    struct ReleaseGate { std::shared_ptr<SaveGate> gate; ~ReleaseGate() { gate->release.store(true); } } release_gate{gate};
    auto* name = find<juce::TextEditor>(panel, "authoring.display_name"); REQUIRE(name);
    auto* save = find<juce::TextButton>(panel, "authoring.save"); REQUIRE(save);
    CHECK(name->isVisible()); CHECK(name->isEnabled()); CHECK(save->isVisible());
    click(panel, "authoring.check"); REQUIRE(pump([&] { return panel.ready().has_value(); }));
    name->setText("Local saved name", false); name->onTextChange();
    CHECK(panel.session().document().declaration.find("Local saved name") != std::string::npos);
    CHECK(panel.session().document().source.empty()); CHECK(panel.session().dirty()); CHECK_FALSE(panel.ready());
    CHECK_FALSE(find<juce::TextButton>(panel, "authoring.check")->isEnabled());
    CHECK_FALSE(find<juce::TextButton>(panel, "authoring.apply")->isEnabled());
    const auto source_before = bytes(root / "author/src/panel_save/process.py");
    SECTION("Save persists the form then requires a new Check") {
        click(panel, "authoring.save");
        REQUIRE(pump([&] { return bytes(root / "author/node.json").find("Local saved name") != std::string::npos && !panel.session().dirty(); }));
        CHECK(panel.session().document().source.empty()); CHECK_FALSE(panel.ready());
        CHECK(find<juce::TextButton>(panel, "authoring.check")->isEnabled());
        CHECK_FALSE(find<juce::TextButton>(panel, "authoring.apply")->isEnabled());
        const auto reopened = WorkspaceStore{}.open(root / "author"); REQUIRE(std::holds_alternative<Workspace>(reopened));
        CHECK(std::get<Workspace>(reopened).declaration == panel.session().document().declaration);
        click(panel, "authoring.check"); REQUIRE(pump([&] { return panel.ready().has_value(); }));
        CHECK(panel.ready()->descriptor.display_name == "Local saved name");
    }
    SECTION("Refresh Save and Close preserve both the external bytes and unsaved form") {
        const auto external = disk.node_json + "\n";
        { std::ofstream output(root / "author/node.json", std::ios::binary); output << external; }
        click(panel, "authoring.workspace_refresh");
        REQUIRE(pump([&] { return find<juce::Label>(panel, "authoring.status")->getText().containsIgnoreCase("conflict"); }));
        CHECK(name->getText() == "Local saved name"); CHECK(panel.session().dirty());
        click(panel, "authoring.save");
        REQUIRE(pump([&] { return find<juce::Label>(panel, "authoring.status")->getText().contains("workspace_definition_conflict"); }));
        CHECK(bytes(root / "author/node.json") == external);
        CHECK(panel.session().document().declaration.find("Local saved name") != std::string::npos);
        click(panel, "authoring.close"); REQUIRE(pump([&] { return !panel.file_save_pending(); }));
        CHECK(closed == 0); CHECK(panel.session().dirty());
    }
    SECTION("Late Save completion preserves newer edits and a second Save publishes them") {
        gate->held.store(true);
        click(panel, "authoring.save"); REQUIRE(pump([&] { return gate->entered.load(); }));
        CHECK(panel.file_save_pending()); CHECK_FALSE(save->isEnabled());
        name->setText("Newer local name", false); name->onTextChange();
        gate->release.store(true);
        REQUIRE(pump([&] { return !panel.file_save_pending(); }));
        CHECK(bytes(root / "author/node.json").find("Local saved name") != std::string::npos);
        CHECK(name->getText() == "Newer local name"); CHECK(panel.session().dirty());
        CHECK_FALSE(panel.ready()); CHECK(panel.session().document().source.empty());
        CHECK_FALSE(find<juce::TextButton>(panel, "authoring.check")->isEnabled());
        click(panel, "authoring.save"); REQUIRE(pump([&] { return !panel.file_save_pending() && !panel.session().dirty(); }));
        CHECK(bytes(root / "author/node.json").find("Newer local name") != std::string::npos);
        CHECK(find<juce::TextButton>(panel, "authoring.check")->isEnabled());
    }
    SECTION("Source refresh preserves the unsaved declaration and Save preserves the new source") {
        const auto new_source = source_before + "\n# external editor edit\n";
        { std::ofstream output(root / "author/src/panel_save/process.py", std::ios::binary); output << new_source; }
        click(panel, "authoring.workspace_refresh");
        const auto reopened = WorkspaceStore{}.open(root / "author"); REQUIRE(std::holds_alternative<Workspace>(reopened));
        REQUIRE(pump([&] { return panel.workspace_content_sha256() == std::get<Workspace>(reopened).content_sha256; }));
        CHECK(name->getText() == "Local saved name"); CHECK(panel.session().dirty());
        CHECK(panel.session().document().source.empty());
        click(panel, "authoring.save"); REQUIRE(pump([&] { return !panel.file_save_pending() && !panel.session().dirty(); }));
        CHECK(bytes(root / "author/src/panel_save/process.py") == new_source);
        // Restore only this test's source fixture before the common byte check.
        { std::ofstream output(root / "author/src/panel_save/process.py", std::ios::binary); output << source_before; }
    }
    CHECK(bytes(root / "author/src/panel_save/process.py") == source_before);
    CHECK_FALSE(std::filesystem::exists(root / "recovery"));
}
