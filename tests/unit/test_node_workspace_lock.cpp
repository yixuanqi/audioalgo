// Verifies the production workspace reader against the independent W0 fixture
// lock. The lock is produced by tests/fixtures/node_workspace/tools/
// workspace_expectation.py (standard library only), never by this reader.
#include "audio_node_authoring/Workspace.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace audioalgo::authoring;
using audioalgo::Diagnostic;
fs::path fixtures() { return fs::path(AUDIOALGO_SOURCE_DIR) / "tests/fixtures/node_workspace"; }
juce::var load_json(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const std::string text(std::istreambuf_iterator<char>(stream), {});
    return juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())));
}
std::string text_of(const juce::var& value) { return value.toString().toStdString(); }
}

TEST_CASE("NWS-01.c frozen workspace fixtures match the independent expectation lock",
          "[NWS-01.c][NWS-01.a][node_workspace][lock]") {
    const auto lock = load_json(fixtures() / "expected" / "workspace_lock.json");
    REQUIRE(lock.isObject());
    CHECK(text_of(lock["algorithm"]) == "AudioAlgoNodeWorkspace/1");
    CHECK(static_cast<int>(lock["budgets"]["source_files"]) == 128);
    CHECK(static_cast<std::int64_t>(lock["budgets"]["source_bytes"]) == 8 * 1024 * 1024 - 16 * 1024);
    CHECK(static_cast<std::int64_t>(lock["budgets"]["node_json_bytes"]) == 1024 * 1024);
    CHECK(static_cast<std::int64_t>(lock["budgets"]["resource_bytes"]) == 64ll * 1024 * 1024);
    CHECK(static_cast<int>(lock["budgets"]["relative_path_bytes"]) == 512);

    const auto workspaces = lock["workspaces"];
    REQUIRE(workspaces.isObject());
    for (const auto& name : {"python_two_files", "python_two_files_v2", "native_two_sources", "preserve_unknown"}) {
        INFO(name);
        const auto expected = workspaces[name];
        REQUIRE(expected.isObject());
        const auto opened = WorkspaceStore{}.open(fixtures() / name);
        if (const auto* error = std::get_if<Diagnostic>(&opened)) INFO(error->code << " " << error->message);
        REQUIRE(std::holds_alternative<Workspace>(opened));
        const auto& workspace = std::get<Workspace>(opened);
        CHECK(workspace.language == text_of(expected["language"]));
        CHECK(workspace.declaration == text_of(expected["declaration"]));
        CHECK(WorkspaceStore::sha256_hex(workspace.declaration) == text_of(expected["declaration_sha256"]));
        CHECK(WorkspaceStore::sha256_hex(workspace.node_json) == text_of(expected["node_json_sha256"]));
        CHECK(workspace.content_sha256 == text_of(expected["content_sha256"]));
        if (text_of(expected["language"]) == "python") {
            CHECK(workspace.entrypoint == text_of(expected["entrypoint"]));
            CHECK(WorkspaceStore::sha256_hex(workspace.entry_source) == text_of(expected["entry_source_sha256"]));
        }
        const auto* files = expected["runtime_files"].getArray();
        REQUIRE(files != nullptr);
        REQUIRE(workspace.files.size() == static_cast<std::size_t>(files->size()));
        for (int index = 0; index < files->size(); ++index) {
            const auto& entry = (*files)[index];
            CHECK(workspace.files[static_cast<std::size_t>(index)].path == text_of(entry["path"]));
            CHECK(workspace.files[static_cast<std::size_t>(index)].bytes ==
                  static_cast<std::uint64_t>(static_cast<juce::int64>(entry["bytes"])));
            CHECK(workspace.files[static_cast<std::size_t>(index)].sha256 == text_of(entry["sha256"]));
        }
        // Excluded files must really exist on disk, otherwise the exclusion check proves nothing.
        const auto* excluded = expected["excluded_present"].getArray();
        REQUIRE(excluded != nullptr);
        for (int index = 0; index < excluded->size(); ++index)
            CHECK(fs::is_regular_file(fixtures() / name / text_of((*excluded)[index])));
    }

    // Only the auxiliary module changed between v1 and v2: same declaration, new identity.
    const auto v1 = workspaces["python_two_files"], v2 = workspaces["python_two_files_v2"];
    CHECK(text_of(v1["node_json_sha256"]) == text_of(v2["node_json_sha256"]));
    CHECK(text_of(v1["declaration_sha256"]) == text_of(v2["declaration_sha256"]));
    CHECK(text_of(v1["content_sha256"]) != text_of(v2["content_sha256"]));

    // The frozen conversion expectation the production create/convert path must reproduce.
    const auto conversion = lock["conversion"];
    REQUIRE(conversion.isObject());
    CHECK(text_of(conversion["entrypoint"]) == "print_a.process:process");
    CHECK(text_of(conversion["module"]) == "print_a");
    const auto* files = conversion["runtime_files"].getArray();
    REQUIRE(files != nullptr);
    CHECK(files->size() == 3);
    CHECK(text_of(lock["legacy"]["sha256"]).size() == 64);
}

TEST_CASE("NWS-01.a production create reproduces the frozen conversion bytes",
          "[NWS-01.a][NWS-01.b][node_workspace][lock]") {
    const auto lock = load_json(fixtures() / "expected" / "workspace_lock.json");
    const auto conversion = lock["conversion"];
    const auto directory = fs::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NWS0001-workspace-create/lock" /
        juce::Uuid().toString().toStdString();
    const auto converted = WorkspaceStore{}.convert(fixtures() / "legacy" / "print_a.aadraft", directory);
    if (const auto* error = std::get_if<Diagnostic>(&converted)) INFO(error->code << " " << error->message);
    REQUIRE(std::holds_alternative<Workspace>(converted));
    const auto& workspace = std::get<Workspace>(converted);
    CHECK(workspace.entrypoint == text_of(conversion["entrypoint"]));
    CHECK(workspace.declaration == text_of(conversion["declaration"]));
    CHECK(workspace.content_sha256 == text_of(conversion["content_sha256"]));
    std::ifstream node_json(directory / "node.json", std::ios::binary);
    const std::string written(std::istreambuf_iterator<char>(node_json), {});
    CHECK(written == text_of(conversion["node_json"]));
    const auto* files = conversion["runtime_files"].getArray();
    REQUIRE(files != nullptr);
    REQUIRE(workspace.files.size() == static_cast<std::size_t>(files->size()));
    for (int index = 0; index < files->size(); ++index) {
        const auto& entry = (*files)[index];
        CHECK(workspace.files[static_cast<std::size_t>(index)].path == text_of(entry["path"]));
        CHECK(workspace.files[static_cast<std::size_t>(index)].sha256 == text_of(entry["sha256"]));
        std::ifstream stream(directory / text_of(entry["path"]), std::ios::binary);
        const std::string payload(std::istreambuf_iterator<char>(stream), {});
        CHECK(WorkspaceStore::sha256_hex(payload) == text_of(entry["sha256"]));
    }
}
