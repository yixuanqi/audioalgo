#include "EditorNodeLibrary.h"
#include "NodeAuthoringPanel.h"
#include "EditorWorkspace.h"
#include "audio_editor_application/EditorController.h"
#include "audio_native_plugins/NativePlugins.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "audio_project/ProjectFileIO.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <chrono>
#include <set>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto absolute = std::filesystem::absolute(path).wstring();
    if (absolute.starts_with(L"\\\\?\\")) return path;
    return absolute.starts_with(L"\\\\") ? std::filesystem::path(L"\\\\?\\UNC\\" + absolute.substr(2))
                                       : std::filesystem::path(L"\\\\?\\" + absolute);
#else
    return path;
#endif
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(io_path(path), std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(io_path(path), std::ios::binary); out << content;
    REQUIRE(out.good());
}
std::string canonical(const juce::var& value) {
    std::string error;
    auto result = audioalgo::authoring::WorkspaceStore::canonical_json(juce::JSON::toString(value, true).toStdString(), error);
    REQUIRE(error.empty());
    return result;
}
template<class T> T* child(juce::Component& root, const juce::String& id) {
    if (root.getComponentID() == id) return dynamic_cast<T*>(&root);
    for (auto* component : root.getChildren())
        if (auto* found = child<T>(*component, id)) return found;
    return nullptr;
}
template<class Predicate> bool pump(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready() && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    return ready();
}
void activate(juce::Component& root, const char* id) {
    auto* button = child<juce::TextButton>(root, id); REQUIRE(button); REQUIRE(button->isEnabled());
    struct Observed final : juce::Button::Listener {
        bool received = false;
        void buttonClicked(juce::Button*) override { received = true; }
    } observed;
    juce::Component::SafePointer<juce::TextButton> safe(button);
    button->addListener(&observed); button->triggerClick();
    const bool clicked = pump([&] { return observed.received; }); if (safe != nullptr) safe->removeListener(&observed); REQUIRE(clicked);
}
class FileDocuments final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path& path) const override { return audioalgo::load_project_file(path); }
    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path& path, const audioalgo::ProjectModel& model) const override { return audioalgo::save_project_file(path, model); }
};
class Preview final : public audioalgo::AudioPreviewService {
public:
    audioalgo::AudioPreviewOutcome play(const std::filesystem::path&) override { return std::monostate{}; }
    void pause() override {} void stop() override {} void shutdown() override {}
    bool is_playing() const noexcept override { return false; }
    double position_seconds() const noexcept override { return 0; }
};
}

