#include "AudioTimeline.h"
#include <limits>
#include <stdexcept>

namespace audioalgo {
std::optional<std::uint64_t> map_sample_frame(std::uint64_t position,
    std::uint32_t source_rate, std::uint32_t target_rate) noexcept {
    if(source_rate==0 || target_rate==0)return {};
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    const auto whole=position/source_rate;
    if(whole>maximum/target_rate)return {};
    const auto base=whole*target_rate;
    // Both rates are uint32; this remainder product always fits uint64.
    const auto fraction=(position%source_rate)*target_rate/source_rate;
    if(fraction>maximum-base)return {};
    return base+fraction;
}

std::optional<AudioInterval> map_sample_interval(AudioInterval interval,
    std::uint32_t source_rate, std::uint32_t target_rate) noexcept {
    const auto maximum=std::numeric_limits<std::size_t>::max();
    if(interval.frame_count>maximum-interval.first_frame)return {};
    const auto first=map_sample_frame(interval.first_frame,source_rate,target_rate);
    const auto end=map_sample_frame(interval.first_frame+interval.frame_count,source_rate,target_rate);
    if(!first || !end || *first>maximum || *end>maximum)return {};
    return AudioInterval{static_cast<std::size_t>(*first),static_cast<std::size_t>(*end-*first)};
}

SampleFrameClock::SampleFrameClock(std::uint32_t source_rate,std::uint32_t target_rate)
    :source_rate_(source_rate),target_rate_(target_rate) {
    if(source_rate==0 || target_rate==0)throw std::invalid_argument("Sample clock rates must be positive");
}
bool SampleFrameClock::advance(std::uint64_t frames) noexcept {
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    if(frames>maximum-source_frames_)return false;
    const auto whole=frames/source_rate_;
    if(whole>maximum/target_rate_)return false;
    const auto base=whole*target_rate_;
    const auto numerator=(frames%source_rate_)*target_rate_+remainder_;
    const auto extra=numerator/source_rate_;
    if(extra>maximum-base || base+extra>maximum-target_frames_)return false;
    source_frames_+=frames;target_frames_+=base+extra;remainder_=numerator%source_rate_;
    return true;
}
}
