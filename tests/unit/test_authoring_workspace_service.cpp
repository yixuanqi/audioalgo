// Pure service checks for the frozen node workspace contract (ADR0040).
// This target links no JUCE, so the workspace service must stay a pure library
// contract. The independent fixture lock is verified separately by
// tests/unit/test_node_workspace_lock.cpp, which compares real fixture bytes.
#include "audio_node_authoring/Workspace.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace audioalgo::authoring;
using audioalgo::Diagnostic;

fs::path root() {
    static std::atomic<std::uint64_t> serial{0};
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                       std::to_string(++serial);
    const auto directory = fs::path(AUDIOALGO_SOURCE_DIR) /
                           "build/reports/loops/NWS0001-workspace-create/service" / stamp;
    std::filesystem::create_directories(directory);
    return directory;
}
void require_workspace(const WorkspaceOutcome& outcome) {
    if (const auto* error = std::get_if<Diagnostic>(&outcome)) FAIL(error->code + ": " + error->message);
    REQUIRE(std::holds_alternative<Workspace>(outcome));
}
fs::path fixtures() { return fs::path(AUDIOALGO_SOURCE_DIR) / "tests/fixtures/node_workspace"; }
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}
Draft python_draft_for(std::string id) {
    Draft draft;
    draft.id = std::move(id);
    draft.language = "python";
    draft.declaration = R"({
  "type_id": "user.print_a", "version": "1.0.0", "display_name": "Print A",
  "inputs": [{"id": "primary", "type": "audio.pcm@1", "required": true},
             {"id": "reference", "type": "audio.pcm@1", "required": true}],
  "outputs": [{"id": "main", "type": "audio.pcm@1"}],
  "format": {"sample_rates_hz": [16000, 48000], "channels": [1, 2, 4]},
  "parameters": [{"id": "gain", "type": "float", "default": 0.5, "minimum": -2, "maximum": 2}]
})";
    draft.source = "def process(inputs, params, state):\n    return {'main': inputs['primary'] * 0.5}\n";
    return draft;
}
#ifdef _WIN32
void junction(const fs::path& link, const fs::path& destination) {
    fs::create_directory(link);
    const auto target = fs::absolute(destination).wstring(), substitute = L"\\??\\" + target;
    struct Data {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD length = 0, reserved = 0, substitute_offset = 0, substitute_length = 0, print_offset = 0, print_length = 0;
        wchar_t path[8192]{};
    } data;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = data.substitute_length + sizeof(wchar_t);
    data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.length = 8 + data.print_offset + data.print_length + sizeof(wchar_t);
    std::copy(substitute.begin(), substitute.end(), data.path);
    std::copy(target.begin(), target.end(), data.path + data.print_offset / sizeof(wchar_t));
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8 + data.length, nullptr, 0, &returned, nullptr);
    const auto error = GetLastError(); CloseHandle(handle);
    INFO(error); REQUIRE(success != 0);
}
#endif
}