TEST_CASE("NLC-07.c Python external single file and directory browser routes never import on browsing",
          "[NLC-07.c][node_library][node_authoring][integration]") {
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc07c/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout << "NLC07C_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    const std::string browser_json = R"({"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["route-marker","增益"],"description":"Python route browsing"})";
    const auto browser = juce::JSON::parse(juce::String::fromUTF8(browser_json.c_str()));
    auto catalog = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*catalog));
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    PythonAuthoring service(root / "library");
    juce::Array<juce::var> receipts;
    std::vector<std::pair<std::filesystem::path, std::string>> markers;
    std::vector<std::pair<std::filesystem::path, std::string>> immutable;
    std::vector<std::string> types;
    for (const bool directory : {false, true}) {
        const std::string route = directory ? "directory" : "single";
        auto draft = python_draft("nlc07c-" + route);
        const std::string type = "user.browser_" + route; types.push_back(type);
        auto declaration = juce::JSON::parse(juce::String::fromUTF8(draft.declaration.c_str()));
        declaration.getDynamicObject()->setProperty("type_id", juce::String(type));
        declaration.getDynamicObject()->setProperty("browser", browser);
        draft.declaration = juce::JSON::toString(declaration, false).toStdString();
        const auto marker = root / (route + "-imports.txt");
        draft.source += "\nimport os, pathlib\nwith pathlib.Path(r'" + marker.generic_string() +
            "').open('a', encoding='utf-8') as marker:\n    marker.write(str(os.getpid()) + '\\n')\n";
        std::optional<Workspace> disk;
        if (directory) {
            auto created = WorkspaceStore{}.create(root / "author", draft);
            if (const auto* error = std::get_if<Diagnostic>(&created)) INFO(error->code + ": " + error->message);
            REQUIRE(std::holds_alternative<Workspace>(created));
            disk = std::get<Workspace>(created); draft = workspace_identity(*disk);
            REQUIRE(draft.source.empty());
            CHECK(canonical(juce::JSON::parse(juce::String::fromUTF8(disk->node_json.c_str()))["node"]["browser"]) == canonical(browser));
            immutable.emplace_back(root / "author/node.json", bytes(root / "author/node.json"));
        }
        auto checking = std::async(std::launch::async, [&] {
            return directory ? service.check_workspace(draft, *disk, cancel) : service.check(draft, cancel);
        });
        auto checked = checking.get();
        if (const auto* error = std::get_if<SourceDiagnostic>(&checked)) INFO(error->code + ": " + error->message);
        REQUIRE(std::holds_alternative<ReadyPackage>(checked));
        const auto& ready = std::get<ReadyPackage>(checked);
        REQUIRE(ready.descriptor.browser_classification.has_value());
        CHECK(ready.descriptor.browser_classification->category_id == "audio.dynamics");
        CHECK(ready.descriptor.browser_classification->subcategory_id == std::optional<std::string>{"gain"});
        CHECK(ready.descriptor.browser_tags == std::vector<std::string>{"route-marker", "增益"});
        CHECK(ready.descriptor.browser_description == "Python route browsing");
        CHECK(ready.descriptor.capabilities.requires_offline_task);
        const auto marker_after_check = bytes(marker); REQUIRE_FALSE(marker_after_check.empty());
        std::istringstream pids(marker_after_check); unsigned int pid = 0; std::size_t imports = 0;
        while (pids >> pid) { CHECK(pid == ready.probe_pid); ++imports; }
        REQUIRE(imports > 0);
        auto applying = std::async(std::launch::async, [&] { return service.apply(ready, draft, catalog, cancel); });
        const auto applied = applying.get();
        if (const auto* error = std::get_if<SourceDiagnostic>(&applied)) INFO(error->code + ": " + error->message);
        REQUIRE(std::holds_alternative<AppliedPackage>(applied));
        const auto& installed = std::get<AppliedPackage>(applied);
        catalog = std::make_shared<NodeRegistry>(*installed.catalog);
        CHECK(bytes(marker) == marker_after_check);
        const auto manifest_path = installed.package_root / "manifest.json";
        immutable.emplace_back(manifest_path, bytes(manifest_path));
        auto manifest = juce::JSON::parse(juce::String::fromUTF8(bytes(manifest_path).c_str()));
        CHECK(canonical(manifest["nodes"][0]["browser"]) == canonical(browser));
        markers.emplace_back(marker, marker_after_check);
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
        receipt->setProperty("route", juce::String(route)); receipt->setProperty("type_id", juce::String(type));
        receipt->setProperty("content_sha256", juce::String(installed.content_sha256));
        receipt->setProperty("package_root", juce::String(installed.package_root.generic_string()));
        receipt->setProperty("check_imports", static_cast<int>(imports)); receipts.add(juce::var(receipt.get()));
    }
    const auto external = root / "external"; std::filesystem::create_directories(external);
    const auto external_marker = root / "external-imported.txt";
    write(external / "never_import.py", "import pathlib\npathlib.Path(r'" + external_marker.generic_string() +
        "').write_text('IMPORTED')\nraise RuntimeError('Browsing must not import this module')\n");
    write(external / "manifest.json", std::string(R"({"schema_version":1,"runtime":"python","plugin_id":"fixture.browser_external","plugin_version":"1.0.0","nodes":[{"type_id":"fixture.browser_external","version":"1.0.0","display_name":"External Browser Python","execution":{"kind":"block","stateful":false},"entrypoint":"never_import:Node","inputs":[{"id":"audio_in","type":"audio.pcm@1","required":true}],"outputs":[{"id":"audio_out","type":"audio.pcm@1"}],"parameters":[],"format":{"sample_rates_hz":[48000],"channels":[2]},"browser":)") + browser_json + "}]}");
    immutable.emplace_back(external / "manifest.json", bytes(external / "manifest.json"));
    immutable.emplace_back(external / "never_import.py", bytes(external / "never_import.py"));
    const auto discovery = load_native_plugins(*catalog, {external});
    REQUIRE(discovery.diagnostics.empty()); REQUIRE(discovery.loaded_plugin_ids == std::vector<std::string>{"fixture.browser_external"});
    types.push_back("fixture.browser_external"); CHECK_FALSE(std::filesystem::exists(external_marker));
    editor::EditorController controller(*catalog);
    ProjectModel project; project.project_id = "browser-selection-stays";
    project.nodes = {{"gain", "builtin.gain", {{"gain_db", 3.0}}, -50, 100}};
    project.selected_output_node_id = "gain"; project.selected_output_port_id = "audio_out";
    controller.adopt_project(project);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root / "before.aaproj", controller.project())));
    int clicks = 0;
    editor::NodeLibrary library(*catalog, [&](auto) { ++clicks; }, [] { return std::uint64_t{1}; });
    library.setSize(400, 700); library.setVisible(true);
    const auto entries = editor::build_node_library_entries(*catalog);
    for (const auto& type : types) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.type_id == type; });
        REQUIRE(found != entries.end());
        CHECK(found->browser_classification.category_id == "audio.dynamics");
        CHECK(found->browser_classification.subcategory_id == std::optional<std::string>{"gain"});
        CHECK(found->browser_tags == std::vector<std::string>{"route-marker", "增益"});
        CHECK(found->language_id == "python");
        CHECK(found->source_id == (type.starts_with("user.") ? "custom" : "third_party"));
    }
    auto* search = child<juce::TextEditor>(library, "library.search"); REQUIRE(search);
    auto* source = child<juce::ComboBox>(library, "library.filter.source"); REQUIRE(source);
    auto* language = child<juce::ComboBox>(library, "library.filter.language"); REQUIRE(language);
    for (int filter = 1; filter <= 4; ++filter) {
        source->setSelectedId(filter, juce::sendNotificationSync);
        language->setSelectedId(3, juce::sendNotificationSync);
        search->setText("route-marker", true);
        search->onTextChange();
        auto* heading = child<editor::NodeLibraryHeading>(library, "library.subcategory.audio.dynamics.gain"); REQUIRE(heading);
        CHECK(heading->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        library.refresh(*catalog);
        int visible = 0;
        for (const auto& type : types) {
            auto* button = child<editor::NodeLibraryButton>(library, "library." + juce::String(type)); REQUIRE(button);
            if (button->isVisible()) ++visible;
        }
        CHECK(visible == std::array{3, 0, 2, 1}[static_cast<std::size_t>(filter - 1)]);
        for (const auto& [path, content] : markers) CHECK(bytes(path) == content);
        CHECK_FALSE(std::filesystem::exists(external_marker));
    }
    CHECK(clicks == 0);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root / "after.aaproj", controller.project())));
    CHECK(bytes(root / "before.aaproj") == bytes(root / "after.aaproj"));
    for (const auto& [path, content] : immutable) CHECK(bytes(path) == content);
    write(root / "routes.json", juce::JSON::toString(receipts, false).toStdString());
}

