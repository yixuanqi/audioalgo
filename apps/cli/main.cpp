#include <iostream>
#include <charconv>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "audio_node_sdk/NodeSDK.h"
#include "plugins/builtin/registration.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_run_service/RunService.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace {
std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

int run_cli(int argc, const char* const* argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "AudioAlgo " << AUDIOALGO_VERSION << '\n';
        return 0;
    }

    if (argc == 5 && std::string_view(argv[1]) == "run") {
        int block_size = 512;
        const std::string_view block_text(argv[4]);
        const auto parsed = std::from_chars(block_text.data(), block_text.data() + block_text.size(), block_size);
        if (parsed.ec != std::errc{} || parsed.ptr != block_text.data() + block_text.size() || block_size <= 0) {
            std::cerr << "invalid block size\n";
            return 2;
        }

        juce::ScopedJuceInitialiser_GUI message_environment;
        const auto project_file = std::filesystem::u8path(argv[2]);
        const auto output_directory = std::filesystem::u8path(argv[3]);
        const auto loaded = audioalgo::load_project_file(project_file);
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&loaded)) {
            std::cerr << error->code << ": " << error->message << '\n';
            return 1;
        }

        audioalgo::NodeRegistry nodes;
        if (!audioalgo::register_builtin_nodes(nodes)) {
            std::cerr << "builtin_node_registration_failed\n";
            return 1;
        }
        audioalgo::RunService service(nodes);
        const auto outcome = service.run({std::get<audioalgo::ProjectModel>(loaded), project_file, output_directory, block_size});
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome)) {
            std::cerr << error->code << ": " << error->message << '\n';
            return 1;
        }
        const auto& result = std::get<audioalgo::RunSuccess>(outcome);
        std::cout << "run_id=" << result.run_id << '\n'
                  << "output=" << path_to_utf8(result.output_file) << '\n'
                  << "frames=" << result.frames << '\n'
                  << "output_sha256=" << result.output_sha256 << '\n';
        return 0;
    }

    std::cout << "AudioAlgo command line runner " << AUDIOALGO_VERSION << "\n"
              << "Usage: audio_cli run <project.aaproj> <new-output-directory> <block-size>\n"
              << "       audio_cli --version\n";
    return argc == 1 ? 0 : 2;
}
} // namespace

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

namespace {
std::string wide_to_utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0')
        return {};
    const int byte_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (byte_count <= 1)
        return {};
    std::string result(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), byte_count, nullptr, nullptr);
    result.pop_back();
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> converted;
    converted.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        converted.push_back(wide_to_utf8(argv[index]));
    std::vector<const char*> arguments;
    arguments.reserve(converted.size());
    for (const auto& argument : converted)
        arguments.push_back(argument.c_str());
    return run_cli(argc, arguments.data());
}
#else
int main(int argc, char** argv) {
    return run_cli(argc, const_cast<const char* const*>(argv));
}
#endif
