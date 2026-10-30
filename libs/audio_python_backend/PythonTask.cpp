#include "PythonTask.h"
#include "audio_core/Utf8LogDecoder.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

namespace audioalgo {
namespace {
using Clock=std::chrono::steady_clock;
struct Failure {Diagnostic diagnostic;};
[[noreturn]] void fail(std::string code,std::string message,std::string port={}) {
    throw Failure{{std::move(code),std::move(message),{},std::move(port)}};
}
void require(bool ok,const char* code,const char* message) {if(!ok)fail(code,message);}
juce::String js(const std::string& value){return juce::String::fromUTF8(value.data(),static_cast<int>(value.size()));}
std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto native=std::filesystem::absolute(path).lexically_normal().wstring();
    if(native.rfind(L"\\\\?\\",0)==0)return native;
    if(native.rfind(L"\\\\",0)==0)return L"\\\\?\\UNC\\"+native.substr(2);
    return L"\\\\?\\"+native;
#else
    return path;
#endif
}
juce::File file(const std::filesystem::path& path){const auto s=path.u8string();return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(s.data()),static_cast<int>(s.size())));}
std::string hash(const std::filesystem::path& path){return juce::SHA256(file(io_path(path))).toHexString().toStdString();}
std::string field(const juce::var& v,const char* key){require(v[key].isString(),"python_protocol_shape","Expected a string field");return v[key].toString().toStdString();}
std::size_t number(const juce::var& v){require((v.isInt()||v.isInt64()) && static_cast<juce::int64>(v)>=0,"python_protocol_shape","Expected a nonnegative integer");return static_cast<std::size_t>(static_cast<juce::int64>(v));}
struct PackageInventory {
    std::map<std::string,std::filesystem::path> files;
    std::set<std::string> resources;
    std::map<std::string,std::pair<std::size_t,std::string>> expected;
    bool operator==(const PackageInventory&) const = default;
};
PackageInventory package_inventory(const std::filesystem::path& root, bool runner_only = false) {
    PackageInventory result;
    std::set<std::string> declared;
    bool workspace = false;
    if(!runner_only && std::filesystem::is_regular_file(io_path(root/"manifest.json"))) {
        std::ifstream stream(io_path(root/"manifest.json"),std::ios::binary);
        std::string bytes(1024*1024+1,'\0');stream.read(bytes.data(),bytes.size());bytes.resize(static_cast<std::size_t>(stream.gcount()));
        require(bytes.size()<=1024*1024,"python_code_limit","Package manifest exceeds1 MiB");
        const auto manifest=juce::JSON::parse(js(bytes));
        if(manifest.hasProperty("workspace_files")) {
            const auto profile=manifest["authoring_profile"];
            require(field(profile,"kind")=="workspace-function-v1" && profile["syntax_only"].isBool(),"python_authoring_profile_invalid","Invalid workspace file profile");
            const auto* entries=manifest["workspace_files"].getDynamicObject();
            require(entries && entries->getProperties().size()<=191,"python_code_limit","Invalid workspace runtime file list");
            workspace=true;declared.insert("manifest.json");
            for(int i=0;i<entries->getProperties().size();++i) {
                const auto name=entries->getProperties().getName(i).toString().toStdString();
                const auto path=std::filesystem::u8path(name);
                const auto entry=entries->getProperties().getValueAt(i);
                require(!name.empty() && name.size()<=522 && name.find('\\')==std::string::npos && name.find(':')==std::string::npos &&
                    !path.has_root_path() && path.lexically_normal()==path && *path.begin()!=".." && name!="manifest.json" && name!="snapshot.json",
                    "python_code_path_invalid","Invalid workspace runtime path");
                const auto role=field(entry,"role"),digest=field(entry,"sha256");
                require((role=="source" || role=="resource") && std::regex_match(digest,std::regex("[0-9a-f]{64}")),"python_authoring_profile_invalid","Invalid workspace runtime identity");
                if(role=="resource")result.resources.insert(name);
                result.expected.emplace(name,std::pair{number(entry["bytes"]),digest});declared.insert(name);
            }
        }
    }
    const auto scan_root = runner_only ? root / "audioalgo_runner" : root;
    if (runner_only) {
        if (!std::filesystem::is_regular_file(io_path(scan_root / "__main__.py")))
            fail("python_runner_missing", "Python runner is missing: " + file(scan_root / "__main__.py").getFullPathName().toStdString());
        bool linked = std::filesystem::is_symlink(std::filesystem::symlink_status(io_path(scan_root)));
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(io_path(scan_root).c_str());
        linked = linked || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
#endif
        require(!linked, "python_code_path_invalid", "Runner snapshots cannot follow a linked package directory");
    }
    for(const auto& item:std::filesystem::recursive_directory_iterator(io_path(scan_root))) {
        bool linked=item.is_symlink();
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(item.path().c_str());
        linked=linked || (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT));
#endif
        require(!linked,"python_code_path_invalid","Package snapshots cannot include linked files or directories");
        if(!item.is_regular_file())continue;
        const auto relative=item.path().lexically_relative(io_path(root)).generic_u8string();
        const auto name=std::string(reinterpret_cast<const char*>(relative.data()),relative.size());
        if(workspace) {
            require(name=="snapshot.json" || declared.contains(name),"python_snapshot_modified","Workspace has an undeclared runtime file");
            if(name!="snapshot.json")result.files.emplace(name,item.path());
        } else if(item.path().extension()==".py" || name=="manifest.json")result.files.emplace(name,item.path());
    }
    require(result.files.size()-result.resources.size()<=128 && result.resources.size()<=64,"python_code_limit","Package exceeds source/resource file budget");
    if(workspace)require(result.files.size()==declared.size(),"python_snapshot_modified","Declared workspace runtime file is missing");
    return result;
}
juce::var array(std::initializer_list<juce::var> values){juce::Array<juce::var> a;for(auto& v:values)a.add(v);return a;}
void set_identity(juce::DynamicObject& object,const PythonTaskRequest& r,const char* kind) {
    object.setProperty("version",1);object.setProperty("message_type",kind);object.setProperty("node_id",js(r.node_id));
    object.setProperty("project_id",js(r.identity.project_id));object.setProperty("session_epoch",static_cast<juce::int64>(r.identity.session_epoch));
    object.setProperty("request_id",js(r.identity.request_id));object.setProperty("run_id",js(r.identity.request_id));object.setProperty("revision",static_cast<juce::int64>(r.identity.revision));
}
void verify_identity(const juce::var& value,const PythonTaskRequest& r) {
    require(field(value,"node_id")==r.node_id && field(value,"project_id")==r.identity.project_id &&
        field(value,"request_id")==r.identity.request_id && field(value,"run_id")==r.identity.request_id &&
        number(value["revision"])==r.identity.revision && number(value["session_epoch"])==r.identity.session_epoch,
        "python_identity_mismatch","Runner response belongs to a different request");
}
void write_npy(const std::filesystem::path& path,const AudioBuffer& audio) {
    static_assert(std::endian::native==std::endian::little,"Current PCM payload profile is little-endian");
    std::string header="{'descr': '<f4', 'fortran_order': False, 'shape': ("+std::to_string(audio.channel_count())+", "+std::to_string(audio.frame_count())+"), }";
    header.append((64-(10+header.size()+1)%64)%64,' ');header+='\n';
    std::array<unsigned char,10> magic{0x93,'N','U','M','P','Y',1,0,static_cast<unsigned char>(header.size()&255),static_cast<unsigned char>(header.size()>>8)};
    std::ofstream out(io_path(path),std::ios::binary);out.write(reinterpret_cast<const char*>(magic.data()),magic.size());out<<header;
    for(std::size_t c=0;c<audio.channel_count();++c)out.write(reinterpret_cast<const char*>(audio.channel(c).data()),static_cast<std::streamsize>(audio.frame_count()*sizeof(float)));
    out.close();require(static_cast<bool>(out),"python_payload_write_failed","Could not materialize PCM input");
}
std::filesystem::path confined(const std::filesystem::path& root,const std::string& name) {
    const auto relative=std::filesystem::u8path(name);require(!relative.is_absolute(),"python_payload_path_invalid","Runner returned an absolute path");
    const auto target=std::filesystem::weakly_canonical(root/relative);
    const auto mismatch=std::mismatch(root.begin(),root.end(),target.begin(),target.end());
    require(mismatch.first==root.end() && target!=root,"python_payload_path_invalid","Runner payload escapes its private task directory");return target;
}
AudioBuffer read_npy(const std::filesystem::path& path,const AudioBuffer& reference,const juce::var& meta,std::size_t limit) {
    const auto size=std::filesystem::file_size(io_path(path));require(size<=limit+10000 && size>=10,"python_payload_limit","Returned array is outside its byte budget");
    require(number(meta["byte_size"])==size,"python_payload_size_mismatch","Payload size differs from the sidecar");
    // Hash and parse the same owned bytes; never hash one file version and execute another.
    std::vector<char> bytes(static_cast<std::size_t>(size));std::ifstream in(io_path(path),std::ios::binary);in.read(bytes.data(),static_cast<std::streamsize>(size));
    require(static_cast<bool>(in),"python_payload_read_failed","Could not read complete Python output");
    require(juce::SHA256(bytes.data(),bytes.size()).toHexString().toStdString()==field(meta,"sha256"),"python_payload_hash_mismatch","Returned PCM bytes differ from the sidecar hash");
    require(std::memcmp(bytes.data(),"\x93NUMPY",6)==0 && bytes[7]==0,"python_payload_format","Invalid NPY magic/version");
    const auto u=[&](std::size_t i){return static_cast<unsigned char>(bytes[i]);};
    std::size_t offset=10,length=u(8)+(static_cast<std::size_t>(u(9))<<8);
    if(bytes[6]==2){require(size>=12,"python_payload_format","Truncated NPY2 header");offset=12;length+=static_cast<std::size_t>(u(10))<<16;length+=static_cast<std::size_t>(u(11))<<24;}
    else require(bytes[6]==1,"python_payload_format","Unsupported NPY header version");
    require(length<=10000 && offset+length<=size,"python_payload_format","Invalid NPY header size");
    const std::string header(bytes.data()+offset,length);std::smatch match;
    static const std::regex pattern(R"npy(\{\s*'descr'\s*:\s*'<f4'\s*,\s*'fortran_order'\s*:\s*False\s*,\s*'shape'\s*:\s*\(\s*([0-9]+)\s*,\s*([0-9]+)\s*\)\s*,?\s*\}\s*)npy");
    require(std::regex_match(header,match,pattern),"python_output_dtype_invalid","Output must be canonical C-order float32 NPY");
    require(std::stoull(match[1].str())==reference.channel_count() && std::stoull(match[2].str())==reference.frame_count(),"python_output_shape_invalid","Returned array shape differs from the planned PCM stream");
    offset+=length;const auto data_bytes=reference.channel_count()*reference.frame_count()*sizeof(float);
    require(size-offset==data_bytes,"python_payload_size_mismatch","NPY data size differs from its shape");
    auto* shape=meta["shape"].getArray();auto* ids=meta["channel_ids"].getArray();auto* valid=meta["valid_interval"].getArray();
    require(shape && shape->size()==2 && number((*shape)[0])==reference.channel_count() && number((*shape)[1])==reference.frame_count() && field(meta,"dtype")=="float32","python_output_shape_invalid","Output sidecar shape/dtype is invalid");
    require(number(meta["sample_rate_hz"])==reference.sample_rate_hz() && ids && static_cast<std::size_t>(ids->size())==reference.channel_count() && valid && valid->size()==2 &&
        number((*valid)[0])==reference.valid_interval().first_frame && number((*valid)[1])==reference.valid_interval().frame_count,"python_output_format_invalid","Output clock/channel/interval metadata changed");
    for(std::size_t c=0;c<reference.channel_count();++c)require((*ids)[static_cast<int>(c)].toString().toStdString()==reference.channel_ids()[c],"python_output_format_invalid","Stable channel IDs changed");
    AudioBuffer output(reference.sample_rate_hz(),reference.channel_ids(),reference.frame_count(),reference.valid_interval());
    for(std::size_t c=0;c<reference.channel_count();++c){std::memcpy(output.mutable_channel(c).data(),bytes.data()+offset+c*reference.frame_count()*4,reference.frame_count()*4);
        for(auto value:output.channel(c))require(std::isfinite(value),"python_output_non_finite","Returned output contains NaN or infinity");}
    return output;
}

