// Owned, hidden test executable. It records arguments without opening an editor.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    std::vector<wchar_t> executable(32768);
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return 2;
    const auto target = std::filesystem::path(std::wstring(executable.data(),length)).parent_path()/"editor-argv.bin";
    auto temporary=target;temporary+=L".tmp";
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    const auto count=static_cast<std::uint32_t>(argc);output.write(reinterpret_cast<const char*>(&count),sizeof(count));
    for(int i=0;i<argc;++i) {
        const auto bytes=static_cast<std::uint32_t>(std::wstring(argv[i]).size()*sizeof(wchar_t));
        output.write(reinterpret_cast<const char*>(&bytes),sizeof(bytes));
        output.write(reinterpret_cast<const char*>(argv[i]),bytes);
    }
    output.close();if(!output)return 3;
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return 4;
    return 0;
}
