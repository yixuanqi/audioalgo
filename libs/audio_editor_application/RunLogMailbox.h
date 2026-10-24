#pragma once
#include "audio_run_api/NodeLogEvent.h"
#include "audio_core/Utf8LogDecoder.h"
#include <deque>
#include <mutex>
#include <stdexcept>

namespace audioalgo::editor {
// Owned pending values. The GUI schedules one drain for an outstanding batch.
class RunLogMailbox {
public:
    struct Counts { std::size_t bytes=0, events=0; bool scheduled=false; };
    static std::size_t weight(const NodeLogEvent& e) {
        return sizeof(e)+e.identity.project_id.size()+e.identity.request_id.size()+e.node_id.size()+e.type_id.size()+e.version.size()+e.package_id.size()+
            e.content_sha256.size()+e.source.size()+e.level.size()+e.text.size()+e.report_file.size()+e.raw_log_file.size();
    }
    bool push(NodeLogEvent event) {
        std::scoped_lock lock(mutex_);
        constexpr std::size_t limit=256*1024;
        const auto metadata=weight(event)-event.text.size();
        if(metadata>=limit)throw std::runtime_error("run_log_metadata_limit: log identity exceeds pending delivery budget");
        event.ui_dropped_text_bytes+=trim_log_text(event.text,limit-metadata);
        while(!events_.empty() && (events_.size()>=128 || bytes_+weight(event)>limit)) {
            const auto& retired=events_.front();bytes_-=weight(retired);
            event.ui_dropped_text_bytes+=retired.text.size()+retired.ui_dropped_text_bytes;
            event.ui_dropped_events+=1+retired.ui_dropped_events;events_.pop_front();
        }
        const bool schedule=!scheduled_;scheduled_=true;
        bytes_+=weight(event);events_.push_back(std::move(event));return schedule;
    }
    std::deque<NodeLogEvent> take() {
        std::scoped_lock lock(mutex_);auto result=std::move(events_);events_.clear();bytes_=0;scheduled_=false;return result;
    }
    Counts counts() const {std::scoped_lock lock(mutex_);return {bytes_,events_.size(),scheduled_};}
private:
    mutable std::mutex mutex_;
    std::deque<NodeLogEvent> events_;
    std::size_t bytes_=0;
    bool scheduled_=false;
};
}
