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
    // Optional lifecycle probe: write only after the launch owner has gone away.
    std::ifstream request(target.parent_path()/"probe-stdio",std::ios::binary);
    if(!request)return 0;
    const std::string name(std::istreambuf_iterator<char>(request),{});
    const std::wstring event_name(name.begin(),name.end());
    const auto gate=OpenEventW(SYNCHRONIZE,FALSE,event_name.c_str());
    if(!gate)return 5;
    const auto waited=WaitForSingleObject(gate,5000);CloseHandle(gate);
    if(waited!=WAIT_OBJECT_0)return 6;
    const char message[]="editor warning after launch owner returned\n";
    std::uint32_t observations[6]{};
    for(int index=0;index<2;++index){
        const auto handle=GetStdHandle(index==0?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
        observations[index*3]=GetFileType(handle);
        DWORD written=0;SetLastError(ERROR_SUCCESS);
        observations[index*3+1]=WriteFile(handle,message,sizeof(message)-1,&written,nullptr)!=FALSE;
        observations[index*3+2]=observations[index*3+1]?ERROR_SUCCESS:GetLastError();
    }
    const auto report=target.parent_path()/"stdio.bin";auto staged=report;staged+=L".tmp";
    std::ofstream log(staged,std::ios::binary);log.write(reinterpret_cast<const char*>(observations),sizeof(observations));log.close();
    if(!log)return 7;
    return MoveFileExW(staged.c_str(),report.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:8;
}