TEST_CASE("NLC-08.a browser only edits revoke Ready and publish new identity without changing frozen versions",
          "[NLC-08.a][node_authoring][node_library][integration]") {
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc08a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout << "NLC08A_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    juce::Array<juce::var> receipts;
    for (const bool directory : {false, true}) {
        const std::string route = directory ? "directory" : "single";
        const auto folder = root / route; std::filesystem::create_directories(folder);
        auto draft = python_draft("nlc08a-" + route);
        auto declaration = juce::JSON::parse(juce::String::fromUTF8(draft.declaration.c_str()));
        declaration.getDynamicObject()->setProperty("type_id", "user.identity_" + juce::String(route));
        declaration.getDynamicObject()->setProperty("browser", juce::JSON::parse(
            R"({"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["before"],"description":"version one"})"));
        draft.declaration = juce::JSON::toString(declaration, false).toStdString();
        auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
        auto service = std::make_shared<PythonAuthoring>(folder / "library");
        editor::DraftFileActions actions;
        if (directory) {
            const auto created = WorkspaceStore{}.create(folder / "author", draft);
            REQUIRE(std::holds_alternative<Workspace>(created)); const auto& disk = std::get<Workspace>(created);
            actions.workspace = editor::DraftFileActions::WorkspaceBinding{disk.directory, disk.content_sha256, disk.files.size(), 0, disk};
            draft = workspace_identity(disk);
        }
        std::vector<AppliedPackage> applied;
        editor::NodeAuthoringPanel panel(draft, true, std::make_shared<FileDraftStorage>(folder / "drafts"), [] {}, service,
            [&] { return registry; }, [&](auto installed) { registry = std::make_shared<NodeRegistry>(*installed.catalog); applied.push_back(std::move(installed)); }, {}, {}, actions);
        panel.setSize(1100, 700);
        const auto check = [&] {
            activate(panel, "authoring.check");
            REQUIRE(pump([&] { return !panel.check_in_progress() && panel.ready().has_value(); }));
            REQUIRE(child<juce::TextButton>(panel, "authoring.apply")->isEnabled());
            return *panel.ready();
        };
        const auto ready_v1 = check(); activate(panel, "authoring.apply");
        REQUIRE(pump([&] { return applied.size() == 1 && !panel.apply_in_progress(); }));
        const auto installed_v1 = applied.front();
        const auto index_before = bytes(folder / "library/index.json");
        std::vector<std::pair<std::filesystem::path, std::string>> frozen;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(io_path(installed_v1.package_root)))
            if (entry.is_regular_file()) frozen.emplace_back(entry.path(), bytes(entry.path()));
        REQUIRE_FALSE(frozen.empty());
        ProjectModel project; project.schema_version = 2; project.project_id = "frozen-" + route;
        NodeInstance node; node.id = "old"; node.type_id = ready_v1.descriptor.type_id;
        node.parameters = {{"gain", 0.5}};
        node.managed_version = NodeInstance::ManagedVersion{installed_v1.package_id, installed_v1.content_sha256,
            "node_packages/" + installed_v1.package_id + "/" + installed_v1.content_sha256, ready_v1.wrapper_version};
        project.nodes.push_back(node); project.selected_output_node_id = "old"; project.selected_output_port_id = "audio_out";
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(folder / "v1.aaproj", project)));
        const auto graph_bytes = bytes(folder / "v1.aaproj");
        const auto ready_before_edit = check();
        CHECK(ready_before_edit.package_sha256 == ready_v1.package_sha256);
        std::string old_node_json;
        if (directory) {
            old_node_json = bytes(folder / "author/node.json");
            auto document = juce::JSON::parse(juce::String::fromUTF8(old_node_json.c_str()));
            auto definition = document["node"];
            definition.getDynamicObject()->setProperty("browser", juce::JSON::parse(
                R"({"schema_version":1,"category_id":"utility","subcategory_id":"inspection","tags":["after"],"description":"version two"})"));
            write(folder / "author/node.json", juce::JSON::toString(document, false).toStdString());
            activate(panel, "authoring.workspace_refresh");
            REQUIRE(pump([&] { return !panel.ready().has_value() && panel.workspace_content_sha256() != ready_before_edit.workspace->content_sha256; }));
            CHECK(bytes(folder / "author/node.json") != old_node_json);
        } else {
            declaration.getDynamicObject()->setProperty("browser", juce::JSON::parse(
                R"({"schema_version":1,"category_id":"utility","subcategory_id":"inspection","tags":["after"],"description":"version two"})"));
            auto* editor = child<juce::CodeEditorComponent>(panel, "authoring.definition"); REQUIRE(editor);
            editor->selectAll(); editor->insertTextAtCaret(juce::JSON::toString(declaration, false));
        }
        CHECK_FALSE(panel.ready().has_value());
        CHECK_FALSE(child<juce::TextButton>(panel, "authoring.apply")->isEnabled());
        const auto current = panel.session().document();
        auto stale = std::async(std::launch::async, [&] { return service->apply(ready_before_edit, current, registry, std::make_shared<std::atomic<bool>>(false)); });
        const auto stale_result = stale.get(); REQUIRE(std::holds_alternative<SourceDiagnostic>(stale_result));
        CHECK(bytes(folder / "library/index.json") == index_before);
        const auto ready_v2 = check();
        CHECK(ready_v2.package_sha256 != ready_v1.package_sha256);
        CHECK(ready_v2.declaration_sha256 != ready_v1.declaration_sha256);
        CHECK(ready_v2.interpreter_sha256 == ready_v1.interpreter_sha256);
        CHECK(ready_v2.runner_sha256 == ready_v1.runner_sha256);
        if (directory) {
            CHECK(ready_v2.workspace->content_sha256 != ready_v1.workspace->content_sha256);
            CHECK(ready_v2.source_sha256 != ready_v1.source_sha256); // Directory source identity includes node.json.
            CHECK(ready_v2.workspace->entry_source == ready_v1.workspace->entry_source);
        } else {
            CHECK(ready_v2.source_sha256 == ready_v1.source_sha256);
            CHECK(ready_v2.draft.source == ready_v1.draft.source);
        }
        auto a = juce::JSON::parse(juce::String::fromUTF8(ready_v1.draft.declaration.c_str()));
        auto b = juce::JSON::parse(juce::String::fromUTF8(ready_v2.draft.declaration.c_str()));
        a.getDynamicObject()->removeProperty("browser"); b.getDynamicObject()->removeProperty("browser");
        CHECK(canonical(a) == canonical(b));
        activate(panel, "authoring.apply"); REQUIRE(pump([&] { return applied.size() == 2 && !panel.apply_in_progress(); }));
        CHECK(applied.back().package_id == installed_v1.package_id);
        CHECK(applied.back().content_sha256 == ready_v2.package_sha256);
        CHECK(applied.back().package_root != installed_v1.package_root);
        const auto restored = service->restore_defaults(NodeRegistry{}); REQUIRE(std::holds_alternative<NodeRegistry>(restored));
        const auto* latest = std::get<NodeRegistry>(restored).find(ready_v1.descriptor.type_id); REQUIRE(latest); REQUIRE(latest->plugin_origin);
        CHECK(latest->plugin_origin->managed_content_sha256 == ready_v2.package_sha256);
        CHECK(latest->browser_classification->category_id == "utility");
        for (const auto& [path, content] : frozen) CHECK(bytes(path) == content);
        CHECK(bytes(folder / "v1.aaproj") == graph_bytes);
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
        receipt->setProperty("route", juce::String(route)); receipt->setProperty("v1", juce::String(installed_v1.content_sha256));
        receipt->setProperty("v2", juce::String(applied.back().content_sha256));
        receipt->setProperty("v1_package", juce::String(installed_v1.package_root.generic_string()));
        receipt->setProperty("v2_package", juce::String(applied.back().package_root.generic_string()));
        receipt->setProperty("stale_diagnostic", juce::String(std::get<SourceDiagnostic>(stale_result).code));
        receipt->setProperty("source_sha256", juce::String(ready_v1.source_sha256));
        receipts.add(juce::var(receipt.get()));
    }
    write(root / "identities.json", juce::JSON::toString(receipts, false).toStdString());
}