#ifdef _WIN32
class Handle {
public:
    ~Handle(){reset();}Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    HANDLE get()const{return value_;}HANDLE* put(){reset();return &value_;}
    void reset(HANDLE next=nullptr){if(value_ && value_!=INVALID_HANDLE_VALUE)CloseHandle(value_);value_=next;}
    explicit operator bool()const{return value_ && value_!=INVALID_HANDLE_VALUE;}
private:HANDLE value_=nullptr;
};
void win_require(bool ok,const char* message){if(!ok)fail("python_process_start_failed",std::string(message)+" (Windows error "+std::to_string(GetLastError())+")");}
struct LogBudget {
    static constexpr std::uint64_t limit = 16 * 1024 * 1024;
    std::atomic<std::uint64_t> total{0};
    std::atomic<bool> exceeded{false}, write_failed{false};
    std::size_t claim(std::size_t size) {
        const auto before = total.fetch_add(size);
        if (before > limit || size > limit - before) exceeded.store(true);
        return before >= limit ? 0 : static_cast<std::size_t>(std::min<std::uint64_t>(size, limit - before));
    }
};
struct Ring {
    std::mutex mutex;std::uint64_t total=0;std::string tail;
    std::ofstream full;
    LogBudget* budget = nullptr;
    Utf8LogDecoder decoder;
    std::string pending;
    std::uint64_t dropped_text_bytes=0;
    bool decoder_finished=false;
    void append(const char* data,std::size_t size){std::scoped_lock lock(mutex);total+=size;tail.append(data,size);if(tail.size()>65536)tail.erase(0,tail.size()-65536);
        const auto retained=budget->claim(size);if(retained)full.write(data,static_cast<std::streamsize>(retained));if(!full)budget->write_failed.store(true);
        pending+=decoder.append(std::string_view(data,size));dropped_text_bytes+=trim_log_text(pending,65536);}
};
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(auto c:value){if(c==L'\\'){++slashes;continue;}result.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=c;}
    result.append(slashes*2,L'\\');return result+L'"';
}
std::vector<wchar_t> environment(const PythonTaskRequest& r,const std::string& token) {
    std::vector<std::wstring> entries;auto* block=GetEnvironmentStringsW();win_require(block!=nullptr,"Read process environment");
    for(const wchar_t* p=block;*p;p+=std::wcslen(p)+1){std::wstring value=p;
        if(_wcsnicmp(value.c_str(),L"PYTHONPATH=",11)!=0 && _wcsnicmp(value.c_str(),L"AUDIOALGO_RUNNER_TOKEN=",23)!=0)entries.push_back(std::move(value));}
    FreeEnvironmentStringsW(block);entries.push_back(L"PYTHONPATH="+io_path(r.runner_module_root).wstring());entries.push_back(L"AUDIOALGO_RUNNER_TOKEN="+std::wstring(token.begin(),token.end()));
    std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});std::vector<wchar_t> result;
    for(auto& e:entries){result.insert(result.end(),e.begin(),e.end());result.push_back(0);}result.push_back(0);return result;
}
class Process {
public:
    Handle job,process,out_read,err_read;std::thread out_thread,err_thread;Ring out,err;DWORD id=0,exit_code=0;bool forced=false;
    std::filesystem::path private_bootstrap_directory;
    std::filesystem::path log_directory;
    LogBudget log_budget;
    std::uint64_t sequence=0;
    Clock::time_point last_delivery{};
    void drain_logs(const PythonTaskRequest& request,bool final=false) {
        if(!request.on_log)return;
        const auto now=Clock::now();if(!final && now-last_delivery<std::chrono::milliseconds(20))return;
        last_delivery=now;
        for(auto [ring,source]:{std::pair{&out,"stdout"},std::pair{&err,"stderr"}}) {
            NodeLogEvent event;
            {
                std::scoped_lock lock(ring->mutex);
                if(final && !ring->decoder_finished){ring->pending+=ring->decoder.finish();ring->decoder_finished=true;ring->dropped_text_bytes+=trim_log_text(ring->pending,65536);}
                if(ring->pending.empty() && !ring->dropped_text_bytes)continue;
                event.text.swap(ring->pending);event.total_bytes=ring->total;
                event.discarded_bytes=ring->total-ring->tail.size();event.dropped_text_bytes=ring->dropped_text_bytes;ring->dropped_text_bytes=0;
            }
            event.identity=request.identity;event.node_id=request.node_id;event.type_id=request.type_id;
            event.sequence=++sequence;event.received_unix_ms=static_cast<std::uint64_t>(juce::Time::currentTimeMillis());
            event.source=source;event.level=event.source=="stdout"?"info":"error";
            event.report_file=file(log_directory/(std::string(source)+".log")).getFullPathName().toStdString();
            event.raw_log_file=event.report_file;
            request.on_log(std::move(event));
        }
    }
    ~Process(){finish();}
    void start(const PythonTaskRequest& r,const std::filesystem::path& directory,int port,const std::string& token) {
        const auto parent=r.log_parent.empty()?r.output_parent:r.log_parent;
        std::filesystem::create_directories(io_path(parent));
        log_directory=parent/("python-log-"+juce::Uuid().toString().toStdString());
        require(std::filesystem::create_directory(io_path(log_directory)),"python_log_create_failed","Could not reserve complete log archive");
        out.budget=&log_budget;err.budget=&log_budget;
        out.full.open(io_path(log_directory/"stdout.log"),std::ios::binary);err.full.open(io_path(log_directory/"stderr.log"),std::ios::binary);
        require(static_cast<bool>(out.full) && static_cast<bool>(err.full),"python_log_create_failed","Could not open complete log streams");
        job.reset(CreateJobObjectW(nullptr,nullptr));win_require(static_cast<bool>(job),"Create Python Job Object");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        win_require(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"Set Python Job Object lifetime");
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle out_write,err_write,input;
        win_require(CreatePipe(out_read.put(),out_write.put(),&security,0)!=0 && CreatePipe(err_read.put(),err_write.put(),&security,0)!=0,"Create Python log pipes");
        win_require(SetHandleInformation(out_read.get(),HANDLE_FLAG_INHERIT,0)!=0 && SetHandleInformation(err_read.get(),HANDLE_FLAG_INHERIT,0)!=0,"Isolate Python pipe handles");
        input.reset(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));win_require(static_cast<bool>(input),"Open Python stdin");
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::byte> storage(size);
        auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());win_require(InitializeProcThreadAttributeList(attributes,1,0,&size)!=0,"Initialize Python handle list");
        struct AttributeGuard{LPPROC_THREAD_ATTRIBUTE_LIST value;~AttributeGuard(){DeleteProcThreadAttributeList(value);}} attribute_guard{attributes};
        std::array<HANDLE,3> handles{input.get(),out_write.get(),err_write.get()};
        win_require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr)!=0,"Restrict Python inherited handles");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
        startup.StartupInfo.hStdInput=input.get();startup.StartupInfo.hStdOutput=out_write.get();startup.StartupInfo.hStdError=err_write.get();startup.lpAttributeList=attributes;
        const std::wstring command=quote(r.python_executable.wstring())+L" -u -m audioalgo_runner --port "+std::to_wstring(port)+L" --run-root "+quote(directory.wstring())+L" --plugin-root "+quote(r.plugin_root.wstring())+L" --max-payload-bytes "+std::to_wstring(r.maximum_payload_bytes);
        // Windows also rejects long paths in SetCurrentDirectory. Keep all
        // payloads at their selected absolute paths, with a separate private
        // scratch cwd only when the task directory cannot be used as a cwd.
        auto bootstrap_directory = directory;
        if (bootstrap_directory.native().size() >= MAX_PATH - 1) {
            const auto candidate = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal()
                / ("AudioAlgo-python-" + juce::Uuid().toString().toStdString());
            require(candidate.native().size() < MAX_PATH - 1,
                "python_process_start_failed", "Windows TEMP must provide a short private Python working directory");
            require(std::filesystem::create_directory(io_path(candidate)),
                "python_process_start_failed", "Could not create private Python working directory");
            private_bootstrap_directory = candidate; // Own cleanup only after exclusive creation succeeded.
            bootstrap_directory = private_bootstrap_directory;
        }
        auto env=environment(r,token);std::vector<wchar_t> mutable_command(command.begin(),command.end());mutable_command.push_back(0);PROCESS_INFORMATION info{};
        win_require(CreateProcessW(r.python_executable.c_str(),mutable_command.data(),nullptr,nullptr,TRUE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,env.data(),bootstrap_directory.c_str(),&startup.StartupInfo,&info)!=0,"Start explicit Python interpreter");
        process.reset(info.hProcess);Handle main_thread;main_thread.reset(info.hThread);id=info.dwProcessId;
        if(!AssignProcessToJobObject(job.get(),process.get())){TerminateProcess(process.get(),1);win_require(false,"Assign suspended Python process to its Job Object");}
        win_require(ResumeThread(main_thread.get())!=static_cast<DWORD>(-1),"Resume Python process");out_write.reset();err_write.reset();
        const auto drain=[](HANDLE handle,Ring& ring){std::array<char,4096> bytes{};DWORD count=0;while(ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr) && count!=0)ring.append(bytes.data(),count);};
        out_thread=std::thread(drain,out_read.get(),std::ref(out));err_thread=std::thread(drain,err_read.get(),std::ref(err));
    }
    bool running()const{return process && WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT;}
    void kill(){if(job)TerminateJobObject(job.get(),0xC000013A);else if(process)TerminateProcess(process.get(),0xC000013A);forced=true;if(process)WaitForSingleObject(process.get(),500);}
    void finish(){
        if(process && running())kill();
        // Closing the Job Object also reclaims descendants after a normally exiting parent.
        job.reset();if(process){WaitForSingleObject(process.get(),500);GetExitCodeProcess(process.get(),&exit_code);}
        if(out_thread.joinable())out_thread.join();if(err_thread.joinable())err_thread.join();out_read.reset();err_read.reset();process.reset();
        for(auto* stream:{&out.full,&err.full})if(stream->is_open()){stream->close();if(!*stream)log_budget.write_failed.store(true);}
        if (!private_bootstrap_directory.empty()) {
            std::error_code ignored;
            const auto expected_parent = std::filesystem::absolute(std::filesystem::temp_directory_path(), ignored).lexically_normal();
            if (!ignored && private_bootstrap_directory.parent_path() == expected_parent
                && private_bootstrap_directory.filename().string().starts_with("AudioAlgo-python-"))
                std::filesystem::remove_all(io_path(private_bootstrap_directory), ignored);
            private_bootstrap_directory.clear();
        }
    }
    void statistics(PythonTaskStatistics& stats){stats.process_id=id;stats.exit_code=exit_code;stats.forced_termination=forced;
        stats.stdout_bytes=out.total;stats.stderr_bytes=err.total;
        Utf8LogDecoder stdout_decoder,stderr_decoder;
        stats.stdout_tail=stdout_decoder.append(out.tail);stats.stdout_tail+=stdout_decoder.finish();
        stats.stderr_tail=stderr_decoder.append(err.tail);stats.stderr_tail+=stderr_decoder.finish();
        trim_log_text(stats.stdout_tail,65536);trim_log_text(stats.stderr_tail,65536);
        stats.stdout_discarded_bytes=out.total-out.tail.size();stats.stderr_discarded_bytes=err.total-err.tail.size();
        stats.log_directory=log_directory;stats.log_limit_exceeded=log_budget.exceeded.load();}
};

