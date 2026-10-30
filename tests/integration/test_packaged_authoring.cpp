#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "audio_python_backend/PythonTask.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <fstream>
#include <cstdlib>

namespace {
namespace fs = std::filesystem;
using namespace audioalgo;
using namespace audioalgo::authoring;
constexpr auto probe_name = "PKG-01 relocated host checks applies and runs a directory node using its bundled runner";
fs::path path(const juce::File& file) { return fs::u8path(file.getFullPathName().toStdString()); }
std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path()); std::ofstream out(file, std::ios::binary); out << bytes;
    REQUIRE(static_cast<bool>(out));
}
struct Environment {
    std::string name, previous;
    Environment(const char* key, const std::string& value) : name(key), previous(juce::SystemStats::getEnvironmentVariable(key, {}).toStdString()) {
        _wputenv_s(juce::String::fromUTF8(key).toWideCharPointer(), juce::String::fromUTF8(value.c_str()).toWideCharPointer());
    }
    ~Environment() { _wputenv_s(juce::String::fromUTF8(name.c_str()).toWideCharPointer(), juce::String::fromUTF8(previous.c_str()).toWideCharPointer()); }
};
template<class T, class E> const T& success(const std::variant<T,E>& outcome) {
    if (const auto* error = std::get_if<E>(&outcome)) FAIL(error->code + ": " + error->message);
    return std::get<T>(outcome);
}
}

