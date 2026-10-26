#include "audio_node_authoring/Draft.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>

namespace {
auto root() { return std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/NA0002-save-recovery/storage" /
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }
std::string bytes(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
}
#ifdef _WIN32
TEST_CASE("PC-01.a production draft parent preserves volume and UNC roots", "[PC-01.a][draft_files][pure]") {
    using audioalgo::authoring::draft_parent_directory;
    for (const auto* target : {LR"(D:\Print.aadraft)", LR"(\\?\D:\Print.aadraft)"}) {
        INFO(std::filesystem::path(target).string());
        CHECK(draft_parent_directory(target).native() == LR"(\\?\D:\)");
    }
    for (const auto* target : {LR"(\\server\share\Print.aadraft)", LR"(\\?\UNC\server\share\Print.aadraft)"})
        CHECK(draft_parent_directory(target).native() == LR"(\\?\UNC\server\share\)");
    CHECK(draft_parent_directory(LR"(D:\folder\Print.aadraft)").native() == LR"(\\?\D:\folder)");
    CHECK(draft_parent_directory(LR"(\\server\share\folder\Print.aadraft)").native() == LR"(\\?\UNC\server\share\folder)");
    for (const auto* bad : {L"Print.aadraft", L"D:", L"D:Print.aadraft", L"D:\\", L""}) {
        CHECK_THROWS(draft_parent_directory(bad));
        CHECK(std::holds_alternative<audioalgo::Diagnostic>(audioalgo::authoring::DraftFiles{}.save(bad, audioalgo::authoring::python_draft("invalid"), {})));
    }
    const auto mounted_root = std::filesystem::current_path().root_path();
    REQUIRE(std::filesystem::is_directory(mounted_root));
    const auto root_target = mounted_root / L"PrintA.aadraft";
    CHECK_NOTHROW(audioalgo::authoring::ensure_draft_parent_directory(root_target));
    CHECK_NOTHROW(audioalgo::authoring::ensure_draft_parent_directory(
        std::filesystem::path(L"\\\\?\\" + root_target.wstring())));
    // The reported failure used the mounted D: root. This check is read-only:
    // it must not attempt to create or modify that existing volume root.
    if (std::filesystem::is_directory(LR"(D:\)"))
        CHECK_NOTHROW(audioalgo::authoring::ensure_draft_parent_directory(LR"(D:\PrintA.aadraft)"));
}
#endif
TEST_CASE("PC-01.b extended long draft save recovery binding and directory probe round trip", "[PC-01.b][PC-01.d][draft_files][pure]") {
    using namespace audioalgo::authoring;
    auto directory = std::filesystem::absolute(root()) / std::filesystem::u8path("卷根修复 中文 目录");
    while (directory.native().size() < 280) directory /= "long-path-segment";
#ifdef _WIN32
    directory = std::filesystem::path(L"\\\\?\\" + directory.wstring());
#endif
    const auto target = directory / std::filesystem::u8path("用户 Print.aadraft");
    auto draft = python_draft("root-fix"); draft.source = "# 中文\ndef incomplete("; draft.generation = 7;
    DraftFiles files;
    REQUIRE_FALSE(probe_authoring_directory(directory));
    auto saved = files.save(target, draft, {});
    if (auto* error = std::get_if<audioalgo::Diagnostic>(&saved)) INFO(error->message);
    REQUIRE(std::holds_alternative<DraftFile>(saved));
    CHECK(std::get<DraftFile>(files.load(target)).document == draft);
    const auto original = std::get<DraftFile>(saved).bytes;
    FileDraftStorage recovery(directory / "recovery");
    ++draft.generation; draft.source += "# unsaved";
    REQUIRE_FALSE(recovery.save_recovery(draft, target, original));
    const auto reopened = recovery.load_recovery(draft.id);
    REQUIRE(std::holds_alternative<DraftRecovery>(reopened));
    const auto& binding = std::get<DraftRecovery>(reopened);
    CHECK(binding.document == draft); CHECK(binding.original_file == target); CHECK(binding.expected_bytes == original);
    CHECK(std::get<DraftFile>(files.load(target)).bytes == original);
    CHECK(std::get<std::vector<std::string>>(recovery.list()) == std::vector<std::string>{draft.id});
    // A file used as a directory fails with an actual filesystem operation and
    // system error, then a valid target still saves the retained draft.
    auto failed = files.save(target / "impossible.aadraft", draft, {});
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(failed));
    CHECK_FALSE(std::get<audioalgo::Diagnostic>(failed).message.empty());
    CHECK(std::get<DraftFile>(files.load(target)).bytes == original);
    REQUIRE(std::holds_alternative<DraftFile>(files.save(directory / "recovered.aadraft", draft, {})));
}
TEST_CASE("NA-01.b recovery backup never marks the user file saved", "[NA-01.b][draft_files][pure]") {
    using namespace audioalgo::authoring;
    DraftSession session(python_draft("separate"));
    CHECK_FALSE(session.recovery_due(499)); CHECK(session.recovery_due(500));
    session.recovered(session.document().generation);
    CHECK(session.recovery_current()); CHECK(session.dirty()); CHECK_FALSE(session.recovery_due(1000));
    const auto old = session.document().generation;
    session.edit("new source", "new definition", 1500);
    session.saved(old); session.recovered(old);
    CHECK(session.dirty()); CHECK_FALSE(session.recovery_current());
    CHECK_FALSE(session.recovery_due(1999)); CHECK(session.recovery_due(2000));
}
TEST_CASE("NA-01.c recovered draft remembers its original file and rejects a torn binding", "[NA-01.c][draft_files][pure]") {
    using namespace audioalgo::authoring;
    const auto directory = root(); FileDraftStorage storage(directory / "recovery");
    const auto original_file = directory / "user chosen.aadraft";
    auto draft = python_draft("binding"); const auto saved = DraftFiles{}.save(original_file, draft, {});
    REQUIRE(std::holds_alternative<DraftFile>(saved));
    const auto expected = std::get<DraftFile>(saved).bytes;
    draft.source = "unsaved recovered text"; ++draft.generation;
    REQUIRE_FALSE(storage.save_recovery(draft, original_file, expected));
    auto recovered = storage.load_recovery(draft.id); REQUIRE(std::holds_alternative<DraftRecovery>(recovered));
    CHECK(std::get<DraftRecovery>(recovered).document == draft);
    CHECK(std::get<DraftRecovery>(recovered).original_file == original_file);
    CHECK(std::get<DraftRecovery>(recovered).expected_bytes == expected);
    CHECK(std::get<DraftRecovery>(recovered).recovery_file.filename() == "binding.aadraft");
    CHECK(std::get<DraftRecovery>(recovered).updated_unix_seconds > 0);
    ++draft.generation; draft.source = "newer recovery before metadata commit";
    REQUIRE_FALSE(storage.save(draft)); // crash between text and binding transactions
    recovered = storage.load_recovery(draft.id); REQUIRE(std::holds_alternative<DraftRecovery>(recovered));
    CHECK(std::get<DraftRecovery>(recovered).document == draft);
    CHECK(std::get<DraftRecovery>(recovered).original_file.empty());
    CHECK_FALSE(std::get<DraftRecovery>(recovered).warning.empty());
    CHECK(std::get<DraftFile>(DraftFiles{}.load(original_file)).bytes == expected);
}
TEST_CASE("NA-01.d chosen draft filenames preserve identity and reject conflicts atomically", "[NA-01.d][NA-01.b][draft_files][pure]") {
    using namespace audioalgo::authoring;
    auto directory = root() / std::filesystem::u8path("中文 选择目录");
    while (directory.native().size() < 280) directory /= "long-path-segment";
    const auto target = directory / std::filesystem::u8path("用户 节点.aadraft");
    DraftFiles files; auto draft = python_draft("logical-identity"); draft.source = "# 中文\ndef incomplete(";
    auto result = files.save(target, draft, {}); REQUIRE(std::holds_alternative<DraftFile>(result));
    const auto original = std::get<DraftFile>(result).bytes;
    auto loaded = files.load(target); REQUIRE(std::holds_alternative<DraftFile>(loaded));
    CHECK(std::get<DraftFile>(loaded).document == draft);
    CHECK(std::holds_alternative<audioalgo::Diagnostic>(files.save(target, draft, {})));
    for (const auto phase : {SavePhase::write, SavePhase::verify, SavePhase::replace}) {
        DraftFiles failing([phase](SavePhase p) { return p != phase; });
        auto changed = draft; changed.source = "changed"; ++changed.generation;
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(failing.save(target, changed, original)));
        CHECK(std::get<DraftFile>(files.load(target)).bytes == original);
    }
    auto external = draft; external.source = "external editor"; ++external.generation;
    REQUIRE(std::holds_alternative<DraftFile>(files.save(target, external, original)));
    const auto conflict = files.save(target, draft, original);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(conflict));
    CHECK(std::get<audioalgo::Diagnostic>(conflict).code == "draft_file_conflict");
    CHECK(std::get<DraftFile>(files.load(target)).document == external);
}
TEST_CASE("NA-01.b injected clock coalesces autosaves and stale saves cannot mark new text saved",
          "[NA-01.b][node_authoring][pure]") {
    using namespace audioalgo::authoring;
    DraftSession session(python_draft("clock"), true);
    session.edit("first", "definition", 100);
    CHECK_FALSE(session.save_due(599)); CHECK(session.save_due(600));
    const auto frozen = session.document();
    session.edit("second", "definition", 590);
    session.saved(frozen.generation); CHECK(session.dirty());
    CHECK_FALSE(session.save_due(1089)); CHECK(session.save_due(1090));
    CHECK(session.save_due(1590));
    session.saved(session.document().generation); CHECK_FALSE(session.dirty());
}
TEST_CASE("NA-01.b write verify and replace failures preserve original bytes and recover",
          "[NA-01.b][node_authoring][pure]") {
    using namespace audioalgo::authoring;
    const auto path = root(); FileDraftStorage storage(path);
    auto original = python_draft("atomic"); REQUIRE_FALSE(storage.save(original));
    const auto before = bytes(path / "atomic.aadraft");
    for (auto phase : {SavePhase::write, SavePhase::verify, SavePhase::replace}) {
        FileDraftStorage failing(path, [phase](SavePhase p) { return p != phase; });
        auto changed = original; changed.source = "changed"; ++changed.generation;
        REQUIRE(failing.save(changed));
        CHECK(bytes(path / "atomic.aadraft") == before);
        REQUIRE(std::holds_alternative<Draft>(storage.load("atomic")));
        CHECK(std::get<Draft>(storage.load("atomic")) == original);
        for (auto& entry : std::filesystem::directory_iterator(path)) CHECK(entry.path().extension() == ".aadraft");
    }
    original.source = "recovered"; ++original.generation;
    REQUIRE_FALSE(storage.save(original)); CHECK(std::get<Draft>(storage.load("atomic")) == original);
}
TEST_CASE("NA-01.d two drafts stay independent and traversal is rejected",
          "[NA-01.d][node_authoring][pure]") {
    using namespace audioalgo::authoring;
    const auto path = root(); FileDraftStorage storage(path);
    auto a = python_draft("a"), b = python_draft("b"); b.source = "other";
    REQUIRE_FALSE(storage.save(a)); REQUIRE_FALSE(storage.save(b));
    CHECK(std::get<Draft>(storage.load("a")) == a); CHECK(std::get<Draft>(storage.load("b")) == b);
    CHECK(std::get<std::vector<std::string>>(storage.list()) == std::vector<std::string>{"a", "b"});
    for (auto bad : {"../escape", "a/b", "a\\b", ".", "C:drive", ""}) {
        a.id = bad; CHECK(storage.save(a).has_value()); CHECK(std::holds_alternative<audioalgo::Diagnostic>(storage.load(bad)));
    }
    // Junctions are exercised by a Windows infrastructure fixture; creating a
    // symbolic link may need a privilege, so this test never silently skips one.
}