TEST_CASE("NLC-08.b classified v2 default and reopened v1 execute exact packages in component and CLI",
          "[NLC-08.b][node_authoring][node_library][integration]") {
    using namespace audioalgo;
    using namespace audioalgo::authoring;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nlc08b/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    std::cout << "NLC08B_ARTIFACT_ROOT=" << root.generic_string() << '\n';
    auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
    auto checks = std::make_shared<PythonAuthoring>(root / "library");
    auto draft = python_draft("nlc08b-versioned");
    auto declaration = juce::JSON::parse(juce::String::fromUTF8(draft.declaration.c_str()));
    declaration.getDynamicObject()->setProperty("type_id", "user.browser_versioned");
    const auto source_v1 = draft.source;
    AudioBuffer input(48000, {"ch0", "ch1"}, 513);
    for (std::size_t c = 0; c < 2; ++c) for (std::size_t n = 0; n < 513; ++n)
        input.mutable_channel(c)[n] = static_cast<float>(.13 * std::sin(.031 * (n + 1) + c) + (n == 177 ? .21 : -.03));
    REQUIRE(std::holds_alternative<WavWriteSummary>(write_wav_file(root / "input.wav", input, WavEncoding::float32)));
    std::vector<AppliedPackage> versions;
    std::string saved_v1;
    std::vector<std::pair<std::filesystem::path, std::string>> old_files;
    for (int version = 1; version <= 2; ++version) {
        declaration.getDynamicObject()->setProperty("browser", juce::JSON::parse(version == 1
            ? R"({"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["old"],"description":"old gain"})"
            : R"({"schema_version":1,"category_id":"utility","subcategory_id":"inspection","tags":["new"],"description":"new classified version"})"));
        draft.declaration = juce::JSON::toString(declaration, false).toStdString();
        draft.source = version == 1 ? source_v1 : "def process(inputs, params, state):\n    return {'audio_out': inputs['audio_in'] * params['gain'] * 2}\n";
        draft.generation = static_cast<std::uint64_t>(version);
        auto task = std::async(std::launch::async, [&] {
            auto checked = checks->check(draft, std::make_shared<std::atomic<bool>>(false));
            if (const auto* error = std::get_if<SourceDiagnostic>(&checked)) return ApplyOutcome{*error};
            return checks->apply(std::get<ReadyPackage>(checked), draft, registry, std::make_shared<std::atomic<bool>>(false));
        });
        const auto outcome = task.get();
        if (const auto* error = std::get_if<SourceDiagnostic>(&outcome)) INFO(error->code + ": " + error->message);
        REQUIRE(std::holds_alternative<AppliedPackage>(outcome)); versions.push_back(std::get<AppliedPackage>(outcome));
        registry = std::make_shared<NodeRegistry>(*versions.back().catalog);
        // Production add-node stamps the currently applied exact version lock.
        editor::EditorController controller(*registry);
        ProjectModel project; project.schema_version = 2; project.project_id = "browser-locked-v" + std::to_string(version);
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 0, 0}, {"output", "builtin.output", {}, 600, 0}};
        controller.adopt_project(project);
        REQUIRE(controller.add_node("user.browser_versioned", "custom", 300, 0).status == editor::ProjectEditStatus::applied);
        REQUIRE(controller.connect({"file", "audio", "custom", "audio_in"}).status == editor::ProjectEditStatus::applied);
        REQUIRE(controller.connect({"custom", "audio_out", "output", "audio"}).status == editor::ProjectEditStatus::applied);
        REQUIRE(controller.set_selected_output_port("output", "").status == editor::ProjectEditStatus::applied);
        const auto graph = root / ("v" + std::to_string(version) + ".aaproj");
        REQUIRE(std::holds_alternative<std::monostate>(save_project_file(graph, controller.project())));
        REQUIRE_FALSE(checks->carry_packages(managed_selections(controller.project()), {}, root));
        const auto custom = controller.node_snapshot("custom"); REQUIRE(custom); REQUIRE(custom->node.managed_version);
        CHECK(custom->node.managed_version->content_sha256 == versions.back().content_sha256);
        if (version == 1) {
            saved_v1 = bytes(graph);
            for (const auto& entry : std::filesystem::recursive_directory_iterator(io_path(versions.front().package_root)))
                if (entry.is_regular_file()) old_files.emplace_back(entry.path(), bytes(entry.path()));
        } else {
            CHECK(bytes(root / "v1.aaproj") == saved_v1);
            CHECK(versions[0].package_id == versions[1].package_id);
            CHECK(versions[0].content_sha256 != versions[1].content_sha256);
        }
    }
    const auto entries = editor::build_node_library_entries(*registry);
    const auto latest = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.type_id == "user.browser_versioned"; });
    REQUIRE(latest != entries.end()); CHECK(latest->browser_classification.category_id == "utility");
    CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& e) { return e.type_id == "user.browser_versioned"; }) == 1);
    editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<ProjectResourceService>(),
        std::make_shared<Preview>(), std::make_shared<FileDraftStorage>(root / "drafts"), false, checks, {}, false, {}, {}, {}, editor::RunConfiguration{127});
    workspace.setSize(1100, 700);
    std::set<std::filesystem::path> seen;
    juce::Array<juce::var> runs;
    const auto verify = [&](const std::filesystem::path& directory, int version, const char* origin) {
        auto loaded = read_wav_file(directory / "output.wav"); REQUIRE(std::holds_alternative<AudioBuffer>(loaded));
        const auto& actual = std::get<AudioBuffer>(loaded);
        REQUIRE(actual.frame_count() == 513); REQUIRE(actual.channel_count() == 2); CHECK(actual.sample_rate_hz() == 48000);
        for (std::size_t c = 0; c < 2; ++c) for (std::size_t n = 0; n < 513; ++n) {
            const double expected = input.channel(c)[n] * (version == 1 ? .5 : 1.0);
            CHECK(std::abs(actual.channel(c)[n] - expected) <= 2e-6 + 1e-5 * std::abs(expected));
        }
        auto report = juce::JSON::parse(juce::String::fromUTF8(bytes(directory / "run.json").c_str()));
        CHECK(static_cast<int>(report["block_size"]) == 127);
        CHECK(report["project_id"].toString() == "browser-locked-v" + juce::String(version));
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
        receipt->setProperty("version", version); receipt->setProperty("origin", origin);
        receipt->setProperty("directory", juce::String(directory.generic_string())); runs.add(juce::var(receipt.get()));
    };
    // First use the new graph, then reopen the old graph after v2 is already default.
    for (int version : {2, 1}) {
        const auto graph = root / ("v" + std::to_string(version) + ".aaproj");
        workspace.open_project_file(graph);
        REQUIRE(pump([&] { return workspace.controller().project().project_id == "browser-locked-v" + std::to_string(version); }));
        const auto custom = workspace.controller().node_snapshot("custom"); REQUIRE(custom); REQUIRE(custom->node.managed_version);
        CHECK(custom->node.managed_version->content_sha256 == versions[static_cast<std::size_t>(version - 1)].content_sha256);
        activate(workspace, "project.run");
        auto* status = child<editor::RunStatusPanel>(workspace, "project.status"); REQUIRE(status);
        REQUIRE(pump([&] { return status->status_text().contains("Completed run"); }));
        std::filesystem::path fresh;
        for (const auto& entry : std::filesystem::directory_iterator(root / "runs"))
            if (std::filesystem::is_regular_file(entry.path() / "run.json") && !seen.contains(entry.path())) { REQUIRE(fresh.empty()); fresh = entry.path(); }
        REQUIRE_FALSE(fresh.empty()); seen.insert(fresh); verify(fresh, version, "gui");
        const auto destination = root / ("cli-v" + std::to_string(version));
        const auto cli = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
        auto running = std::async(std::launch::async, [&] {
            juce::ChildProcess process;
            if (!process.start(juce::StringArray{cli.getFullPathName(), "run", juce::String(graph.generic_string()), juce::String(destination.generic_string()), "127"})) return -1;
            auto reader = std::async(std::launch::async, [&] { return process.readAllProcessOutput(); });
            const bool done = process.waitForProcessToFinish(15000); if (!done) process.kill();
            std::ofstream log(io_path(root / ("cli-v" + std::to_string(version) + ".log")), std::ios::binary);
            log << reader.get().toStdString(); if (!log.good()) return -3;
            return done ? static_cast<int>(process.getExitCode()) : -2;
        });
        REQUIRE(pump([&] { return running.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; })); REQUIRE(running.get() == 0);
        verify(destination, version, "cli");
    }
    CHECK(bytes(root / "v1.aaproj") == saved_v1);
    for (const auto& [path, content] : old_files) CHECK(bytes(path) == content);
    write(root / "runs.json", juce::JSON::toString(runs, false).toStdString());
    int drained = 0; workspace.begin_shutdown([&] { ++drained; }); REQUIRE(pump([&] { return drained == 1; }));
}

