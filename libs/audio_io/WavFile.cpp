#include "audio_io/WavFile.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

namespace audioalgo {
namespace {

juce::File to_juce_file(const std::filesystem::path& path) {
    const auto utf8_path = path.u8string();
    const std::string bytes(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
    return juce::File(juce::String::fromUTF8(bytes.c_str()));
}

Diagnostic io_error(const std::string& code, const juce::String& detail) {
    return {code, detail.toStdString()};
}

} // namespace

AudioReadOutcome read_wav_file(const std::filesystem::path& path) {
    const auto file = to_juce_file(path);
    if (!file.existsAsFile())
        return Diagnostic{"audio_file_missing", "Audio file does not exist: " + file.getFullPathName().toStdString()};

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader)
        return Diagnostic{"audio_format_unsupported", "File is not a readable WAV/AIFF audio file"};
    if (!reader->getFormatName().equalsIgnoreCase("WAV file"))
        return Diagnostic{"audio_format_unsupported", "Only WAV audio files are supported"};
    if (!((reader->bitsPerSample == 16 && !reader->usesFloatingPointData) ||
          (reader->bitsPerSample == 32 && reader->usesFloatingPointData)))
        return Diagnostic{"audio_encoding_unsupported", "WAV must contain PCM16 or float32 samples"};
    if (reader->sampleRate != 16000.0 && reader->sampleRate != 48000.0)
        return Diagnostic{"sample_rate_unsupported", "WAV sample rate must be 16000 Hz or 48000 Hz"};
    if (reader->numChannels != 1 && reader->numChannels != 2 && reader->numChannels != 4)
        return Diagnostic{"channel_count_unsupported", "WAV must contain 1, 2, or 4 channels"};
    if (reader->lengthInSamples < 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        return Diagnostic{"audio_length_unsupported", "WAV sample count exceeds the supported range"};

    const auto channels = static_cast<std::size_t>(reader->numChannels);
    const auto frames = static_cast<std::size_t>(reader->lengthInSamples);
    std::vector<std::string> channel_ids;
    channel_ids.reserve(channels);
    for (std::size_t channel = 0; channel < channels; ++channel)
        channel_ids.push_back("ch" + std::to_string(channel));

    AudioBuffer audio(static_cast<std::uint32_t>(reader->sampleRate), std::move(channel_ids), frames);
    std::array<float*, 4> destination{};
    for (std::size_t channel = 0; channel < channels; ++channel)
        destination[channel] = audio.mutable_channel(channel).data();
    if (frames != 0 && !reader->read(destination.data(), static_cast<int>(channels), 0, static_cast<int>(frames)))
        return Diagnostic{"audio_read_failed", "The WAV reader could not decode all samples"};
    return audio;
}

AudioWriteOutcome write_wav_file(const std::filesystem::path& path, const AudioBuffer& audio,
                                WavEncoding encoding, bool allow_overwrite) {
    if (const auto format_error = audio.format_error(); !format_error.empty())
        return Diagnostic{"invalid_audio_format", format_error};
    if (audio.frame_count() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Diagnostic{"audio_length_unsupported", "Audio sample count exceeds the WAV writer limit"};

    const auto output = to_juce_file(path);
    if (output.existsAsFile() && !allow_overwrite)
        return Diagnostic{"output_exists", "Refusing to overwrite an existing audio file"};
    if (!output.getParentDirectory().isDirectory())
        return Diagnostic{"output_directory_missing", "Output directory does not exist"};

    const auto temp_path = path.parent_path() / (path.filename().u8string() + u8".tmp-" +
                                                  std::filesystem::path(juce::Uuid().toString().toStdString()).u8string());
    const auto temp_file = to_juce_file(temp_path);
    auto stream = std::make_unique<juce::FileOutputStream>(temp_file);
    if (!stream->openedOk())
        return Diagnostic{"audio_output_open_failed", "Could not create temporary WAV output"};

    juce::WavAudioFormat wav;
    const int bits = encoding == WavEncoding::pcm16 ? 16 : 32;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(
        stream.get(), audio.sample_rate_hz(), static_cast<unsigned int>(audio.channel_count()), bits, {}, 0));
    if (!writer)
        return Diagnostic{"audio_writer_unavailable", "The WAV writer rejected the requested format"};
    stream.release(); // AudioFormatWriter owns the output stream after successful creation.

    std::array<const float*, 4> planes{};
    std::size_t clipped = 0;
    for (std::size_t channel = 0; channel < audio.channel_count(); ++channel) {
        const auto samples = audio.channel(channel);
        planes[channel] = samples.data();
        if (encoding == WavEncoding::pcm16) {
            for (const float sample : samples)
                if (sample < -1.0f || sample > 1.0f) ++clipped;
        }
    }
    if (audio.frame_count() != 0 && !writer->writeFromFloatArrays(planes.data(), static_cast<int>(audio.channel_count()), static_cast<int>(audio.frame_count()))) {
        writer.reset();
        temp_file.deleteFile();
        return Diagnostic{"audio_write_failed", "The WAV writer failed while writing samples"};
    }
    writer->flush();
    writer.reset();

    std::error_code error;
    if (allow_overwrite && output.existsAsFile()) {
        if (!temp_file.replaceFileIn(output))
            return Diagnostic{"audio_publish_failed", "Could not atomically replace the existing output WAV"};
    } else {
        std::filesystem::rename(temp_path, path, error);
        if (error) {
            temp_file.deleteFile();
            return Diagnostic{"audio_publish_failed", "Could not atomically publish WAV: " + error.message()};
        }
    }
    return WavWriteSummary{audio.frame_count(), audio.channel_count(), clipped};
}

} // namespace audioalgo