class Session {
public:
    Session(Process& p,const PythonTaskRequest& request):process(p),r(request),deadline(Clock::now()+r.execution_timeout){}
    std::unique_ptr<juce::StreamingSocket> socket;bool execute_sent=false;
    void checkpoint(bool allow_cancel=true) {
        process.drain_logs(r);
        if(process.log_budget.exceeded.load()){process.kill();fail("log_limit","Python stdout/stderr exceeded the complete 16 MiB log budget");}
        if(process.log_budget.write_failed.load()){process.kill();fail("python_log_write_failed","Could not persist complete Python log bytes");}
        const auto now=Clock::now();
        if(allow_cancel && r.is_cancelled && r.is_cancelled() && !cancelled_at){
            cancelled_at=now;
            if(!execute_sent){process.kill();fail("run_cancelled","Python startup/control submission cancelled");}
            juce::DynamicObject::Ptr cancel=new juce::DynamicObject;set_identity(*cancel,r,"cancel");send(juce::var(cancel.get()),false);
        }
        if(cancelled_at && now-*cancelled_at>=std::chrono::seconds(2)){process.kill();fail("run_cancelled","Python process tree reclaimed after its cooperative cancellation budget");}
        if(now>=deadline){process.kill();fail("python_execution_timeout","Python execution exceeded its configured deadline");}
    }
    void transfer(char* bytes,std::size_t size,bool writing,bool allow_cancel=true) {
        std::size_t offset=0;
        while(offset<size){checkpoint(allow_cancel);const auto ready=socket->waitUntilReady(!writing,50);
            if(ready<0)fail("python_runner_disconnected","Python control connection failed");
            if(ready==0){if(!process.running())fail(cancelled_at?"run_cancelled":"python_runner_exited","Python process exited before a terminal response");continue;}
            const auto count=static_cast<int>(std::min<std::size_t>(16384,size-offset));
            const auto actual=writing?socket->write(bytes+offset,count):socket->read(bytes+offset,count,false);
            if(actual>0)offset+=static_cast<std::size_t>(actual);
            else if(!writing || !process.running() || WSAGetLastError()!=WSAEWOULDBLOCK)
                fail(cancelled_at?"run_cancelled":"python_runner_disconnected","Python control connection closed");
        }
    }
    void send(const juce::var& value,bool allow_cancel=true) {
        auto data=juce::JSON::toString(value,true).toStdString();require(data.size()<=1048576,"protocol_message_too_large","Control message exceeds1MiB");
        const auto n=static_cast<std::uint32_t>(data.size());std::array<char,4> header{static_cast<char>(n>>24),static_cast<char>(n>>16),static_cast<char>(n>>8),static_cast<char>(n)};
        transfer(header.data(),header.size(),true,allow_cancel);transfer(data.data(),data.size(),true,allow_cancel);
    }
    juce::var receive() {
        std::array<char,4> header{};transfer(header.data(),header.size(),false);std::uint32_t size=0;for(auto c:header)size=(size<<8)|static_cast<unsigned char>(c);
        require(size>0 && size<=1048576,"protocol_message_too_large","Runner control length exceeds1MiB");std::string data(size,'\0');transfer(data.data(),data.size(),false);
        juce::var result;require(juce::JSON::parse(js(data),result).wasOk() && result.isObject(),"python_protocol_json_invalid","Runner returned invalid JSON");
        require(number(result["version"])==1,"protocol_version_invalid","Runner protocol version differs");return result;
    }
    void accept(juce::StreamingSocket& listener,const std::string& token) {
        const auto startup=Clock::now()+std::chrono::seconds(5);
        while(!socket){checkpoint();require(Clock::now()<startup,"python_startup_timeout","Runner did not connect within startup budget");
            if(!process.running())fail("python_runner_exited","Python exited during startup");
            if(listener.waitUntilReady(true,50)>0)socket.reset(listener.waitForNextConnection());}
        u_long nonblocking=1;require(ioctlsocket(static_cast<SOCKET>(socket->getRawSocketHandle()),FIONBIO,&nonblocking)==0,"python_socket_failed","Could not set bounded nonblocking control IO");
        const auto original=deadline;deadline=std::min(deadline,startup);const auto hello=receive();deadline=original;
        require(field(hello,"message_type")=="hello" && field(hello,"token")==token && number(hello["pid"])==process.id,"python_authentication_failed","Python handshake token/PID mismatch");
    }
    void shutdown() {
        if(!socket || !process.running())return;
        try{juce::DynamicObject::Ptr message=new juce::DynamicObject;message->setProperty("version",1);message->setProperty("message_type","shutdown");send(juce::var(message.get()),false);
            WaitForSingleObject(process.process.get(),2000);}catch(...){ }
    }
private:Process& process;const PythonTaskRequest& r;Clock::time_point deadline;std::optional<Clock::time_point> cancelled_at;
};
#endif
}