TEST_CASE("NRG-01.a production library recreates a deleted Output exactly once and history restores the original",
          "[NRG-01.a][node_library][component]") {
    using namespace audioalgo;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nrg01a/artifacts" / juce::Uuid().toString().toStdString();
    std::filesystem::create_directories(root);
    auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
    editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<ProjectResourceService>(),
        std::make_shared<Preview>(), std::make_shared<authoring::FileDraftStorage>(root / "drafts"), false);
    workspace.setSize(1100, 700);
    ProjectModel project; project.project_id = "recreate-output"; project.selected_output_node_id = "output";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0, 0},
                     {"gain", "builtin.gain", {{"gain_db", 6.0}}, 250, 0}, {"output", "builtin.output", {}, 500, 0}};
    project.connections = {{"file", "audio", "gain", "audio_in"}, {"gain", "audio_out", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root / "graph.aaproj", project)));
    workspace.open_project_file(root / "graph.aaproj");
    REQUIRE(pump([&] { return workspace.controller().project().project_id == project.project_id; }));
    auto* canvas = child<editor::GraphCanvas>(workspace, "project.canvas"); REQUIRE(canvas);
    canvas->select("output"); REQUIRE(canvas->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(workspace.controller().project().nodes.size() == 2);
    CHECK(workspace.controller().project().selected_output_node_id.empty());
    const auto removed_revision = workspace.controller().revision();
    activate(workspace, "library.builtin.output");
    REQUIRE(workspace.controller().project().nodes.size() == 3);
    CHECK(workspace.controller().revision() == removed_revision + 1);
    const auto count = [&] { return std::count_if(workspace.controller().project().nodes.begin(), workspace.controller().project().nodes.end(),
        [](const auto& node) { return node.type_id == "builtin.output"; }); };
    CHECK(count() == 1);
    const auto restored_revision = workspace.controller().revision();
    activate(workspace, "library.builtin.output");
    CHECK(count() == 1); CHECK(workspace.controller().revision() == restored_revision);
    REQUIRE(workspace.keyPressed(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0))); CHECK(count() == 0);
    REQUIRE(workspace.keyPressed(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0))); CHECK(count() == 1);
    const auto original = workspace.controller().node_snapshot("output"); REQUIRE(original);
    CHECK(original->node.x == 500); CHECK(original->node.y == 0);
    auto restored = workspace.controller().project();
    CHECK(restored.revision == project.revision + 4);
    restored.revision = project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root / "restored.aaproj", restored)));
    CHECK(bytes(root / "restored.aaproj") == bytes(root / "graph.aaproj"));
    CHECK(workspace.controller().project().selected_output_node_id == "output");
}

