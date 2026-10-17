#include "audio_algorithms/Gain.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <type_traits>

using audioalgo::AudioBuffer;
using audioalgo::AudioInterval;
using audioalgo::Diagnostic;
using audioalgo::apply_gain_db;

TEST_CASE("CORE-01 planar buffers preserve lengths, channel IDs and valid intervals", "[G1][CORE-01]") {
    AudioBuffer empty(16000, {"mic"}, 0);
    const AudioInterval empty_interval{0, 0};
    const AudioInterval partial_interval{5, 501};
    REQUIRE(empty.format_error().empty());
    REQUIRE(empty.frame_count() == 0);
    REQUIRE(empty.channel_count() == 1);
    REQUIRE(empty.valid_interval() == empty_interval);

    AudioBuffer stereo(48000, {"left", "right"}, 513, {5, 501});
    REQUIRE(stereo.format_error().empty());
    REQUIRE(stereo.valid_interval() == partial_interval);
    REQUIRE(stereo.valid_interval().first_frame + stereo.valid_interval().frame_count == 506);
    REQUIRE(stereo.channel(0).size() == 513);
    REQUIRE(stereo.channel(1).size() == 513);
    REQUIRE(stereo.channel(0).data() != stereo.channel(1).data());

    stereo.mutable_channel(0)[0] = 0.25f;
    stereo.mutable_channel(1)[0] = -0.5f;
    const AudioBuffer& readonly = stereo;
    static_assert(std::is_const_v<std::remove_reference_t<decltype(readonly.channel(0)[0])>>);
    REQUIRE(readonly.channel(0)[0] == 0.25f);
    REQUIRE(readonly.channel(1)[0] == -0.5f);

    const auto gain = apply_gain_db(stereo, 6.0);
    REQUIRE(std::holds_alternative<AudioBuffer>(gain));
    const auto& gained = std::get<AudioBuffer>(gain);
    REQUIRE(gained.valid_interval() == stereo.valid_interval());
    REQUIRE(stereo.channel(0)[0] == 0.25f);
    REQUIRE(stereo.channel(1)[0] == -0.5f);
}

TEST_CASE("CORE-01 invalid formats and valid intervals return structured diagnostics", "[G1][CORE-01]") {
    REQUIRE_FALSE(AudioBuffer(44100, {"mono"}, 16).format_error().empty());
    REQUIRE_FALSE(AudioBuffer(48000, {}, 16).format_error().empty());
    REQUIRE_FALSE(AudioBuffer(48000, {"left", "left"}, 16).format_error().empty());
    REQUIRE_FALSE(AudioBuffer(48000, {""}, 16).format_error().empty());
    REQUIRE_FALSE(AudioBuffer(48000, {"mono"}, 16, {15, 2}).format_error().empty());
    REQUIRE_FALSE(AudioBuffer(48000, {"mono"}, 16, {17, 0}).format_error().empty());

    const auto invalid_gain = apply_gain_db(AudioBuffer(48000, {"mono"}, 16, {15, 2}), 0.0);
    REQUIRE(std::holds_alternative<Diagnostic>(invalid_gain));
    REQUIRE(std::get<Diagnostic>(invalid_gain).code == "invalid_audio_format");
    REQUIRE(std::holds_alternative<Diagnostic>(apply_gain_db(AudioBuffer(48000, {"mono"}, 16), 12.1)));
}
