#define NOMINMAX
#include <windows.h>
#include <string>
#include <audioalgo/log_queue_v1.hpp>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local bool watching=false;
std::atomic<unsigned long long> allocations{0};
void count() noexcept { if(watching) allocations.fetch_add(1,std::memory_order_relaxed); }
int failures=0;
void check(bool ok,const char* expression,int line){if(!ok){++failures;std::cerr<<line<<": "<<expression<<'\n';}}
#define VERIFY(x) check(static_cast<bool>(x),#x,__LINE__)
using Queue=audioalgo::sdk::LogQueueV1;
using Record=audioalgo::sdk::LogRecordV1;
uint32_t send(Queue& q,const char* text,uint32_t bytes,uint32_t level=AA_LOG_INFO) {
    const auto sink=q.sink();const aa_log_message_v1 message{sizeof(message),AA_LOG_ABI_V1,level,bytes,text};
    watching=true;const auto code=sink.emit(sink.context,&message);watching=false;return code;
}
uint32_t invalid(Queue& q,const aa_log_message_v1& message){const auto sink=q.sink();watching=true;const auto code=sink.emit(sink.context,&message);watching=false;return code;}
bool pop(Queue& q,Record& record){watching=true;const auto ok=q.pop(record);watching=false;return ok;}
}
void* operator new(std::size_t size){count();if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
#ifdef _WIN32
void* operator new(std::size_t size,std::align_val_t alignment){count();if(auto* p=_aligned_malloc(size?size:1,static_cast<std::size_t>(alignment)))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size,std::align_val_t alignment){return ::operator new(size,alignment);}
void operator delete(void* p,std::align_val_t) noexcept{_aligned_free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{_aligned_free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{_aligned_free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{_aligned_free(p);}
#endif

int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)return 3;
    const auto main_entry=reinterpret_cast<aa_get_api_v1>(GetProcAddress(module,AA_NODE_ENTRY));const auto log_entry=reinterpret_cast<aa_get_log_api_v1>(GetProcAddress(module,AA_LOG_ENTRY));if(!main_entry||!log_entry){FreeLibrary(module);return 4;}
    aa_node_api_v1 api{};aa_node_log_api_v1 logs{};aa_error error{sizeof(error)};VERIFY(main_entry(AA_NODE_ABI_V1,sizeof(api),&api,&error)==0);VERIFY(log_entry(AA_LOG_ABI_V1,sizeof(logs),&logs,&error)==0);
    constexpr const char* modes[]={"ok","empty","full","utf8","length","size","version","level","null"};constexpr unsigned statuses[]={0,0,1,4,3,2,2,2,2};std::cout<<"{\"rows\":[";
    for(unsigned n=0;n<9;++n){
        Queue queue;auto sink=queue.sink();aa_instance instance=nullptr;constexpr char type[]="example.sdk_logging";VERIFY(api.create({type,sizeof(type)-1},&instance,&error)==0);if(!instance)return 5;VERIFY(logs.set_sink(instance,&sink,&error)==0);
        const std::string json=std::string("{\"mode\":\"")+modes[n]+"\"}";aa_prepare_v1 context{sizeof(context),48000,1,2,{json.data(),static_cast<uint32_t>(json.size())}};VERIFY(api.prepare(instance,&context,&error)==0);
        std::array<float,2> input{.125f,.25f},output{};const float* in=input.data();float* out=output.data();constexpr char in_name[]="audio_in",out_name[]="audio_out";aa_input_v1 inputs{{in_name,sizeof(in_name)-1},1,&in};aa_output_v1 outputs{{out_name,sizeof(out_name)-1},1,&out};aa_block_v1 block{sizeof(block),48000,2,1,1,&inputs,&outputs};aa_result_v1 result{sizeof(result),0,0};
        watching=true;const auto processed=api.process(instance,&block,&result,&error);api.reset(instance);watching=false;VERIFY(processed==0);VERIFY(result.consumed_frames==2&&result.produced_frames==2);VERIFY(output[0]==.25f&&output[1]==.5f);
        const auto stats=queue.statistics();VERIFY(stats.full_messages==(n==2?1:0));VERIFY(stats.full_bytes==(n==2?1024:0));VERIFY(stats.rejected_messages==(n>=3?1:0));VERIFY(stats.closed_messages==0);
        VERIFY(stats.accepted_messages==(n==2?64:n==1?3:2));VERIFY(stats.attempted_bytes==(n==2?65549:28));
        Record record{};std::string text;unsigned records=0;while(pop(queue,record)){VERIFY(record.sequence==++records);text.append(record.message,record.message_bytes);}VERIFY(records==stats.accepted_messages);
        const auto expected=n==2?std::string("fault-prefix\n")+std::string(63*1024,'F'):std::string("fault-prefix\n")+"fault-status="+std::to_string(statuses[n])+"\n";VERIFY(text==expected);
        VERIFY(logs.set_sink(instance,nullptr,&error)==0);queue.close();api.destroy(instance);
        if(n)std::cout<<',';std::cout<<"{\"mode\":\""<<modes[n]<<"\",\"producer_expected_status\":"<<statuses[n]<<",\"process_status\":"<<processed<<",\"accepted\":"<<stats.accepted_messages<<",\"full\":"<<stats.full_messages<<",\"full_bytes\":"<<stats.full_bytes<<",\"rejected\":"<<stats.rejected_messages<<",\"attempted_bytes\":"<<stats.attempted_bytes<<",\"raw_bytes\":"<<text.size()<<"}";
    }
    VERIFY(allocations.load()==0);VERIFY(FreeLibrary(module)!=0);std::cout<<"],\"status\":\""<<(failures?"failed":"passed")<<"\",\"failures\":"<<failures<<",\"host_callback_pop_allocations\":"<<allocations.load()<<"}\n";return failures?1:0;
}
