#include "audio_algorithms/Gain.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace audioalgo;

TEST_CASE("AR-04.d Gain prepare/process/reset uses borrowed block views", "[AR-04.d][pure]") {
    static_assert(std::is_trivially_copyable_v<AudioChannelView>);
    static_assert(std::is_trivially_copyable_v<MutableAudioChannelView>);

    GainProcessor processor;
    const GainConfig config{6.020599913279624};
    REQUIRE(std::holds_alternative<std::monostate>(processor.prepare(config, AudioFormat{48000, 1})));

    AudioBuffer input(48000, {"mono"}, 3, AudioInterval{1, 1});
    AudioBuffer output(48000, {"mono"}, 3, AudioInterval{1, 1});
    input.mutable_channel(0)[0] = 0.25f;
    input.mutable_channel(0)[1] = -0.5f;
    input.mutable_channel(0)[2] = 0.125f;
    std::fill(output.mutable_channel(0).begin(), output.mutable_channel(0).end(), 9.0f);

    const auto input_view = input.channel_view(0);
    auto output_view = output.mutable_channel_view(0);
    REQUIRE(input_view.samples.data() == input.channel(0).data());
    REQUIRE(output_view.samples.data() == output.mutable_channel(0).data());
    const auto processed = processor.process(input_view, output_view);
    REQUIRE(std::holds_alternative<GainProcessResult>(processed));
    REQUIRE(std::get<GainProcessResult>(processed).valid_interval == AudioInterval{1, 1});
    REQUIRE(output.channel(0)[0] == 0.5f);
    REQUIRE(output.channel(0)[1] == -1.0f);
    REQUIRE(output.channel(0)[2] == 0.25f);
    REQUIRE(input.channel(0)[0] == 0.25f);

    processor.reset(GainResetReason::new_run);
    const auto after_reset = processor.process(input_view, output_view);
    REQUIRE(std::holds_alternative<Diagnostic>(after_reset));
    REQUIRE(std::get<Diagnostic>(after_reset).code == "not_prepared");

    GainProcessor invalid;
    const auto rejected = invalid.prepare(GainConfig{12.01}, AudioFormat{48000, 1});
    REQUIRE(std::holds_alternative<Diagnostic>(rejected));
    REQUIRE(std::get<Diagnostic>(rejected).code == "parameter_out_of_range");
    REQUIRE(std::holds_alternative<Diagnostic>(invalid.process(input_view, output_view)));

    REQUIRE(std::holds_alternative<std::monostate>(processor.prepare(GainConfig{-6.0}, AudioFormat{48000, 1})));
    AudioBuffer second_input(48000, {"mono"}, 1);
    AudioBuffer second_output(48000, {"mono"}, 1);
    second_input.mutable_channel(0)[0] = 1.0f;
    const auto second_result = processor.process(second_input.channel_view(0), second_output.mutable_channel_view(0));
    REQUIRE(std::holds_alternative<GainProcessResult>(second_result));
    REQUIRE(std::abs(second_output.channel(0)[0] - 0.5011872f) < 1e-6f);
}

TEST_CASE("AR-05.a Gain APIs match a float64 oracle across the full block matrix", "[AR-05.a][G1][DSP-01]") {
    constexpr std::array<std::size_t, 8> lengths{0, 1, 127, 256, 511, 512, 513, 16037};
    constexpr std::array<std::size_t, 3> channel_counts{1, 2, 4};
    constexpr std::array<double, 5> gains{-60.0, -12.0, 0.0, 6.020599913279624, 12.0};

    for (const auto gain_db : gains) {
        const double scale = std::pow(10.0, gain_db / 20.0);
        for (const auto channel_count : channel_counts) {
            for (const auto frames : lengths) {
                std::vector<std::string> ids;
                for (std::size_t channel = 0; channel < channel_count; ++channel)
                    ids.push_back("channel-" + std::to_string(channel));
                const auto first = std::min<std::size_t>(1, frames);
                AudioBuffer input(48000, ids, frames, AudioInterval{first, frames - first});
                AudioBuffer block_output(48000, ids, frames, input.valid_interval());
                for (std::size_t channel = 0; channel < channel_count; ++channel) {
                    auto samples = input.mutable_channel(channel);
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        const double phase = static_cast<double>((channel + 1) * (frame + 1)) / 97.0;
                        samples[frame] = static_cast<float>(0.31 * std::sin(phase) - 0.07 * std::cos(phase * 0.37));
                    }
                }
                const AudioBuffer original = input;

                const auto convenience = apply_gain_db(input, gain_db);
                REQUIRE(std::holds_alternative<AudioBuffer>(convenience));
                const auto& convenience_output = std::get<AudioBuffer>(convenience);

                GainProcessor processor;
                REQUIRE(std::holds_alternative<std::monostate>(processor.prepare(
                    GainConfig{gain_db}, AudioFormat{48000, channel_count})));
                for (std::size_t channel = 0; channel < channel_count; ++channel) {
                    const auto processed = processor.process(input.channel_view(channel),
                                                             block_output.mutable_channel_view(channel));
                    REQUIRE(std::holds_alternative<GainProcessResult>(processed));
                    REQUIRE(std::get<GainProcessResult>(processed).valid_interval == input.valid_interval());
                    REQUIRE(std::get<GainProcessResult>(processed).processed_frames == frames);
                }

                REQUIRE(convenience_output.sample_rate_hz() == input.sample_rate_hz());
                REQUIRE(convenience_output.channel_ids() == input.channel_ids());
                REQUIRE(convenience_output.valid_interval() == input.valid_interval());
                for (std::size_t channel = 0; channel < channel_count; ++channel) {
                    const auto source = original.channel(channel);
                    const auto convenience_samples = convenience_output.channel(channel);
                    const auto block_samples = block_output.channel(channel);
                    REQUIRE(std::equal(input.channel(channel).begin(), input.channel(channel).end(),
                                       original.channel(channel).begin(), original.channel(channel).end()));
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        const double expected = static_cast<double>(source[frame]) * scale;
                        const double tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
                        REQUIRE(std::abs(static_cast<double>(convenience_samples[frame]) - expected) <= tolerance);
                        REQUIRE(std::abs(static_cast<double>(block_samples[frame]) - expected) <= tolerance);
                    }
                }
            }
        }
    }
}
