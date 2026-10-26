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
extern "C" int audioalgo_c_log_header_check(void);
int main(){
    VERIFY(audioalgo_c_log_header_check()==1);
    Queue q;Record record{};const auto sink=q.sink();VERIFY(sink.struct_size==sizeof(sink));VERIFY(sink.abi_version==AA_LOG_ABI_V1);VERIFY(sink.context==&q);VERIFY(sink.emit!=nullptr);VERIFY(!pop(q,record));
    char borrowed[]="abc";
    for(unsigned n=0;n<Queue::capacity;++n)VERIFY(send(q,borrowed,3)==AA_LOG_OK);
    borrowed[0]='X';VERIFY(send(q,"full",4)==AA_LOG_QUEUE_FULL);
    for(unsigned n=0;n<Queue::capacity;++n){VERIFY(pop(q,record));VERIFY(record.sequence==n+1);VERIFY(record.message_bytes==3);VERIFY(record.level==AA_LOG_INFO);VERIFY(std::memcmp(record.message,"abc",3)==0);}
    VERIFY(!pop(q,record));
    std::array<char,AA_LOG_MAX_MESSAGE_BYTES+1> maximum{};maximum.fill('M');
    VERIFY(send(q,maximum.data(),AA_LOG_MAX_MESSAGE_BYTES)==AA_LOG_OK);VERIFY(pop(q,record));VERIFY(record.sequence==Queue::capacity+2);VERIFY(record.message_bytes==AA_LOG_MAX_MESSAGE_BYTES);VERIFY(std::memcmp(record.message,maximum.data(),AA_LOG_MAX_MESSAGE_BYTES)==0);
    const char utf8[]="\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x99\x82\n";
    VERIFY(send(q,utf8,sizeof(utf8)-1,AA_LOG_WARNING)==AA_LOG_OK);VERIFY(pop(q,record));VERIFY(record.level==AA_LOG_WARNING);VERIFY(record.message_bytes==sizeof(utf8)-1);VERIFY(std::memcmp(record.message,utf8,sizeof(utf8)-1)==0);
    const char embedded[]={'a','\0','b'};VERIFY(send(q,embedded,3,AA_LOG_DEBUG)==AA_LOG_OK);VERIFY(pop(q,record));VERIFY(record.message_bytes==3);VERIFY(std::memcmp(record.message,embedded,3)==0);
    VERIFY(send(q,nullptr,0,AA_LOG_ERROR)==AA_LOG_OK);VERIFY(pop(q,record));VERIFY(record.message_bytes==0);VERIFY(record.level==AA_LOG_ERROR);
    aa_log_message_v1 bad{sizeof(bad),AA_LOG_ABI_V1,AA_LOG_INFO,1,"x"};bad.struct_size--;VERIFY(invalid(q,bad)==AA_LOG_INVALID_ARGUMENT);bad.struct_size=sizeof(bad);bad.abi_version++;VERIFY(invalid(q,bad)==AA_LOG_INVALID_ARGUMENT);bad.abi_version=AA_LOG_ABI_V1;bad.level=999;VERIFY(invalid(q,bad)==AA_LOG_INVALID_ARGUMENT);
    VERIFY(send(q,nullptr,1)==AA_LOG_INVALID_ARGUMENT);VERIFY(send(q,maximum.data(),AA_LOG_MAX_MESSAGE_BYTES+1)==AA_LOG_MESSAGE_TOO_LONG);
    const char overlong[]={'\xc0','\xaf'},shortened[]={'\xe4','\xb8'},surrogate[]={'\xed','\xa0','\x80'},too_large[]={'\xf4','\x90','\x80','\x80'};
    VERIFY(send(q,overlong,2)==AA_LOG_INVALID_UTF8);VERIFY(send(q,shortened,2)==AA_LOG_INVALID_UTF8);VERIFY(send(q,surrogate,3)==AA_LOG_INVALID_UTF8);VERIFY(send(q,too_large,4)==AA_LOG_INVALID_UTF8);VERIFY(!pop(q,record));
    auto stats=q.statistics();VERIFY(stats.accepted_messages==Queue::capacity+4);VERIFY(stats.full_messages==1);VERIFY(stats.rejected_messages==9);VERIFY(stats.full_bytes==4);VERIFY(stats.attempted_bytes==3*Queue::capacity+4+AA_LOG_MAX_MESSAGE_BYTES+sizeof(utf8)-1+3);
    q.close();VERIFY(send(q,"closed",6)==AA_LOG_CLOSED);VERIFY(q.statistics().closed_messages==1);VERIFY(!pop(q,record));
    // One producer/consumer per instance. Two instances may run concurrently.
    Queue first,second;constexpr unsigned loops=10000;
    auto produce=[&](Queue& queue,const char* message){for(unsigned n=0;n<loops;++n){uint32_t code;do{code=send(queue,message,5);if(code==AA_LOG_QUEUE_FULL)std::this_thread::yield();}while(code==AA_LOG_QUEUE_FULL);if(code!=AA_LOG_OK)std::abort();}};
    std::thread left(produce,std::ref(first),"first"),right(produce,std::ref(second),"other");
    unsigned got_first=0,got_second=0;uint64_t seq_first=0,seq_second=0;
    while(got_first<loops||got_second<loops){if(pop(first,record)){VERIFY(record.sequence>seq_first);seq_first=record.sequence;VERIFY(record.message_bytes==5);VERIFY(std::memcmp(record.message,"first",5)==0);++got_first;}if(pop(second,record)){VERIFY(record.sequence>seq_second);seq_second=record.sequence;VERIFY(record.message_bytes==5);VERIFY(std::memcmp(record.message,"other",5)==0);++got_second;}}
    left.join();right.join();const auto first_stats=first.statistics(),second_stats=second.statistics();VERIFY(first_stats.accepted_messages==loops);VERIFY(second_stats.accepted_messages==loops);VERIFY(seq_first==loops+first_stats.full_messages);VERIFY(seq_second==loops+second_stats.full_messages);VERIFY(first_stats.full_bytes==5*first_stats.full_messages);VERIFY(second_stats.full_bytes==5*second_stats.full_messages);VERIFY(first_stats.attempted_bytes==5*(loops+first_stats.full_messages));VERIFY(second_stats.attempted_bytes==5*(loops+second_stats.full_messages));VERIFY(!pop(first,record));VERIFY(!pop(second,record));VERIFY(allocations.load()==0);
    std::cout<<"{\"status\":\""<<(failures?"failed":"passed")<<"\",\"failures\":"<<failures<<",\"callback_pop_allocations\":"<<allocations.load()<<",\"capacity\":"<<Queue::capacity<<",\"max_message_bytes\":"<<AA_LOG_MAX_MESSAGE_BYTES<<",\"basic_accepted\":"<<stats.accepted_messages<<",\"basic_full\":"<<stats.full_messages<<",\"basic_rejected\":"<<stats.rejected_messages<<",\"concurrent_delivered\":"<<got_first+got_second<<"}\n";
    return failures?1:0;
}