PythonRuntimePaths resolve_python_runtime(std::filesystem::path executable, std::filesystem::path modules,
                                         std::filesystem::path host) {
    if (host.empty()) host = std::filesystem::u8path(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getFullPathName().toStdString());
    const bool bundled = std::filesystem::exists(io_path(host / "python")) || std::filesystem::exists(io_path(host / "runner"));
    if (executable.empty()) {
        const auto override = juce::SystemStats::getEnvironmentVariable("AUDIOALGO_PYTHON_EXECUTABLE", {});
        executable = override.isNotEmpty() ? std::filesystem::u8path(override.toStdString()) :
            bundled ? host / "python/python.exe" : std::filesystem::u8path(AUDIOALGO_PYTHON_EXE_SOURCE);
    }
    if (modules.empty()) {
        // A damaged new bundle must report its missing runner, not silently
        // consume a development tree or a different legacy installation.
        if (std::filesystem::exists(io_path(host / "runner"))) modules = host / "runner";
        else if (bundled) modules = host / "python";
        else modules = std::filesystem::u8path(AUDIOALGO_PYTHON_MODULE_SOURCE);
    }
    return {std::filesystem::absolute(executable).lexically_normal(), std::filesystem::absolute(modules).lexically_normal()};
}
std::optional<Diagnostic> validate_python_runtime(const PythonRuntimePaths& runtime) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(io_path(runtime.executable), error))
        return Diagnostic{"python_executable_missing", "Python executable is missing or unreadable: " + file(runtime.executable).getFullPathName().toStdString()};
    if (!std::filesystem::is_regular_file(io_path(runtime.runner_module_root / "audioalgo_runner/__main__.py"), error))
        return Diagnostic{"python_runner_missing", "Python runner is missing or unreadable: " + file(runtime.runner_module_root / "audioalgo_runner/__main__.py").getFullPathName().toStdString()};
    return {};
}