TEST_CASE("NWS-00.a workspace sha256 and module names follow the frozen contract", "[NWS-00.a][NWS-01.a][node_workspace][pure]") {
    CHECK(WorkspaceStore::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(WorkspaceStore::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(WorkspaceStore::sha256_hex("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    CHECK(workspace_module_name("print-a") == "print_a");
    CHECK(workspace_module_name("PrintA") == "printa");
    CHECK(workspace_module_name("7lives") == "node_7lives");
}

TEST_CASE("NWS-02.a create refuses a Windows junction target or ancestor before writing any file",
          "[NWS-02.a][workspace_create_junction][node_workspace][pure]") {
#ifdef _WIN32
    const auto directory = root(), destination = directory / "foreign", link = directory / "junction";
    fs::create_directory(destination);
    junction(link, destination);
    struct Cleanup { fs::path link; ~Cleanup() { std::error_code error; fs::remove(link,error); } } cleanup{link};
    for (const auto& target : {link, link / "project"}) {
        INFO(target.string());
        int writes = 0;
        const auto outcome = WorkspaceStore([&](WorkspacePhase phase) { if (phase == WorkspacePhase::write) ++writes; return true; })
            .create(target, python_draft_for("junction-target"));
        REQUIRE(std::holds_alternative<Diagnostic>(outcome));
        CHECK(std::get<Diagnostic>(outcome).code == "workspace_path_invalid");
        CHECK(writes == 0);
        CHECK(fs::is_empty(destination));
    }
#endif
}

TEST_CASE("NWS-02.c swapping a source folder to a junction during capture rejects all outside bytes and permits retry",
          "[NWS-02.c][workspace_copy_junction][node_workspace][pure]") {
#ifdef _WIN32
    const auto directory = root(), author = directory / "author", target = directory / "frozen";
    require_workspace(WorkspaceStore{}.create(author, python_draft_for("swap-source")));
    const auto original = std::get<Workspace>(WorkspaceStore{}.open(author));
    const auto module = author / "src/swap_source", parked = directory / "parked", foreign = directory / "foreign";
    write(foreign / "__init__.py", "# outside marker\n");
    write(foreign / "process.py", "raise RuntimeError('outside source must never be read')\n");
    bool swapped = false;
    struct Restore {
        fs::path module, parked; bool& swapped;
        void restore() { if (swapped) { fs::remove(module); fs::rename(parked,module); swapped=false; } }
        ~Restore() { if(swapped) { std::error_code error; fs::remove(module,error); fs::rename(parked,module,error); } }
    } restore{module,parked,swapped};
    const auto failed = WorkspaceStore{}.snapshot(author,target,{},[&](const std::string& path) {
        if(path != "node.json")return;
        fs::rename(module,parked); swapped=true; junction(module,foreign);
    });
    REQUIRE(std::holds_alternative<Diagnostic>(failed));
    CHECK(std::get<Diagnostic>(failed).code == "workspace_path_invalid");
    CHECK_FALSE(fs::exists(target));
    CHECK(bytes(foreign / "__init__.py") == "# outside marker\n");
    CHECK(bytes(foreign / "process.py") == "raise RuntimeError('outside source must never be read')\n");
    restore.restore();
    const auto retry = WorkspaceStore{}.snapshot(author,target);
    require_workspace(retry);
    CHECK(std::get<Workspace>(retry).files == original.files);
    CHECK(std::get<Workspace>(retry).content_sha256 == original.content_sha256);
#endif
}

TEST_CASE("NWS-00.a canonical JSON preserves number tokens key order and escapes", "[NWS-00.a][node_workspace][pure]") {
    std::string error;
    const auto canonical = WorkspaceStore::canonical_json(
        "{\n  \"b\": 0.5000, \"a\": [1e-5, -2, true, null], \"s\": \"line\\nbreak\", \"u\": \"\\u4e2d\"\n}", error);
    CHECK(error.empty());
    CHECK(canonical == "{\"b\":0.5000,\"a\":[1e-5,-2,true,null],\"s\":\"line\\nbreak\",\"u\":\"中\"}");
    CHECK(WorkspaceStore::canonical_json("{\"x\":1,\"x\":2}", error).empty());
    CHECK(error.find("workspace_json_duplicate_key") != std::string::npos);
    CHECK(WorkspaceStore::canonical_json("{\"x\":1,}", error).empty());
    CHECK(error.find("workspace_json_invalid") != std::string::npos);
}

TEST_CASE("NWS-01.a workspace create writes the declaration and the entry source then reopens them",
          "[NWS-01.a][NWS-01.c][node_workspace][pure]") {
    const auto directory = root() / std::filesystem::u8path("目录 Print A");
    const auto draft = python_draft_for("print-a");
    const WorkspaceStore store;
    const auto created = store.create(directory, draft);
    require_workspace(created);
    const auto& workspace = std::get<Workspace>(created);
    CHECK(workspace.language == "python");
    CHECK(workspace.entrypoint == "print_a.process:process");
    CHECK(workspace.entry_source == draft.source);
    CHECK(workspace.directory == fs::absolute(directory).lexically_normal());
    REQUIRE(workspace.files.size() == 3);
    CHECK(workspace.files[0].path == "node.json");
    CHECK(workspace.files[1].path == "src/print_a/__init__.py");
    CHECK(workspace.files[1].bytes == 0);
    CHECK(workspace.files[2].path == "src/print_a/process.py");
    CHECK(workspace.files[2].bytes == draft.source.size());
    CHECK(bytes(directory / "src/print_a/process.py") == draft.source);
    CHECK(bytes(directory / "node.json").find("\"workspace_schema\":1") != std::string::npos);
    std::string error;
    CHECK(WorkspaceStore::canonical_json(draft.declaration, error) == workspace.declaration);
    // Reopening reads the same bytes back and recomputes the same identity.
    const auto reopened = store.open(directory);
    require_workspace(reopened);
    CHECK(std::get<Workspace>(reopened) == workspace);
}

TEST_CASE("NWS-01.a workspace create refuses a non-empty target and keeps its bytes",
          "[NWS-01.a][NWS-02.b][node_workspace][pure]") {
    const auto directory = root() / "occupied";
    write(directory / "keep.txt", "user bytes");
    const auto created = WorkspaceStore{}.create(directory, python_draft_for("blocked"));
    REQUIRE(std::holds_alternative<Diagnostic>(created));
    CHECK(std::get<Diagnostic>(created).code == "workspace_target_not_empty");
    CHECK(bytes(directory / "keep.txt") == "user bytes");
    CHECK_FALSE(fs::exists(directory / "node.json"));
    // A second create on the same directory stays refused and writes nothing.
    const auto again = WorkspaceStore{}.create(directory, python_draft_for("blocked"));
    REQUIRE(std::holds_alternative<Diagnostic>(again));
    CHECK(std::get<Diagnostic>(again).code == "workspace_target_not_empty");
    CHECK_FALSE(fs::exists(directory / "src"));
}

TEST_CASE("NWS-01.a cpp workspace create writes one declared source", "[NWS-01.a][node_workspace][pure]") {
    const auto directory = root() / "native";
    auto draft = python_draft_for("native-node");
    draft.language = "cpp";
    draft.source = "// user source\nfloat process(float in) { return -2.0f * in; }\n";
    const auto created = WorkspaceStore{}.create(directory, draft);
    require_workspace(created);
    const auto& workspace = std::get<Workspace>(created);
    CHECK(workspace.language == "cpp");
    CHECK(workspace.files.size() == 2);
    CHECK(workspace.files[0].path == "node.json");
    CHECK(workspace.files[1].path == "src/node.cpp");
    CHECK(bytes(directory / "src/node.cpp") == draft.source);
    const auto opened = WorkspaceStore{}.open(directory);
    require_workspace(opened);
    CHECK(std::get<Workspace>(opened) == workspace);
}

TEST_CASE("NWS-01.c excluded author files never change the runtime content hash",
          "[NWS-01.c][NWS-02.a][node_workspace][pure]") {
    const auto directory = root() / "hash-rules";
    const auto draft = python_draft_for("hash-rules");
    require_workspace(WorkspaceStore{}.create(directory, draft));
    const auto baseline = std::get<Workspace>(WorkspaceStore{}.open(directory));
    write(directory / "tests/test_process.py", "def test_nothing():\n    assert True\n");
    write(directory / "build/cache.bin", "stale bytes");
    write(directory / ".vscode/settings.json", "{}");
    const auto after_excluded = WorkspaceStore{}.open(directory);
    require_workspace(after_excluded);
    CHECK(std::get<Workspace>(after_excluded).content_sha256 == baseline.content_sha256);
    CHECK(std::get<Workspace>(after_excluded).files == baseline.files);
    // Any runtime source change is part of the identity.
    write(directory / "src/print_a/filter.py", "COEFFICIENTS = (0.5, -0.25)\n");
    const auto after_source = WorkspaceStore{}.open(directory);
    require_workspace(after_source);
    CHECK(std::get<Workspace>(after_source).content_sha256 != baseline.content_sha256);
    CHECK(std::get<Workspace>(after_source).files.size() == baseline.files.size() + 1);
    // Changing the declaration also changes the identity.
    const auto node_json = bytes(directory / "node.json");
    write(directory / "node.json", node_json + "\n");
    const auto after_declaration = WorkspaceStore{}.open(directory);
    require_workspace(after_declaration);
    CHECK(std::get<Workspace>(after_declaration).content_sha256 != std::get<Workspace>(after_source).content_sha256);
}

TEST_CASE("NWS-01.a frozen invalid workspaces report the documented codes",
          "[NWS-01.a][NWS-02.a][node_workspace][pure]") {
    const auto base = root() / "invalid";
    const std::vector<std::pair<std::string, std::string>> cases{
        {"duplicate_key", "workspace_json_duplicate_key"},
        {"schema2", "workspace_schema_unsupported"},
        {"missing_entrypoint", "workspace_contract_invalid"},
        {"escape_entrypoint", "workspace_entrypoint_invalid"},
        {"absolute_source", "workspace_path_invalid"},
        {"case_conflict", "workspace_path_case_conflict"},
    };
    for (const auto& [name, expected] : cases) {
        INFO(name);
        const auto directory = base / name;
        write(directory / "node.json", bytes(fixtures() / "invalid" / (name + ".json")));
        const auto opened = WorkspaceStore{}.open(directory);
        REQUIRE(std::holds_alternative<Diagnostic>(opened));
        CHECK(std::get<Diagnostic>(opened).code == expected);
        CHECK(std::get<Diagnostic>(opened).message.size() > 0);
    }
    // Missing and empty directories keep their own diagnostics.
    const auto missing = WorkspaceStore{}.open(base / "does-not-exist");
    REQUIRE(std::holds_alternative<Diagnostic>(missing));
    CHECK(std::get<Diagnostic>(missing).code == "workspace_directory_missing");
    fs::create_directories(base / "empty");
    const auto empty = WorkspaceStore{}.open(base / "empty");
    REQUIRE(std::holds_alternative<Diagnostic>(empty));
    CHECK(std::get<Diagnostic>(empty).code == "workspace_node_json_missing");
}

TEST_CASE("NWS-01.c unknown declaration fields are preserved in the canonical declaration",
          "[NWS-01.c][NWS-01.a][node_workspace][pure]") {
    const auto source = root() / "preserve";
    fs::copy(fixtures() / "preserve_unknown", source, fs::copy_options::recursive);
    const auto opened = WorkspaceStore{}.open(source);
    require_workspace(opened);
    const auto& workspace = std::get<Workspace>(opened);
    CHECK(workspace.declaration.find("\"future_node_field\":{\"owner\":\"authoring-tools\",\"revision\":7}") !=
          std::string::npos);
    CHECK(workspace.entrypoint == "preserve.process:process");
    CHECK(workspace.files.size() == 2);
    CHECK(workspace.files[1].path == "src/preserve/process.py");
}

TEST_CASE("NWS-02.b a target replaced by a junction during creation never overwrites or rolls back foreign files",
          "[NWS0010][workspace_create_swap][NWS-02.a][NWS-02.b][node_workspace][pure]") {
#ifdef _WIN32
    for(const auto phase:{WorkspacePhase::write,WorkspacePhase::verify,WorkspacePhase::replace}) {
        INFO(static_cast<int>(phase));
        const auto base=root(), target=base/"author", parked=base/"parked", foreign=base/"foreign";
        write(foreign/"node.json","foreign declaration");
        write(foreign/"src/swap/__init__.py","foreign init");
        write(foreign/"src/swap/process.py","foreign source");
        int occurrences=0;bool swapped=false;
        const auto result=WorkspaceStore([&](WorkspacePhase observed) {
            if(observed==phase && ++occurrences==2) {fs::rename(target,parked);junction(target,foreign);swapped=true;}
            return true;
        }).create(target,python_draft_for("swap"));
        struct Cleanup {fs::path link;~Cleanup(){std::error_code error;fs::remove(link,error);}} cleanup{target};
        REQUIRE(swapped);REQUIRE(std::holds_alternative<Diagnostic>(result));
        CHECK(std::get<Diagnostic>(result).code=="workspace_path_invalid");
        CHECK(bytes(foreign/"node.json")=="foreign declaration");
        CHECK(bytes(foreign/"src/swap/__init__.py")=="foreign init");
        CHECK(bytes(foreign/"src/swap/process.py")=="foreign source");
        std::size_t files=0;for(const auto& item:fs::recursive_directory_iterator(foreign))if(item.is_regular_file())++files;
        CHECK(files==3);
        require_workspace(WorkspaceStore{}.create(base/"retry",python_draft_for("swap")));
        std::cout<<"NWS0010_ROOT="<<base.generic_string()<<'\n';
    }
#endif
}

TEST_CASE("NWS-01.b conversion from the frozen legacy draft keeps its bytes and reopens",
          "[NWS-01.b][node_workspace][pure]") {
    const auto legacy = fixtures() / "legacy/print_a.aadraft";
    const auto legacy_before = bytes(legacy);
    REQUIRE_FALSE(legacy_before.empty());
    const auto directory = root() / "converted";
    const auto converted = WorkspaceStore{}.convert(legacy, directory);
    require_workspace(converted);
    CHECK(bytes(legacy) == legacy_before);
    const auto& workspace = std::get<Workspace>(converted);
    CHECK(workspace.entrypoint == "print_a.process:process");
    CHECK(workspace.files.size() == 3);
    // The same declaration and source, read back through the product's own reader.
    const auto loaded = DraftFiles{}.load(legacy);
    REQUIRE(std::holds_alternative<DraftFile>(loaded));
    CHECK(workspace.entry_source == std::get<DraftFile>(loaded).document.source);
    std::string error;
    CHECK(workspace.declaration == WorkspaceStore::canonical_json(std::get<DraftFile>(loaded).document.declaration, error));
    CHECK(error.empty());
    const auto reopened = WorkspaceStore{}.open(directory);
    require_workspace(reopened);
    CHECK(std::get<Workspace>(reopened) == workspace);
    CHECK(bytes(legacy) == legacy_before);
    // Converting into an occupied directory fails and leaves both sides intact.
    const auto blocked = WorkspaceStore{}.convert(legacy, directory);
    REQUIRE(std::holds_alternative<Diagnostic>(blocked));
    CHECK(std::get<Diagnostic>(blocked).code == "workspace_target_not_empty");
    CHECK(bytes(legacy) == legacy_before);
    CHECK(std::get<Workspace>(WorkspaceStore{}.open(directory)) == workspace);
}

TEST_CASE("NWS-02.b injected create write verify and replace failures leave no partial workspace",
          "[NWS-02.b][NWS-01.a][node_workspace][pure]") {
    for (const auto phase : {WorkspacePhase::create, WorkspacePhase::write, WorkspacePhase::verify, WorkspacePhase::replace}) {
        INFO("phase " << static_cast<int>(phase));
        const auto directory = root() / ("fault-" + std::to_string(static_cast<int>(phase)));
        bool injected = false;
        const WorkspaceStore store([&](WorkspacePhase observed) {
            if (observed != phase || injected) return true;
            injected = true;
            return false;
        });
        const auto failed = store.create(directory, python_draft_for("fault-injection"));
        REQUIRE(std::holds_alternative<Diagnostic>(failed));
        CHECK(std::get<Diagnostic>(failed).code == "workspace_write_failed");
        CHECK(std::get<Diagnostic>(failed).message.find("Injected workspace failure") != std::string::npos);
        CHECK_FALSE(fs::exists(directory));
        const auto retry = WorkspaceStore{}.create(directory, python_draft_for("fault-injection"));
        require_workspace(retry);
        CHECK(std::get<Workspace>(retry).files.size() == 3);
    }
    // A pre-existing empty directory survives a failure and stays empty.
    const auto empty = root() / "pre-existing-empty";
    fs::create_directories(empty);
    const auto failed = WorkspaceStore([](WorkspacePhase phase) { return phase != WorkspacePhase::verify; })
                            .create(empty, python_draft_for("keep-empty"));
    REQUIRE(std::holds_alternative<Diagnostic>(failed));
    CHECK(std::get<Diagnostic>(failed).code == "workspace_write_failed");
    CHECK(fs::is_directory(empty));
    CHECK(fs::is_empty(empty));
    // A pre-existing user file is never touched by a refused create.
    const auto occupied = root() / "occupied";
    write(occupied / "user.txt", "keep");
    const auto refused = WorkspaceStore{}.create(occupied, python_draft_for("occupied"));
    REQUIRE(std::holds_alternative<Diagnostic>(refused));
    CHECK(std::get<Diagnostic>(refused).code == "workspace_target_not_empty");
    CHECK(bytes(occupied / "user.txt") == "keep");
}

TEST_CASE("NWS-02.a long workspace paths and hostile declared paths are rejected before writing",
          "[NWS-02.a][node_workspace][pure]") {
    // A real workspace root longer than 260 characters creates and reopens.
    auto deep = root();
    for (int index = 0; index < 12; ++index)
        deep /= "long-segment-" + std::to_string(index) + "-0123456789";
    REQUIRE(deep.native().size() > 260);
    const auto created = WorkspaceStore{}.create(deep, python_draft_for("deep-path"));
    require_workspace(created);
    const auto reopened = WorkspaceStore{}.open(deep);
    require_workspace(reopened);
    CHECK(std::get<Workspace>(reopened).content_sha256 == std::get<Workspace>(created).content_sha256);

    const auto directory = root() / "hostile";
    write(directory / "src/x/process.py", "def process(inputs, params, state):\n    return {}\n");
    write(directory / "data.bin", "x");
    const auto document = [](const std::string& resources) {
        return std::string(R"({"workspace_schema":1,"language":"python","node":{"type_id":"user.x"},)"
                           R"("python":{"source_root":"src","entrypoint":"x.process:process"},"resources":)") +
               resources + "}";
    };
    write(directory / "node.json", document(R"(["data.bin","data.bin"])"));
    const auto duplicate = WorkspaceStore{}.open(directory);
    REQUIRE(std::holds_alternative<Diagnostic>(duplicate));
    CHECK(std::get<Diagnostic>(duplicate).code == "workspace_path_invalid");
    write(directory / "node.json", document(R"(["data.bin:stream"])"));
    const auto stream = WorkspaceStore{}.open(directory);
    REQUIRE(std::holds_alternative<Diagnostic>(stream));
    CHECK(std::get<Diagnostic>(stream).code == "workspace_path_invalid");
    write(directory / "node.json", document(R"(["..\\outside.bin"])"));
    const auto traversal = WorkspaceStore{}.open(directory);
    REQUIRE(std::holds_alternative<Diagnostic>(traversal));
    CHECK(std::get<Diagnostic>(traversal).code == "workspace_path_invalid");
}

TEST_CASE("NWS-02.c snapshot rejects concurrent helper edits and stable retry freezes exact runtime bytes",
          "[NWS-02.c][node_workspace][pure]") {
    const auto base = root();
    const auto directory = base / "author";
    fs::copy(fixtures() / "python_two_files", directory, fs::copy_options::recursive);
    const auto helper = directory / "src/print_a/filter.py";
    const auto changed_helper = bytes(fixtures() / "python_two_files_v2/src/print_a/filter.py");
    const auto target = base / "snapshot";
    const auto failed = WorkspaceStore{}.snapshot(directory, target, {}, [&](const std::string& path) {
        if (path == "node.json") write(helper, changed_helper);
    });
    REQUIRE(std::holds_alternative<Diagnostic>(failed));
    CHECK(std::get<Diagnostic>(failed).code == "source_changed_during_snapshot");
    CHECK_FALSE(fs::exists(target));
    CHECK(bytes(helper) == changed_helper);
    const auto stable = WorkspaceStore{}.snapshot(directory, target);
    require_workspace(stable);
    const auto& frozen = std::get<Workspace>(stable);
    CHECK(frozen.content_sha256 == std::get<Workspace>(WorkspaceStore{}.open(directory)).content_sha256);
    for (const auto& file : frozen.files) CHECK(bytes(target / file.path) == bytes(directory / file.path));
    CHECK_FALSE(fs::exists(target / "tests"));
    CHECK_FALSE(fs::exists(target / ".vscode"));
    write(helper, "external later edit");
    CHECK(bytes(target / "src/print_a/filter.py") == changed_helper);
    const auto refused = WorkspaceStore{}.snapshot(directory, target);
    REQUIRE(std::holds_alternative<Diagnostic>(refused));
    CHECK(std::get<Diagnostic>(refused).code == "workspace_target_not_empty");
    CHECK(bytes(target / "src/print_a/filter.py") == changed_helper);
    const auto cancelled = WorkspaceStore{}.snapshot(directory, base / "cancelled", [] { return true; });
    REQUIRE(std::holds_alternative<Diagnostic>(cancelled));
    CHECK(std::get<Diagnostic>(cancelled).code == "run_cancelled");
    CHECK_FALSE(fs::exists(base / "cancelled"));
}

TEST_CASE("NWS-02.b late source failure removes every newly created ancestor and permits retry",
          "[NWS-02.b][node_workspace][pure]") {
    for (const bool existing : {false, true}) {
        for (const auto phase : {WorkspacePhase::write, WorkspacePhase::verify, WorkspacePhase::replace}) {
            const auto directory = root() / "late-source-failure";
            if (existing) fs::create_directory(directory);
            int occurrences = 0;
            const auto failed = WorkspaceStore([&](WorkspacePhase observed) {
                return observed != phase || ++occurrences != 2;
            }).create(directory, python_draft_for("nested-source"));
            REQUIRE(std::holds_alternative<Diagnostic>(failed));
            CHECK(std::get<Diagnostic>(failed).code == "workspace_write_failed");
            CHECK_FALSE(fs::exists(directory / "src"));
            CHECK_FALSE(fs::exists(directory / "node.json"));
            CHECK(fs::exists(directory) == existing);
            if (existing) CHECK(fs::is_empty(directory));
            require_workspace(WorkspaceStore{}.create(directory, python_draft_for("nested-source")));
        }
    }
}

TEST_CASE("NWS-01.b author identity survives snapshot and changes revoke an otherwise identical runtime tree",
          "[NWS-01.b][NWS-02.c][node_workspace][pure]") {
    const auto base = root(); const auto directory = base / "author";
    const auto draft = python_draft_for("original-author-id");
    const auto created = WorkspaceStore{}.create(directory,draft); require_workspace(created);
    const auto disk = std::get<Workspace>(created);
    CHECK(workspace_identity(disk).id == draft.id);
    CHECK(workspace_identity(disk).source.empty());
    CHECK(disk.files.size() == 3); // Identity is not runtime source/definition.
    const auto copied = WorkspaceStore{}.snapshot(directory,base/"frozen"); require_workspace(copied);
    CHECK(std::get<Workspace>(copied).content_sha256 == disk.content_sha256);
    CHECK(std::get<Workspace>(copied).authoring_metadata == disk.authoring_metadata);
    CHECK(workspace_identity(std::get<Workspace>(copied)).id == draft.id);
    const auto failed = WorkspaceStore{}.snapshot(directory,base/"changed",{},[&](const std::string& file){
        if(file=="node.json") write(directory/".audioalgo-authoring.json",R"({"schema_version":1,"draft_id":"another-author"})");
    });
    REQUIRE(std::holds_alternative<Diagnostic>(failed));
    CHECK(std::get<Diagnostic>(failed).code == "source_changed_during_snapshot");
    CHECK_FALSE(fs::exists(base/"changed"));
    const auto changed = WorkspaceStore{}.open(directory); require_workspace(changed);
    CHECK(std::get<Workspace>(changed).content_sha256 == disk.content_sha256);
    CHECK(workspace_identity(std::get<Workspace>(changed)).id != draft.id);
    write(directory/".audioalgo-authoring.json",R"({"schema_version":1,"draft_id":"../escape"})");
    const auto invalid = WorkspaceStore{}.open(directory);
    REQUIRE(std::holds_alternative<Diagnostic>(invalid)); CHECK(std::get<Diagnostic>(invalid).code == "workspace_identity_invalid");
}

TEST_CASE("NWS-05.b declaration save changes only node json and rejects stale or failed publication",
          "[NWS-05.b][NWS0015][node_workspace][pure]") {
    const auto directory = root() / "author";
    const auto created = WorkspaceStore{}.create(directory, python_draft_for("declaration-save")); require_workspace(created);
    const auto original = std::get<Workspace>(created);
    auto edited = original.declaration;
    const auto name = edited.find("Print A"); REQUIRE(name != std::string::npos);
    edited.replace(name, 7, "Saved definition");
    edited.insert(edited.size() - 1, R"(,"future_node_field":{"preserve":[1,"value"]})");
    const auto author_identity = bytes(directory / ".audioalgo-authoring.json");
    const auto source = bytes(directory / "src/declaration_save/process.py");
    const auto no_temporary_files = [&] {
        for (const auto& entry : fs::directory_iterator(directory))
            CHECK(entry.path().filename().string().find(".tmp-") == std::string::npos);
    };
    SECTION("a valid definition is saved with unchanged author source and control sections") {
        const auto saved = WorkspaceStore{}.save_declaration(original, edited); require_workspace(saved);
        const auto& updated = std::get<Workspace>(saved);
        CHECK(updated.declaration.find("Saved definition") != std::string::npos);
        CHECK(updated.declaration.find("future_node_field") != std::string::npos);
        CHECK(updated.content_sha256 != original.content_sha256);
        CHECK(updated.author_id == original.author_id); CHECK(updated.entrypoint == original.entrypoint);
        CHECK(updated.entry_source == original.entry_source);
        CHECK(bytes(directory / ".audioalgo-authoring.json") == author_identity);
        CHECK(bytes(directory / "src/declaration_save/process.py") == source);
        const auto reopened = WorkspaceStore{}.open(directory); require_workspace(reopened);
        CHECK(std::get<Workspace>(reopened) == updated); no_temporary_files();
    }
    SECTION("an external node json edit is preserved and local definition stays available") {
        const auto external = original.node_json + "\n"; write(directory / "node.json", external);
        const auto saved = WorkspaceStore{}.save_declaration(original, edited);
        REQUIRE(std::holds_alternative<Diagnostic>(saved)); CHECK(std::get<Diagnostic>(saved).code == "workspace_definition_conflict");
        CHECK(bytes(directory / "node.json") == external); CHECK(edited.find("Saved definition") != std::string::npos); no_temporary_files();
    }
    SECTION("an external edit immediately before publication is detected") {
        const auto external = original.node_json + " \n";
        WorkspaceStore saving([&](WorkspacePhase phase) { if (phase == WorkspacePhase::replace) write(directory / "node.json", external); return true; });
        const auto saved = saving.save_declaration(original, edited);
        REQUIRE(std::holds_alternative<Diagnostic>(saved)); CHECK(std::get<Diagnostic>(saved).code == "workspace_definition_conflict");
        CHECK(bytes(directory / "node.json") == external); no_temporary_files();
    }
    SECTION("write verification and replace failures retain every original byte") {
        for (const auto rejected : {WorkspacePhase::write, WorkspacePhase::verify, WorkspacePhase::replace}) {
            const auto saved = WorkspaceStore([&](WorkspacePhase phase) { return phase != rejected; }).save_declaration(original, edited);
            REQUIRE(std::holds_alternative<Diagnostic>(saved)); CHECK(bytes(directory / "node.json") == original.node_json);
            CHECK(bytes(directory / "src/declaration_save/process.py") == source); no_temporary_files();
        }
    }
    SECTION("invalid definitions are rejected before writing") {
        for (const auto* bad : {"{broken", "[]", "{}"}) {
            const auto saved = WorkspaceStore{}.save_declaration(original, bad);
            REQUIRE(std::holds_alternative<Diagnostic>(saved)); CHECK(bytes(directory / "node.json") == original.node_json); no_temporary_files();
        }
    }
    SECTION("independent source edits are retained while saving the declaration") {
        const auto external_source = source + "\n# external source remains authoritative\n";
        write(directory / "src/declaration_save/process.py", external_source);
        const auto saved = WorkspaceStore{}.save_declaration(original, edited); require_workspace(saved);
        CHECK(std::get<Workspace>(saved).entry_source == external_source);
        CHECK(bytes(directory / "src/declaration_save/process.py") == external_source); no_temporary_files();
    }
}
