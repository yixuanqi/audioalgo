#pragma once
#include "audio_node_sdk/NodeSDK.h"
#include "audio_run_api/RunIdentity.h"
#include <chrono>
#include <filesystem>

namespace audioalgo {
struct PythonRuntimePaths {
    std::filesystem::path executable, runner_module_root;
};
// Explicit selections, then the interpreter environment override, then the
// installed bundle. Development paths are used only outside a bundle.
[[nodiscard]] PythonRuntimePaths resolve_python_runtime(std::filesystem::path executable = {},
    std::filesystem::path runner_module_root = {}, std::filesystem::path host_directory = {});
[[nodiscard]] std::optional<Diagnostic> validate_python_runtime(const PythonRuntimePaths&);
struct PythonTaskRequest {
    std::filesystem::path python_executable,runner_module_root,plugin_root,output_parent;
    std::string type_id,node_id;
    NodeParameters parameters;
    AudioPorts inputs;
    std::vector<std::string> output_ports;
    RunIdentity identity;
    int maximum_block_frames=512;
    std::size_t maximum_payload_bytes=64*1024*1024;
    std::function<bool()> is_cancelled;
    std::chrono::milliseconds execution_timeout{120000};
    // Optional owned diagnostic archive parent, independent of disposable PCM.
    std::filesystem::path log_parent;
    std::function<void(NodeLogEvent)> on_log;
};
struct PythonTaskStatistics {
    std::uint32_t process_id=0,exit_code=0;
    std::uint64_t stdout_bytes=0,stderr_bytes=0;
    std::uint64_t stdout_discarded_bytes=0,stderr_discarded_bytes=0;
    std::string stdout_tail,stderr_tail;
    bool forced_termination=false;
    bool log_limit_exceeded=false;
    std::filesystem::path log_directory;
};
struct PythonTaskSuccess {
    AudioPorts outputs;
    std::filesystem::path artifact_directory;
    std::string provenance_json;
};
using PythonTaskOutcome=std::variant<PythonTaskSuccess,Diagnostic>;
// Data-only capture; does not import user code or start an interpreter.
[[nodiscard]] std::variant<std::filesystem::path,Diagnostic> freeze_python_package(
    const std::filesystem::path& root,const std::filesystem::path& destination,
    const std::string& expected_manifest_sha256,const std::function<bool()>& is_cancelled={});
// Capture only audioalgo_runner/**, retaining its module prefix. Never scan
// the interpreter's Lib/site-packages even with the legacy combined layout.
[[nodiscard]] std::variant<std::filesystem::path,Diagnostic> freeze_python_runner(
    const std::filesystem::path& module_root, const std::filesystem::path& destination,
    const std::function<bool()>& is_cancelled={});
[[nodiscard]] std::optional<Diagnostic> validate_python_snapshot(const std::filesystem::path&,
    const std::function<bool()>& is_cancelled={});
// Synchronous worker API. Owns a dedicated process/Job Object and borrowed request
// only for this call. Never use from an audio callback or a GUI message handler.
[[nodiscard]] PythonTaskOutcome run_python_task(const PythonTaskRequest&,PythonTaskStatistics* statistics=nullptr);
}
