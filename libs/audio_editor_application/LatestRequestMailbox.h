#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace audioalgo {
// Value-only, thread-safe latest-request handoff. At most one active token and
// one pending payload; retired payloads are destroyed after unlocking. Worker
// tickets prevent a finishing worker from clearing a newly scheduled worker.
template<typename Request>
class LatestRequestMailbox final {
public:
    using Ticket=std::uint64_t;
    struct Entry {Request request;std::shared_ptr<std::atomic<bool>> cancelled;};
    struct Submission {bool accepted=false,start_worker=false;Ticket ticket=0;};
    struct Counts {std::size_t active=0,pending=0;bool worker_claimed=false;};
    Submission submit(Request request) {
        auto token=std::make_shared<std::atomic<bool>>(false);
        std::optional<Entry> retired;
        Submission result;
        {
            std::lock_guard lock(mutex_);
            if(closed_)return {};
            if(active_)active_->store(true);
            if(pending_) {pending_->cancelled->store(true);retired=std::move(pending_);}
            pending_.emplace(Entry{std::move(request),std::move(token)});
            if(!worker_claimed_) {worker_claimed_=true;++ticket_;result.start_worker=true;}
            result.accepted=true;result.ticket=ticket_;
        }
        return result;
    }
    std::optional<Entry> take(Ticket ticket) {
        std::lock_guard lock(mutex_);
        if(ticket!=ticket_)return {};
        if(!pending_) {active_.reset();worker_claimed_=false;return {};}
        auto next=std::move(pending_);pending_.reset();active_=next->cancelled;return next;
    }
    void cancel(bool close=false) noexcept {
        std::optional<Entry> retired;
        {
            std::lock_guard lock(mutex_);
            closed_|=close;
            if(active_)active_->store(true);
            if(pending_) {pending_->cancelled->store(true);retired=std::move(pending_);pending_.reset();}
        }
    }
    void abandon(Ticket ticket) noexcept {
        std::optional<Entry> retired;
        {
            std::lock_guard lock(mutex_);
            if(ticket!=ticket_)return;
            if(active_)active_->store(true);
            active_.reset();worker_claimed_=false;
            if(pending_) {pending_->cancelled->store(true);retired=std::move(pending_);pending_.reset();}
        }
    }
    Counts counts() const {std::lock_guard lock(mutex_);return {active_?1u:0u,pending_?1u:0u,worker_claimed_};}
private:
    mutable std::mutex mutex_;
    std::optional<Entry> pending_;
    std::shared_ptr<std::atomic<bool>> active_;
    Ticket ticket_=0;
    bool worker_claimed_=false,closed_=false;
};
}
