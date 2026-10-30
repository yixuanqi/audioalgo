#pragma once
#include "Draft.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace audioalgo::authoring {
// Frozen contract: doc/decisions/0040-node-workspace-schema-v1.md.
// One authoring directory holds exactly one node and one authoritative node.json;
// the runtime manifest covers node.json, src/**, include/** and explicit resources.
struct WorkspaceFile {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    bool operator==(const WorkspaceFile&) const = default;
};
struct Workspace {
    std::uint32_t schema = 1;
    std::string language;
    std::string declaration;    // canonical JSON of node.json "node"
    std::string entrypoint;     // python "module.path:function"
    std::string entry_source;   // python entrypoint bytes; empty for cpp
    std::string content_sha256; // AudioAlgoNodeWorkspace/1 over the runtime files
    std::string node_json;      // exact node.json bytes read from disk
    std::vector<WorkspaceFile> files;
    std::filesystem::path directory;
    std::string author_id; // Optional creation/conversion identity, separate from runtime content.
    std::string authoring_metadata; // Exact small identity record; never buffered source/definition.
    bool operator==(const Workspace&) const = default;
};
using WorkspaceOutcome = std::variant<Workspace, Diagnostic>;
using WorkspaceReadOutcome = std::variant<std::string, Diagnostic>;
// A directory session carries identity/definition only, never buffered source.
Draft workspace_identity(const Workspace&, std::uint64_t generation = 1);
// Failure injection seam for tests: returning false aborts the phase with a
// workspace_write_failed diagnostic and leaves every pre-existing byte intact.
enum class WorkspacePhase { create, write, verify, replace };
class WorkspaceStore {
public:
    explicit WorkspaceStore(std::function<bool(WorkspacePhase)> allow = {}) : allow_(std::move(allow)) {}
    // Creates the directory when it is absent or an existing empty directory.
    // Writes node.json plus the declared source files; never overwrites a file.
    WorkspaceOutcome create(const std::filesystem::path& target, const Draft& draft) const;
    // Read-only validation of an authoring directory. Writes nothing.
    WorkspaceOutcome open(const std::filesystem::path& directory) const;
    // Worker-only optimistic declaration save. The exact expected node.json and
    // author record are rechecked before atomic publication; source files are
    // never written. A conflict preserves both disk bytes and caller-owned edits.
    WorkspaceOutcome save_declaration(const Workspace& expected, const std::string& declaration) const;
    WorkspaceReadOutcome read_file(const Workspace&, const std::string& relative) const;
    // Worker-only stable copy. Never mutates the author directory. The optional
    // observation hook makes concurrent edits reproducible in transaction tests.
    WorkspaceOutcome snapshot(const std::filesystem::path& directory, const std::filesystem::path& target,
        const std::function<bool()>& cancelled = {},
        const std::function<void(const std::string&)>& copied = {}) const;
    // Reads a legacy .aadraft, creates the directory and re-verifies that the
    // original draft bytes are unchanged. The draft file is never written.
    WorkspaceOutcome convert(const std::filesystem::path& draft_file, const std::filesystem::path& target) const;
    // Canonical compact JSON (ADR0040 §3). Returns an empty string and sets error on failure.
    static std::string canonical_json(const std::string& document, std::string& error);
    static std::string sha256_hex(const std::string& bytes);
private:
    std::function<bool(WorkspacePhase)> allow_;
};
// Draft id normalised for a python module: lower case, '-' -> '_', digit prefix "node_".
std::string workspace_module_name(const std::string& draft_id);
}
