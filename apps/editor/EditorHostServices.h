#pragma once
#include "EditorUiSupport.h"
#include "audio_editor_application/ProjectDocumentService.h"
#include "audio_editor_application/AudioPreviewService.h"
#include "audio_project/ProjectFileIO.h"
#include <juce_audio_utils/juce_audio_utils.h>
namespace audioalgo::editor {
class ProjectFileDocumentService final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path& path) const override {
        return audioalgo::load_project_file(path);
    }

    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path& path,
                                               const audioalgo::ProjectModel& project) const override {
        return audioalgo::save_project_file(path, project);
    }
};

class JuceAudioPreviewService final : public audioalgo::AudioPreviewService {
public:
    JuceAudioPreviewService() { read_ahead_thread_.startThread(); }
    ~JuceAudioPreviewService() override { shutdown(); }

    audioalgo::AudioPreviewOutcome play(const std::filesystem::path& audio_file) override {
        if (shut_down_)
            return audioalgo::Diagnostic{"preview_service_closed", "Audio preview is shutting down"};

        if (reader_source_ == nullptr || current_file_ != audio_file) {
            transport_.stop();
            transport_.setSource(nullptr);
            reader_source_.reset();
            audio_format_manager_.registerBasicFormats();
            auto* reader = audio_format_manager_.createReaderFor(to_juce_file(audio_file));
            if (reader == nullptr)
                return audioalgo::Diagnostic{"preview_file_open_failed", "Could not open the rendered WAV for playback"};
            const auto sample_rate = reader->sampleRate;
            reader_source_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
            transport_.setSource(reader_source_.get(), 32768, &read_ahead_thread_, sample_rate);
            current_file_ = audio_file;
        }

        if (!audio_device_ready_) {
            const auto error = audio_device_manager_.initialise(0, 2, nullptr, true);
            if (error.isNotEmpty()) {
                audio_device_manager_.closeAudioDevice();
                return audioalgo::Diagnostic{"audio_device_unavailable", error.toStdString()};
            }
            audio_device_manager_.addAudioCallback(&audio_source_player_);
            audio_source_player_.setSource(&transport_);
            audio_device_ready_ = true;
        }
        if (transport_.getCurrentPosition() >= transport_.getLengthInSeconds())
            transport_.setPosition(0.0);
        transport_.start();
        return std::monostate{};
    }

    void pause() override { transport_.stop(); }

    void stop() override {
        transport_.stop();
        transport_.setSource(nullptr);
        reader_source_.reset();
        current_file_.clear();
        audio_source_player_.setSource(nullptr);
        if (audio_device_ready_) {
            audio_device_manager_.removeAudioCallback(&audio_source_player_);
            audio_device_manager_.closeAudioDevice();
            audio_device_ready_ = false;
        }
    }

    void shutdown() override {
        if (shut_down_)
            return;
        stop();
        read_ahead_thread_.stopThread(2000);
        shut_down_ = true;
    }

    [[nodiscard]] bool is_playing() const noexcept override { return transport_.isPlaying(); }
    [[nodiscard]] double position_seconds() const noexcept override { return transport_.getCurrentPosition(); }

private:
    juce::AudioDeviceManager audio_device_manager_;
    juce::AudioSourcePlayer audio_source_player_;
    juce::AudioTransportSource transport_;
    juce::AudioFormatManager audio_format_manager_;
    juce::TimeSliceThread read_ahead_thread_{"AudioAlgo preview read-ahead"};
    std::unique_ptr<juce::AudioFormatReaderSource> reader_source_;
    std::filesystem::path current_file_;
    bool audio_device_ready_ = false;
    bool shut_down_ = false;
};


} // namespace audioalgo::editor
