#include "audio_algorithms/ChannelSwap.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

TEST_CASE("AR-12.c ChannelSwap block matrix follows an independent stereo permutation oracle", "[AR-12.c][R4][pure]") {
    for (const auto rate : {16000u, 48000u})
        for (const auto frames : {0u, 1u, 479u, 480u, 481u, 511u, 512u, 513u, 16037u}) {
            audioalgo::AudioBuffer input(rate, {"left", "right"}, frames);
            for (std::size_t i = 0; i < frames; ++i) {
                input.mutable_channel(0)[i] = static_cast<float>(0.125 + i * 0.00001);
                input.mutable_channel(1)[i] = static_cast<float>(-0.25 - i * 0.00002);
            }
            const auto before_left = std::vector<float>(input.channel(0).begin(), input.channel(0).end());
            const auto before_right = std::vector<float>(input.channel(1).begin(), input.channel(1).end());
            for (const bool enabled : {false, true}) {
                const auto complete = audioalgo::apply_channel_swap(input, enabled);
                REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(complete));
                const auto& whole = std::get<audioalgo::AudioBuffer>(complete);
                CHECK(whole.channel_ids() == input.channel_ids());
                CHECK(whole.valid_interval() == input.valid_interval());
                for (const auto host : {1u, 127u, 256u, 480u, 512u, 1024u}) {
                    audioalgo::ChannelSwapProcessor processor;
                    REQUIRE(std::holds_alternative<std::monostate>(processor.prepare({enabled}, input.format())));
                    audioalgo::AudioBuffer output(rate, input.channel_ids(), frames);
                    for (std::size_t first = 0; first < frames; first += host) {
                        const auto count = std::min<std::size_t>(host, frames - first);
                        std::array<audioalgo::AudioChannelView, 2> in;
                        std::array<audioalgo::MutableAudioChannelView, 2> out;
                        for (std::size_t c = 0; c < 2; ++c) {
                            in[c] = {rate, c, input.channel(c).subspan(first, count), {0, count}};
                            out[c] = {rate, c, output.mutable_channel(c).subspan(first, count), {0, count}};
                        }
                        const auto result = processor.process(in, out);
                        REQUIRE(std::holds_alternative<audioalgo::ChannelSwapProcessResult>(result));
                        CHECK(std::get<audioalgo::ChannelSwapProcessResult>(result).processed_frames == count);
                    }
                    for (std::size_t c = 0; c < 2; ++c) {
                        const auto expected = input.channel(enabled ? 1 - c : c);
                        CHECK(std::equal(output.channel(c).begin(), output.channel(c).end(), expected.begin(), expected.end()));
                        CHECK(std::equal(whole.channel(c).begin(), whole.channel(c).end(), expected.begin(), expected.end()));
                    }
                }
            }
            CHECK(std::equal(input.channel(0).begin(), input.channel(0).end(), before_left.begin(), before_left.end()));
            CHECK(std::equal(input.channel(1).begin(), input.channel(1).end(), before_right.begin(), before_right.end()));
        }
}

TEST_CASE("AR-12.c ChannelSwap instances reset format errors and aliased views are isolated", "[AR-12.c][R4][pure]") {
    audioalgo::ChannelSwapProcessor swapped, identity;
    REQUIRE(std::holds_alternative<std::monostate>(swapped.prepare({true}, {48000, 2})));
    REQUIRE(std::holds_alternative<std::monostate>(identity.prepare({false}, {48000, 2})));
    audioalgo::AudioBuffer input(48000, {"L", "R"}, 3), output(48000, {"L", "R"}, 3);
    input.mutable_channel(0)[0] = 0.25f;
    input.mutable_channel(1)[0] = -0.5f;
    std::array<audioalgo::AudioChannelView,2> in{input.channel_view(0), input.channel_view(1)};
    std::array<audioalgo::MutableAudioChannelView,2> out{output.mutable_channel_view(0), output.mutable_channel_view(1)};
    REQUIRE(std::holds_alternative<audioalgo::ChannelSwapProcessResult>(swapped.process(in, out)));
    CHECK(output.channel(0)[0] == -0.5f);
    CHECK(output.channel(1)[0] == 0.25f);
    REQUIRE(std::holds_alternative<audioalgo::ChannelSwapProcessResult>(identity.process(in, out)));
    CHECK(output.channel(0)[0] == 0.25f);
    swapped.reset(audioalgo::ChannelSwapResetReason::new_run);
    CHECK(std::get<audioalgo::Diagnostic>(swapped.process(in, out)).code == "not_prepared");
    CHECK(identity.is_prepared());
    for (const auto channels : {1u, 4u})
        CHECK(std::holds_alternative<audioalgo::Diagnostic>(swapped.prepare({true}, {48000, channels})));
    REQUIRE(std::holds_alternative<std::monostate>(swapped.prepare({true}, {48000, 2})));
    std::array<audioalgo::MutableAudioChannelView,2> aliased{input.mutable_channel_view(0), input.mutable_channel_view(1)};
    CHECK(std::get<audioalgo::Diagnostic>(swapped.process(in, aliased)).code == "aliased_audio_view");
    input.mutable_channel(1)[2] = std::numeric_limits<float>::infinity();
    CHECK(std::get<audioalgo::Diagnostic>(swapped.process(in, out)).code == "non_finite_audio");
}