static std::variant<std::filesystem::path,Diagnostic> freeze_python_code(const std::filesystem::path& root,
    const std::filesystem::path& destination,const std::string& manifest_hash,const std::function<bool()>& cancelled, bool runner_only) {
    try {
        constexpr std::size_t maximum=8*1024*1024,resource_maximum=64*1024*1024;std::size_t total=0,resource_total=0;
        const auto inventory=[&] {
            auto plan=package_inventory(root, runner_only);
            require(!plan.files.empty() && (manifest_hash.empty() || plan.files.contains("manifest.json")),"python_manifest_missing","Snapshot has no required source/manifest");return plan;
        };
        const auto plan=inventory();const auto& files=plan.files;std::map<std::string,std::string> contents,digests;
        for(const auto& [name,path]:files) {
            if(cancelled && cancelled())fail("run_cancelled","Code snapshot cancelled");
            std::ifstream input(io_path(path),std::ios::binary);require(input.is_open(),"python_code_read_failed","Could not read package source");
            auto& used=plan.resources.contains(name)?resource_total:total;
            const auto limit=plan.resources.contains(name)?resource_maximum:maximum;
            const auto capacity=plan.expected.contains(name)?std::min(limit-used,plan.expected.at(name).first):limit-used;
            std::string bytes(capacity+1,'\0');input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));const auto count=static_cast<std::size_t>(input.gcount());
            require(!input.bad() && count<=limit-used,"python_code_limit","Snapshot exceeds source/resource byte budget");bytes.resize(count);used+=count;
            digests[name]=juce::SHA256(bytes.data(),bytes.size()).toHexString().toStdString();contents[name]=std::move(bytes);
            if(plan.expected.contains(name))require(plan.expected.at(name)==std::pair{count,digests[name]},"python_snapshot_modified","Workspace runtime file differs from its declared identity");
        }
        if(!manifest_hash.empty())require(digests.at("manifest.json")==manifest_hash,"python_manifest_changed","Python metadata changed; refresh the host catalog");
        require(inventory()==plan,"python_code_changed_during_snapshot","Code file set changed during capture");
        std::string signature;
        for(const auto& [name,path]:files) {
            if(cancelled && cancelled())fail("run_cancelled","Code snapshot cancelled");
            require(hash(path)==digests.at(name),"python_code_changed_during_snapshot","Code contents changed during capture");
            signature+=name+'\0'+digests.at(name)+'\n';
        }
        const auto digest=juce::SHA256(signature.data(),signature.size()).toHexString().toStdString();const auto target=destination/digest;
        std::filesystem::create_directories(io_path(target));
        for(const auto& [name,bytes]:contents) {
            const auto path=target/std::filesystem::u8path(name);std::filesystem::create_directories(io_path(path.parent_path()));
            if(std::filesystem::exists(io_path(path)))require(hash(path)==digests.at(name),"python_snapshot_modified","Existing immutable package snapshot changed");
            else {std::ofstream output(io_path(path),std::ios::binary);output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();require(static_cast<bool>(output),"python_code_write_failed","Could not materialize package snapshot");}
        }
        juce::DynamicObject::Ptr metadata=new juce::DynamicObject,hashes=new juce::DynamicObject;
        for(const auto& [name,digest_value]:digests)hashes->setProperty(js(name),js(digest_value));
        metadata->setProperty("files",juce::var(hashes.get()));metadata->setProperty("content_sha256",js(digest));
        const auto json=juce::JSON::toString(juce::var(metadata.get()),true).toStdString();const auto metadata_path=target/"snapshot.json";
        if(std::filesystem::exists(io_path(metadata_path)))require(hash(metadata_path)==juce::SHA256(json.data(),json.size()).toHexString().toStdString(),"python_snapshot_modified","Snapshot metadata changed");
        else {std::ofstream out(io_path(metadata_path),std::ios::binary);out<<json;out.close();require(static_cast<bool>(out),"python_code_write_failed","Could not record snapshot metadata");}
        return target;
    } catch(const Failure& error){return error.diagnostic;}
      catch(const std::exception& error){return Diagnostic{"python_code_snapshot_failed",error.what()};}
}

