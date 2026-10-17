#include "audio_io/WavFile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using audioalgo::AudioBuffer;
using audioalgo::Diagnostic;
using audioalgo::WavEncoding;
using audioalgo::WavWriteSummary;
using audioalgo::read_wav_file;
using audioalgo::write_wav_file;

namespace {
std::filesystem::path make_test_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() / std::filesystem::path(L"AudioAlgo 中文 IO") /
                     ("case-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("IO-01 WAV PCM16 and float32 round trips preserve shape and values", "[G1][IO-01]") {
    const auto directory = make_test_directory();
    const auto pcm_path = directory / std::filesystem::path(L"PCM16 波形.wav");
    const auto float_path = directory / std::filesystem::path(L"Float32 test.wav");

    AudioBuffer input(48000, {"mic-left", "mic-right"}, 481);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        auto samples = input.mutable_channel(channel);
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            samples[frame] = static_cast<float>(0.7 * std::sin(2.0 * 3.141592653589793 * (channel + 1) * frame / 48000.0));
    }

    const auto pcm_result = write_wav_file(pcm_path, input, WavEncoding::pcm16);
    const auto* summary = std::get_if<WavWriteSummary>(&pcm_result);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->frames == 481);
    REQUIRE(summary->channels == 2);
    REQUIRE(summary->clipped_samples == 0);

    const auto pcm_read = read_wav_file(pcm_path);
    if (const auto* error = std::get_if<Diagnostic>(&pcm_read))
        FAIL_CHECK(error->code + ": " + error->message);
    const auto* pcm_audio = std::get_if<AudioBuffer>(&pcm_read);
    REQUIRE(pcm_audio != nullptr);
    REQUIRE(pcm_audio->sample_rate_hz() == input.sample_rate_hz());
    REQUIRE(pcm_audio->channel_count() == input.channel_count());
    REQUIRE(pcm_audio->frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        const auto expected = input.channel(channel);
        const auto actual = pcm_audio->channel(channel);
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            REQUIRE(std::abs(actual[frame] - expected[frame]) <= (1.0 / 32768.0) + 1.0e-6);
    }

    const auto float_result = write_wav_file(float_path, input, WavEncoding::float32);
    REQUIRE(std::holds_alternative<WavWriteSummary>(float_result));
    const auto float_read = read_wav_file(float_path);
    const auto* float_audio = std::get_if<AudioBuffer>(&float_read);
    REQUIRE(float_audio != nullptr);
    REQUIRE(float_audio->frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        REQUIRE(std::equal(float_audio->channel(channel).begin(), float_audio->channel(channel).end(),
                           input.channel(channel).begin(), input.channel(channel).end()));

    const auto overwrite_result = write_wav_file(pcm_path, input, WavEncoding::pcm16);
    const auto* overwrite_error = std::get_if<Diagnostic>(&overwrite_result);
    REQUIRE(overwrite_error != nullptr);
    REQUIRE(overwrite_error->code == "output_exists");

    std::ofstream(directory / "broken.wav", std::ios::binary) << "invalid";
    REQUIRE(std::holds_alternative<Diagnostic>(read_wav_file(directory / "broken.wav")));
    std::filesystem::remove_all(directory);
}

TEST_CASE("IO-01 PCM16 writes saturate and count clipped samples", "[G1][IO-01]") {
    const auto directory = make_test_directory();
    AudioBuffer input(16000, {"mono"}, 4);
    input.mutable_channel(0)[0] = -1.5f;
    input.mutable_channel(0)[1] = -1.0f;
    input.mutable_channel(0)[2] = 1.0f;
    input.mutable_channel(0)[3] = 1.5f;
    const auto output = directory / "clipped.wav";
    const auto write_result = write_wav_file(output, input, WavEncoding::pcm16);
    const auto* summary = std::get_if<WavWriteSummary>(&write_result);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->clipped_samples == 2);
    const auto read_result = read_wav_file(output);
    if (const auto* error = std::get_if<Diagnostic>(&read_result))
        FAIL_CHECK(error->code + ": " + error->message);
    const auto* decoded = std::get_if<AudioBuffer>(&read_result);
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->channel(0)[0] >= -1.0f);
    REQUIRE(decoded->channel(0)[3] <= 1.0f);
    std::filesystem::remove_all(directory);
}