TEST_CASE(probe_name, "[PKG-01][packaged_authoring][NA-04.a][NA-07.a][NWS-03.a]") {
    const auto executable = path(juce::File::getSpecialLocation(juce::File::currentExecutableFile));
    const auto configured = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_PACKAGE_PROBE_ROOT", {});
    if (configured.isEmpty()) {
        const auto root = fs::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PKG0001-python-runtime/artifacts" / juce::Uuid().toString().toStdString();
        for (const auto* layout : {"runner", "python"}) {
            const auto host = root / fs::u8path("打包 软件") / layout;
            fs::create_directories(host / layout);
            const auto relocated = host / executable.filename(); fs::copy_file(executable, relocated);
            fs::copy(fs::path(AUDIOALGO_SOURCE_DIR) / "python/audioalgo_runner", host / layout / "audioalgo_runner", fs::copy_options::recursive);
            // Distinguish the shipped runner from the available development tree.
            std::ofstream(host / layout / "audioalgo_runner/sdk.py", std::ios::app) << "\n# packaged runner identity\n";
            // A full Python installation must never enter the runner code budget.
            for (int i = 0; i < 140; ++i) write(host / "python/Lib" / ("unrelated" + std::to_string(i) + ".py"), "raise RuntimeError('must not import or snapshot')\n");
            Environment fixture("AUDIOALGO_PACKAGE_PROBE_ROOT", utf8(host / "work"));
            Environment interpreter("AUDIOALGO_PYTHON_EXECUTABLE", AUDIOALGO_PYTHON_EXECUTABLE);
            juce::ChildProcess child;
            REQUIRE(child.start(juce::StringArray{juce::String::fromUTF8(utf8(relocated).c_str()), probe_name}));
            REQUIRE(child.waitForProcessToFinish(60000));
            const auto output = child.readAllProcessOutput(); INFO(output.toStdString());
            write(host / "probe.log", output.toStdString());
            CHECK(child.getExitCode() == 0);
        }
        return;
    }
    const auto root = fs::u8path(configured.toStdString()), host = executable.parent_path();
    const auto modules = fs::exists(host / "runner/audioalgo_runner") ? host / "runner" : host / "python";
    fs::create_directories(root / "expected-runner");
    fs::copy(modules / "audioalgo_runner", root / "expected-runner/audioalgo_runner", fs::copy_options::recursive);
    const auto expected = freeze_python_package(root / "expected-runner", root / "expected-snapshot", "");
    const auto expected_hash = success<fs::path>(expected).filename().string();
    auto draft = python_draft("packaged-node");
    draft.source = "import sys\ndef process(inputs, params, state):\n    print('PACKAGED_INTERPRETER:' + sys.executable, flush=True)\n    return {'audio_out': inputs['audio_in'] * 0.25}\n";
    const auto created = WorkspaceStore{}.create(root / "node", draft);
    const auto workspace = success<Workspace>(created);
    draft.declaration = workspace.declaration;
    PythonAuthoring service(root / "library");
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto checked = service.check_workspace(draft, workspace, cancel);
    const auto ready = success<ReadyPackage>(checked);
    CHECK(ready.runner_sha256 == expected_hash);
    const auto runtime = resolve_python_runtime();
    CHECK(ready.interpreter_sha256 == juce::SHA256(juce::File(juce::String::fromUTF8(utf8(runtime.executable).c_str()))).toHexString().toStdString());
    std::string log_text;
    for (const auto& log : ready.logs) log_text += log.event.text;
    auto interpreter_path = utf8(runtime.executable); std::replace(interpreter_path.begin(), interpreter_path.end(), '/', '\\');
    CHECK(log_text.find("PACKAGED_INTERPRETER:" + interpreter_path) != std::string::npos);
    CHECK(ready.syntax_pid != 0); CHECK(ready.probe_pid != 0);
    const auto applied_result = service.apply(ready, ready.draft, std::make_shared<NodeRegistry>(), cancel);
    const auto applied = success<AppliedPackage>(applied_result);
    auto node = applied.catalog->create_processor(ready.descriptor.type_id, "packaged-node"); REQUIRE(node.processor);
    NodeExecutionContext context; context.output_directory = utf8(root / "run"); context.node_id = "packaged-node";
    context.identity = {"packaged-project", 1, "packaged-run", 1};
    const auto frozen = node.processor->freeze_task(context);
    if (frozen) FAIL(frozen->code + ": " + frozen->message);
    AudioBuffer input(16000, {"mono"}, 513);
    for (std::size_t i = 0; i < 513; ++i) input.mutable_channel(0)[i] = static_cast<float>(static_cast<int>(i % 31) - 15) / 32.f;
    AudioPorts inputs{{"audio_in", input}}; NodeParameters parameters;
    const auto result = node.processor->process({parameters, inputs, context});
    const auto& output = success<AudioPorts>(result).at("audio_out");
    REQUIRE(output.frame_count() == 513);
    for (std::size_t i = 0; i < 513; ++i) CHECK(output.channel(0)[i] == input.channel(0)[i] * .25f);
    CHECK_FALSE(fs::exists(root / "run/Lib"));
    std::ofstream samples(root / "output.f32", std::ios::binary);
    samples.write(reinterpret_cast<const char*>(output.channel(0).data()), output.frame_count() * sizeof(float));
    REQUIRE(static_cast<bool>(samples));
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("interpreter", juce::String::fromUTF8(utf8(runtime.executable).c_str()));
    receipt->setProperty("runner", juce::String::fromUTF8(utf8(runtime.runner_module_root).c_str()));
    receipt->setProperty("runner_sha256", juce::String(ready.runner_sha256));
    receipt->setProperty("interpreter_sha256", juce::String(ready.interpreter_sha256));
    receipt->setProperty("package_sha256", juce::String(ready.package_sha256));
    write(root / "receipt.json", juce::JSON::toString(juce::var(receipt.get()), false).toStdString());
}