TEST_CASE("NRG-02.b compact production library keeps authoring actions and recovers a saved draft",
          "[NRG-02.b][node_library][component]") {
    using namespace audioalgo;
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = std::filesystem::path(AUDIOALGO_SOURCE_DIR) /
        "build/reports/loops/CNL0001/nrg02b/artifacts" / juce::Uuid().toString().toStdString();
    auto storage = std::make_shared<authoring::FileDraftStorage>(root / "drafts");
    const auto draft = authoring::python_draft("recover-entry"); REQUIRE_FALSE(storage->save(draft));
    const auto saved_bytes = bytes(root / "drafts/recover-entry.aadraft");
    auto registry = std::make_shared<NodeRegistry>(); REQUIRE(register_builtin_nodes(*registry));
    editor::EditorComponent workspace(registry, std::make_shared<FileDocuments>(), std::make_shared<ProjectResourceService>(),
        std::make_shared<Preview>(), storage, false);
    const auto revision = workspace.controller().revision();
    for (const auto size : {juce::Point<int>{1100, 700}, juce::Point<int>{1440, 900}}) {
        workspace.setSize(size.x, size.y);
        std::vector<juce::Rectangle<int>> bounds;
        for (const auto* id : {"authoring.new", "authoring.open", "authoring.open_workspace", "authoring.recover", "authoring.upgrade"}) {
            auto* button = child<juce::TextButton>(workspace, id); REQUIRE(button);
            CHECK(button->isVisible()); CHECK(button->isEnabled()); CHECK(button->getWidth() > 0); CHECK(button->getHeight() >= 28);
            REQUIRE(button->getParentComponent()); CHECK(button->getParentComponent()->getLocalBounds().contains(button->getBounds()));
            for (const auto& other : bounds) CHECK_FALSE(other.intersects(button->getBounds()));
            bounds.push_back(button->getBounds());
        }
    }
    activate(workspace, "authoring.recover");
    REQUIRE(pump([&] { return child<editor::NodeAuthoringPanel>(workspace, "authoring.panel") != nullptr; }));
    auto* recovered = child<editor::NodeAuthoringPanel>(workspace, "authoring.panel"); REQUIRE(recovered);
    CHECK(recovered->session().document() == draft);
    CHECK(child<juce::CodeEditorComponent>(*recovered, "authoring.source")->getDocument().getAllContent().toStdString() == draft.source);
    CHECK(bytes(root / "drafts/recover-entry.aadraft") == saved_bytes);
    CHECK(workspace.controller().revision() == revision); CHECK_FALSE(workspace.controller().dirty());
    activate(workspace, "authoring.close"); REQUIRE(pump([&] { return child<editor::NodeAuthoringPanel>(workspace, "authoring.panel") == nullptr; }));
    int drained = 0; workspace.begin_shutdown([&] { ++drained; }); REQUIRE(pump([&] { return drained == 1; }));
}
