#include "audio_algorithms/Gain.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using audioalgo::AudioBuffer;
using audioalgo::Diagnostic;
using audioalgo::apply_gain_db;

namespace {
std::vector<std::string> channel_ids(std::size_t count) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < count; ++i)
        ids.push_back("ch" + std::to_string(i));
    return ids;
}
} // namespace

TEST_CASE("DSP-01 Gain matches an independent float64 oracle", "[G1][DSP-01]") {
    constexpr std::array<std::size_t, 9> lengths{0, 1, 479, 480, 481, 511, 512, 513, 16037};
    constexpr std::array<std::size_t, 3> channel_counts{1, 2, 4};
    constexpr double gain_db = 6.020599913279624;
    const double scale = std::pow(10.0, gain_db / 20.0);

    for (const auto channel_count : channel_counts) {
        for (const auto frames : lengths) {
            AudioBuffer input(48000, channel_ids(channel_count), frames);
            for (std::size_t ch = 0; ch < channel_count; ++ch) {
                auto samples = input.mutable_channel(ch);
                for (std::size_t i = 0; i < frames; ++i)
                    samples[i] = static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * (ch + 1) * i / 48000.0));
            }
            const AudioBuffer original = input;
            const auto outcome = apply_gain_db(input, gain_db);
            const auto* output = std::get_if<AudioBuffer>(&outcome);
            REQUIRE(output != nullptr);
            REQUIRE(output->sample_rate_hz() == input.sample_rate_hz());
            REQUIRE(output->channel_count() == channel_count);
            REQUIRE(output->frame_count() == frames);
            REQUIRE(output->channel_ids() == input.channel_ids());

            for (std::size_t ch = 0; ch < channel_count; ++ch) {
                const auto source = original.channel(ch);
                const auto actual = output->channel(ch);
                REQUIRE(actual.size() == source.size());
                for (std::size_t i = 0; i < frames; ++i) {
                    const double expected = static_cast<double>(source[i]) * scale;
                    const double tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
                    REQUIRE(std::abs(static_cast<double>(actual[i]) - expected) <= tolerance);
                }
                REQUIRE(std::equal(input.channel(ch).begin(), input.channel(ch).end(), original.channel(ch).begin(), original.channel(ch).end()));
            }
        }
    }
}

TEST_CASE("DSP-01 Gain rejects malformed formats and parameters", "[G1][DSP-01]") {
    AudioBuffer valid(48000, {"L"}, 1);
    REQUIRE(std::holds_alternative<AudioBuffer>(apply_gain_db(valid, 0.0)));
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(valid, -60.1)));
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(valid, 12.1)));
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(valid, std::nan(""))));
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(AudioBuffer(44100, {"L"}, 1), 0.0)));
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(AudioBuffer(48000, {"L", "L"}, 1), 0.0)));
}
