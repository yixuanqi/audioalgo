#include "audio_core/AudioTimeline.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>

TEST_CASE("AR-15.f integer sample clocks map ten minutes across 16 and 48 kHz without drift","[AR-15.f][CORE-02]") {
    for(const auto source:{16000u,48000u})for(const auto target:{16000u,48000u})for(const auto block:{1u,127u,480u,512u,1024u,0u}) {
        INFO("source="<<source<<" target="<<target<<" block="<<block);
        audioalgo::SampleFrameClock clock(source,target);std::mt19937 random(0x20261023);
        const std::uint64_t total=static_cast<std::uint64_t>(source)*600;
        bool valid=true;std::uint64_t maximum_error=0;
        while(clock.source_frames()<total) {
            const auto count=std::min<std::uint64_t>(block?block:1+random()%1024,total-clock.source_frames());
            if(!clock.advance(count)){valid=false;break;}
            // Independent absolute reference; the ten-minute product fits uint64.
            const auto absolute=clock.source_frames()*target/source;
            maximum_error=std::max(maximum_error,clock.target_frames()>absolute?clock.target_frames()-absolute:absolute-clock.target_frames());
        }
        CHECK(valid);CHECK(maximum_error==0);CHECK(clock.target_frames()==static_cast<std::uint64_t>(target)*600);
        CHECK(clock.remainder()==0);clock.reset();CHECK(clock.source_frames()==0);CHECK(clock.target_frames()==0);
        CHECK(clock.advance(1));CHECK(clock.target_frames()==target/source);
    }
}

TEST_CASE("AR-15.f invalid rates overflow and interval endpoints fail transactionally","[AR-15.f][CORE-02]") {
    using audioalgo::map_sample_frame;
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    CHECK_FALSE(map_sample_frame(1,0,48000));CHECK_FALSE(map_sample_frame(1,16000,0));
    CHECK_THROWS_AS(audioalgo::SampleFrameClock(0,16000),std::invalid_argument);
    CHECK(map_sample_frame(maximum,48000,48000)==maximum);
    CHECK_FALSE(map_sample_frame(maximum,16000,48000));
    audioalgo::SampleFrameClock clock(16000,48000);REQUIRE(clock.advance(5));
    CHECK_FALSE(clock.advance(maximum));CHECK(clock.source_frames()==5);CHECK(clock.target_frames()==15);
    audioalgo::SampleFrameClock down(48000,16000);REQUIRE(down.advance(maximum));
    CHECK_FALSE(down.advance(1));CHECK(down.source_frames()==maximum);CHECK(down.target_frames()==maximum/3);
    CHECK(audioalgo::map_sample_interval({1,2},48000,16000)==audioalgo::AudioInterval{0,1});
    CHECK(audioalgo::map_sample_interval({0,0},48000,16000)==audioalgo::AudioInterval{0,0});
    CHECK_FALSE(audioalgo::map_sample_interval({maximum,1},48000,16000));
    CHECK(map_sample_frame(1477,48000,1000000000)==30770833);
}