std::variant<std::filesystem::path,Diagnostic> freeze_python_package(const std::filesystem::path& root,
    const std::filesystem::path& destination,const std::string& manifest_hash,const std::function<bool()>& cancelled) {
    return freeze_python_code(root, destination, manifest_hash, cancelled, false);
}
std::variant<std::filesystem::path,Diagnostic> freeze_python_runner(const std::filesystem::path& root,
    const std::filesystem::path& destination,const std::function<bool()>& cancelled) {
    return freeze_python_code(root, destination, {}, cancelled, true);
}

std::optional<Diagnostic> validate_python_snapshot(const std::filesystem::path& root,const std::function<bool()>& cancelled) {
    try {
        std::ifstream stream(io_path(root/"snapshot.json"),std::ios::binary);require(stream.is_open(),"python_snapshot_modified","Fixed code snapshot metadata disappeared");
        std::string bytes(1024*1024+1,'\0');stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));const auto count=static_cast<std::size_t>(stream.gcount());
        require(!stream.bad() && count<=1024*1024,"python_snapshot_modified","Invalid snapshot metadata size");bytes.resize(count);
        const auto plan=package_inventory(root);
        const auto metadata=juce::JSON::parse(js(bytes));const auto* files=metadata["files"].getDynamicObject();require(files && static_cast<std::size_t>(files->getProperties().size())==plan.files.size(),"python_snapshot_modified","Invalid fixed code file list");
        std::map<std::string,std::string> expected;
        std::size_t source_bytes=0,resource_bytes=0;
        for(int i=0;i<files->getProperties().size();++i) {
            if(cancelled && cancelled())fail("run_cancelled","Fixed code validation cancelled");
            const auto name=files->getProperties().getName(i).toString().toStdString(),digest=files->getProperties().getValueAt(i).toString().toStdString();
            const auto relative=std::filesystem::u8path(name);require(!relative.is_absolute() && !relative.has_root_path() && relative.lexically_normal()==relative && !relative.empty() && *relative.begin()!="..","python_snapshot_modified","Invalid fixed code path");
            require(hash(root/relative)==digest,"python_snapshot_modified","Fixed source bytes changed");expected[name]=digest;
            const auto size=std::filesystem::file_size(io_path(root/relative));
            (plan.resources.contains(name)?resource_bytes:source_bytes)+=size;
            if(plan.expected.contains(name))require(plan.expected.at(name)==std::pair{static_cast<std::size_t>(size),digest},"python_snapshot_modified","Fixed workspace runtime identity changed");
        }
        require(source_bytes<=8*1024*1024 && resource_bytes<=64*1024*1024,"python_snapshot_modified","Fixed source/resource budget exceeded");
        std::set<std::string> actual;
        for(const auto& [name,path]:plan.files)actual.insert(name);
        require(actual.size()==expected.size() && std::all_of(actual.begin(),actual.end(),[&](const auto& n){return expected.contains(n);}),"python_snapshot_modified","Fixed source inventory changed");
        std::string signature;for(const auto& [name,digest]:expected)signature+=name+'\0'+digest+'\n';
        const auto digest=juce::SHA256(signature.data(),signature.size()).toHexString().toStdString();
        require(digest==root.filename().string() && digest==field(metadata,"content_sha256"),"python_snapshot_modified","Fixed code manifest content identity changed");
        return {};
    } catch(const Failure& e){return e.diagnostic;}catch(const std::exception& e){return Diagnostic{"python_snapshot_modified",e.what()};}
}