TEST_CASE("PKG-01 runtime selection preserves explicit overrides and diagnoses incomplete bundles without development fallback",
          "[PKG-01][packaged_authoring][NA-04.a]") {
    const auto root = fs::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PKG0001-python-runtime/selection" / juce::Uuid().toString().toStdString();
    Environment clear("AUDIOALGO_PYTHON_EXECUTABLE", "");
    fs::create_directories(root / "host/runner");
    auto selected = resolve_python_runtime({}, {}, root / "host");
    CHECK(selected.executable == root / "host/python/python.exe");
    CHECK(selected.runner_module_root == root / "host/runner");
    REQUIRE(validate_python_runtime(selected)); CHECK(validate_python_runtime(selected)->code == "python_executable_missing");
    write(selected.executable, "fixture executable, never launched");
    REQUIRE(validate_python_runtime(selected)); CHECK(validate_python_runtime(selected)->code == "python_runner_missing");
    write(selected.runner_module_root / "audioalgo_runner/__main__.py", "# fixture, never launched\n");
    CHECK_FALSE(validate_python_runtime(selected));
    Environment override("AUDIOALGO_PYTHON_EXECUTABLE", utf8(root / "override.exe"));
    CHECK(resolve_python_runtime({}, {}, root / "host").executable == root / "override.exe");
    selected = resolve_python_runtime(root / "explicit.exe", root / "explicit-runner", root / "host");
    CHECK(selected.executable == root / "explicit.exe"); CHECK(selected.runner_module_root == root / "explicit-runner");
    PythonAuthoring missing(root / "library", selected.executable, selected.runner_module_root);
    const auto result = missing.check(python_draft("missing-runtime"), std::make_shared<std::atomic<bool>>(false));
    REQUIRE(std::holds_alternative<SourceDiagnostic>(result));
    const auto& error = std::get<SourceDiagnostic>(result);
    CHECK(error.code == "python_executable_missing"); CHECK(error.file.empty()); CHECK(error.line == 0); CHECK(error.column == 0);
    CHECK_FALSE(fs::exists(root / "library/staging"));
}

TEST_CASE("PKG-01 runner capture excludes interpreter files but retains byte identity budgets and Apply environment locking",
          "[PKG-01][packaged_authoring][NA-04.d][NWS-04.a]") {
    const auto root = fs::path(AUDIOALGO_SOURCE_DIR) / "build/reports/loops/PKG0001-python-runtime/locking" / juce::Uuid().toString().toStdString();
    fs::create_directories(root / "modules");
    fs::copy(fs::path(AUDIOALGO_SOURCE_DIR) / "python/audioalgo_runner", root / "modules/audioalgo_runner", fs::copy_options::recursive);
    const auto first = freeze_python_runner(root / "modules", root / "snapshots");
    const auto original = success<fs::path>(first);
    for (int i = 0; i < 140; ++i) write(root / "modules/Lib" / (std::to_string(i) + ".py"), "# outside runner\n");
    const auto second = freeze_python_runner(root / "modules", root / "snapshots");
    CHECK(success<fs::path>(second) == original); CHECK_FALSE(validate_python_snapshot(original));
    CHECK_FALSE(fs::exists(original / "Lib"));
    auto draft = python_draft("runner-lock");
    PythonAuthoring checker(root / "library", fs::u8path(AUDIOALGO_PYTHON_EXECUTABLE), root / "modules");
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    const auto checked = checker.check(draft, cancel); const auto ready = success<ReadyPackage>(checked);
    write(root / "modules/audioalgo_runner/new_helper.py", "# environment changed after Check\n");
    const auto applied = checker.apply(ready, draft, std::make_shared<NodeRegistry>(), cancel);
    REQUIRE(std::holds_alternative<SourceDiagnostic>(applied));
    CHECK(std::get<SourceDiagnostic>(applied).message.find("Checked runner environment changed") != std::string::npos);
    CHECK_FALSE(fs::exists(root / "library/index.json"));
    // The same 128-source-file bound still applies inside the runner itself.
    for (int i = 0; i < 129; ++i) write(root / "modules/audioalgo_runner" / ("extra" + std::to_string(i) + ".py"), "# runner source\n");
    const auto over = freeze_python_runner(root / "modules", root / "over-limit");
    REQUIRE(std::holds_alternative<Diagnostic>(over)); CHECK(std::get<Diagnostic>(over).code == "python_code_limit");
}
