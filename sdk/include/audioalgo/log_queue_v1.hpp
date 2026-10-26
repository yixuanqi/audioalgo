#ifndef AUDIOALGO_LOG_QUEUE_V1_HPP
#define AUDIOALGO_LOG_QUEUE_V1_HPP
#include "node_log_abi_v1.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace audioalgo::sdk {
struct LogRecordV1 {
    std::uint64_t sequence=0;
    std::uint32_t level=AA_LOG_INFO, message_bytes=0;
    char message[AA_LOG_MAX_MESSAGE_BYTES]{};
};
struct LogQueueStatisticsV1 {
    std::uint64_t accepted_messages, full_messages, rejected_messages, closed_messages;
    std::uint64_t attempted_bytes, full_bytes;
};

// Fixed-capacity single-producer/single-consumer storage, one queue per instance.
// Construction precedes prepare/process/reset; emit/pop never allocate or lock.
// The consumer runs outside process/reset on a worker. Serialize producers across
// prepare/process/reset even when their calling threads differ. No global sink.
// Statistics are independent monotonic counters, not a concurrent atomic snapshot;
// read after quiescence for exact totals. Failed valid emits consume sequence IDs.
class LogQueueV1 final {
public:
    static constexpr std::uint32_t capacity=64;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    LogQueueV1() noexcept=default;
    LogQueueV1(const LogQueueV1&)=delete;
    LogQueueV1& operator=(const LogQueueV1&)=delete;

    aa_log_sink_v1 sink() noexcept { return {sizeof(aa_log_sink_v1),AA_LOG_ABI_V1,this,&emit_callback}; }
    void close() noexcept { closed_.store(true,std::memory_order_release); }
    bool pop(LogRecordV1& record) noexcept {
        const auto read=read_.load(std::memory_order_relaxed);
        if(read==write_.load(std::memory_order_acquire))return false;
        record=records_[read%capacity];
        read_.store(read+1,std::memory_order_release);
        return true;
    }
    LogQueueStatisticsV1 statistics() const noexcept {
        return {accepted_.load(std::memory_order_relaxed),full_.load(std::memory_order_relaxed),
                rejected_.load(std::memory_order_relaxed),closed_messages_.load(std::memory_order_relaxed),
                attempted_bytes_.load(std::memory_order_relaxed),full_bytes_.load(std::memory_order_relaxed)};
    }
private:
    static bool utf8(const char* message,std::uint32_t size) noexcept {
        std::uint32_t at=0;
        while(at<size){
            const auto first=static_cast<unsigned char>(message[at++]);
            if(first<0x80)continue;
            std::uint32_t rest=0,code=0,minimum=0;
            if(first>=0xc2&&first<=0xdf){rest=1;code=first&0x1f;minimum=0x80;}
            else if(first>=0xe0&&first<=0xef){rest=2;code=first&0x0f;minimum=0x800;}
            else if(first>=0xf0&&first<=0xf4){rest=3;code=first&0x07;minimum=0x10000;}
            else return false;
            if(rest>size-at)return false;
            for(std::uint32_t n=0;n<rest;++n){const auto byte=static_cast<unsigned char>(message[at++]);if((byte&0xc0)!=0x80)return false;code=(code<<6)|(byte&0x3f);}
            if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
        }
        return true;
    }
    static std::uint32_t AA_CALL emit_callback(void* opaque,const aa_log_message_v1* message) noexcept {
        if(!opaque)return AA_LOG_INVALID_ARGUMENT;
        return static_cast<LogQueueV1*>(opaque)->emit(message);
    }
    std::uint32_t reject(std::uint32_t code) noexcept {rejected_.fetch_add(1,std::memory_order_relaxed);return code;}
    std::uint32_t emit(const aa_log_message_v1* message) noexcept {
        if(closed_.load(std::memory_order_acquire)){closed_messages_.fetch_add(1,std::memory_order_relaxed);return AA_LOG_CLOSED;}
        if(!message||message->struct_size!=sizeof(aa_log_message_v1)||message->abi_version!=AA_LOG_ABI_V1||message->level>AA_LOG_ERROR||(!message->message&&message->message_bytes))return reject(AA_LOG_INVALID_ARGUMENT);
        if(message->message_bytes>AA_LOG_MAX_MESSAGE_BYTES)return reject(AA_LOG_MESSAGE_TOO_LONG);
        if(!utf8(message->message,message->message_bytes))return reject(AA_LOG_INVALID_UTF8);
        const auto sequence=++sequence_;
        attempted_bytes_.fetch_add(message->message_bytes,std::memory_order_relaxed);
        const auto write=write_.load(std::memory_order_relaxed);
        if(static_cast<std::uint32_t>(write-read_.load(std::memory_order_acquire))>=capacity){full_.fetch_add(1,std::memory_order_relaxed);full_bytes_.fetch_add(message->message_bytes,std::memory_order_relaxed);return AA_LOG_QUEUE_FULL;}
        auto& record=records_[write%capacity];record.sequence=sequence;record.level=message->level;record.message_bytes=message->message_bytes;
        if(message->message_bytes)std::memcpy(record.message,message->message,message->message_bytes);
        accepted_.fetch_add(1,std::memory_order_relaxed);
        write_.store(write+1,std::memory_order_release);
        return AA_LOG_OK;
    }
    std::array<LogRecordV1,capacity> records_{};
    std::atomic<std::uint32_t> read_{0},write_{0};
    std::atomic<bool> closed_{false};
    std::uint64_t sequence_=0;
    std::atomic<std::uint64_t> accepted_{0},full_{0},rejected_{0},closed_messages_{0},attempted_bytes_{0},full_bytes_{0};
};
}
#endif