PythonTaskOutcome run_python_task(const PythonTaskRequest& r,PythonTaskStatistics* statistics) {
    if(statistics)*statistics={};
#ifndef _WIN32
    return Diagnostic{"python_platform_unsupported","The supervised Python process profile currently requires Windows",r.node_id,{}};
#else
    std::filesystem::path directory;Process process;std::unique_ptr<Session> session;PythonTaskStatistics collected;
    const auto close=[&]{if(session)session->shutdown();process.finish();process.drain_logs(r,true);process.statistics(collected);if(statistics)*statistics=collected;};
    try {
        require(std::filesystem::is_regular_file(io_path(r.python_executable)),"python_executable_missing","Configured Python executable is missing");
        require(std::filesystem::is_regular_file(io_path(r.runner_module_root/"audioalgo_runner/__main__.py")),"python_runner_missing","Runner module installation is missing");
        require(!r.inputs.empty() && !r.output_ports.empty() && r.output_ports.size()<=64 && r.maximum_block_frames>0 && r.maximum_block_frames<=65536,"python_request_invalid","Invalid task ports/block size");
        require(r.maximum_payload_bytes>0 && r.maximum_payload_bytes<=1024*1024*1024 && r.execution_timeout.count()>0,"python_request_invalid","Invalid task byte/time budget");
        require(!r.node_id.empty() && !r.identity.project_id.empty() && !r.identity.request_id.empty() && r.identity.revision<=INT64_MAX && r.identity.session_epoch<=INT64_MAX,"python_identity_invalid","Task requires a bounded complete identity");
        if(r.is_cancelled && r.is_cancelled())fail("run_cancelled","Python task cancelled before startup");
        const auto& reference=r.inputs.begin()->second;std::size_t bytes=0;
        for(auto& [port,audio]:r.inputs){require(audio.format_error().empty() && audio.sample_rate_hz()==reference.sample_rate_hz() && audio.channel_ids()==reference.channel_ids() && audio.frame_count()==reference.frame_count() && audio.valid_interval()==reference.valid_interval(),"python_input_format_invalid","Task PCM inputs differ in clock/channel IDs/interval");
            require(audio.frame_count()<=r.maximum_payload_bytes/(audio.channel_count()*4),"python_payload_limit","Input exceeds task budget");bytes+=audio.frame_count()*audio.channel_count()*4;}
        require(bytes<=r.maximum_payload_bytes && r.output_ports.size()<= (r.maximum_payload_bytes-bytes)/std::max<std::size_t>(1,reference.frame_count()*reference.channel_count()*4),"python_payload_limit","Aggregate task buffers exceed the byte budget");
        require(std::filesystem::is_directory(io_path(r.output_parent)),"python_output_parent_missing","Task output parent must exist");
        directory=std::filesystem::weakly_canonical(io_path(r.output_parent))/("python-task-"+juce::Uuid().toString().toStdString());require(std::filesystem::create_directory(io_path(directory)),"python_output_create_failed","Could not reserve a private task directory");
        juce::DynamicObject::Ptr input_json=new juce::DynamicObject;
        for(auto& [port,audio]:r.inputs){if(r.is_cancelled && r.is_cancelled())fail("run_cancelled","Task input materialization cancelled");
            const auto name=juce::Uuid().toString().toStdString()+".npy";const auto path=directory/name;write_npy(path,audio);juce::DynamicObject::Ptr meta=new juce::DynamicObject;
            meta->setProperty("path",js(name));meta->setProperty("sha256",js(hash(path)));meta->setProperty("sample_rate_hz",static_cast<int>(audio.sample_rate_hz()));meta->setProperty("dtype","float32");
            meta->setProperty("shape",array({static_cast<juce::int64>(audio.channel_count()),static_cast<juce::int64>(audio.frame_count())}));
            juce::Array<juce::var> ids;for(auto& id:audio.channel_ids())ids.add(js(id));meta->setProperty("channel_ids",ids);
            meta->setProperty("valid_interval",array({static_cast<juce::int64>(audio.valid_interval().first_frame),static_cast<juce::int64>(audio.valid_interval().frame_count)}));input_json->setProperty(js(port),juce::var(meta.get()));}
        juce::DynamicObject::Ptr message=new juce::DynamicObject;set_identity(*message,r,"execute");message->setProperty("type_id",js(r.type_id));message->setProperty("block_frames",r.maximum_block_frames);message->setProperty("inputs",juce::var(input_json.get()));
        juce::DynamicObject::Ptr parameters=new juce::DynamicObject;
        for(auto& [key,value]:r.parameters)std::visit([&](auto& v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::string>)parameters->setProperty(js(key),js(v));else if constexpr(std::is_same_v<T,std::int64_t>)parameters->setProperty(js(key),static_cast<juce::int64>(v));else parameters->setProperty(js(key),v);},value);
        message->setProperty("parameters",juce::var(parameters.get()));juce::StreamingSocket listener;require(listener.createListener(0,"127.0.0.1"),"python_socket_failed","Could not reserve loopback control listener");
        const auto token=juce::Uuid().toString().toStdString()+juce::Uuid().toString().toStdString();process.start(r,directory,listener.getBoundPort(),token);
        session=std::make_unique<Session>(process,r);session->accept(listener,token);session->send(juce::var(message.get()));session->execute_sent=true;
        juce::var response;
        for(;;){response=session->receive();verify_identity(response,r);const auto kind=field(response,"message_type");
            if(kind=="progress")continue;
            if(kind=="diagnostic")fail(field(response,"code"),field(response,"message")+"\n"+field(response,"traceback"),field(response,"port_id"));
            require(kind=="result","protocol_message_unknown","Unexpected terminal message type");break;}
        if(r.is_cancelled && r.is_cancelled())fail("run_cancelled","Python task cancelled before result acceptance");
        const auto result=response["result"];auto* outputs=result["outputs"].getDynamicObject();require(outputs!=nullptr,"python_protocol_shape","Result has no output map");
        std::set<std::string> expected(r.output_ports.begin(),r.output_ports.end());require(expected.size()==r.output_ports.size() && static_cast<std::size_t>(outputs->getProperties().size())==expected.size(),"python_output_ports_invalid","Returned output count differs from descriptor");
        PythonTaskSuccess success;success.artifact_directory=directory;success.provenance_json=juce::JSON::toString(result,true).toStdString();
        std::vector<std::pair<std::filesystem::path,std::string>> validated;
        for(int i=0;i<outputs->getProperties().size();++i){const auto port=outputs->getProperties().getName(i).toString().toStdString();require(expected.erase(port)!=0,"python_output_ports_invalid","Runner returned an undeclared output");const auto meta=outputs->getProperties().getValueAt(i);
            const auto path=confined(directory,field(meta,"path"));success.outputs.emplace(port,read_npy(path,reference,meta,r.maximum_payload_bytes));validated.emplace_back(path,field(meta,"sha256"));}
        close();
        require(!process.log_budget.exceeded.load(),"log_limit","Python stdout/stderr exceeded the complete 16 MiB log budget");
        require(!process.log_budget.write_failed.load(),"python_log_write_failed","Could not persist complete Python log bytes");
        for(auto& [path,digest]:validated)require(hash(path)==digest,"python_output_modified","Python output changed before worker collection completed");
        const auto executed_root=confined(directory,field(result,"code_snapshot"));
        const auto executed_plan=package_inventory(executed_root);
        const auto* code_files=result["code_files"].getDynamicObject();require(code_files && static_cast<std::size_t>(code_files->getProperties().size())==executed_plan.files.size(),"python_snapshot_modified","Missing executed code inventory");
        for(int i=0;i<code_files->getProperties().size();++i) {
            const auto name=code_files->getProperties().getName(i).toString().toStdString();
            require(executed_plan.files.contains(name),"python_snapshot_modified","Executed file was not in the runtime inventory");
            const auto source=confined(directory,field(result,"code_snapshot")+"/"+name);
            require(hash(source)==code_files->getProperties().getValueAt(i).toString().toStdString(),"python_snapshot_modified","Executed source snapshot changed during task execution");
        }
        if(r.is_cancelled && r.is_cancelled())fail("run_cancelled","Python task cancelled before terminal result publication");
        return success;
    } catch(Failure& error){close();if(error.diagnostic.node_id.empty())error.diagnostic.node_id=r.node_id;
        if(!collected.stderr_tail.empty())error.diagnostic.message+="\n"+collected.stderr_tail.substr(collected.stderr_tail.size()>4096?collected.stderr_tail.size()-4096:0);
        if(!directory.empty()){std::error_code ignored;std::filesystem::remove_all(io_path(directory),ignored);}return error.diagnostic;
    } catch(const std::exception& error){close();if(!directory.empty()){std::error_code ignored;std::filesystem::remove_all(io_path(directory),ignored);}return Diagnostic{"python_host_exception",error.what(),r.node_id,{}};}
#endif
}
}
