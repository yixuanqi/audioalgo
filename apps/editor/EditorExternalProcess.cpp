#include "EditorExternalProcess.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace audioalgo::editor {
#ifdef _WIN32
namespace external_process_detail {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE handle) : value(handle) {}
};
// Windows argument encoding, including empty arguments and trailing backslashes.
inline std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
}
#endif

// External GUI editors outlive this call and the authoring panel. Their logging
// must not depend on a temporary ChildProcess pipe owned by AudioAlgo.
std::optional<std::string> launch_external_editor(const juce::StringArray& arguments) {
    if (arguments.isEmpty()) return "No editor executable selected";
#ifdef _WIN32
    using external_process_detail::Handle;
    const auto failure = [](const char* phase) {
        return std::string("Could not start the selected editor (") + phase + ", Windows error " +
            std::to_string(GetLastError()) + "); the project remains on disk";
    };
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) return failure("standard input");
    Handle output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) return failure("standard output");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) return failure("handle list size");
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return failure("handle list");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } guard{attributes};
    HANDLE inherited[]{input.value, output.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        return failure("standard handle inheritance");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.value;
    startup.lpAttributeList = attributes;
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += external_process_detail::quote(argument.toWideCharPointer());
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(arguments[0].toWideCharPointer(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
        &startup.StartupInfo, &process)) return failure("CreateProcess");
    Handle thread(process.hThread), child(process.hProcess);
    return {};
#else
    juce::ChildProcess process;
    if (!process.start(arguments, 0)) return "Could not start the selected editor; the project remains on disk";
    return {};
#endif
}
}
