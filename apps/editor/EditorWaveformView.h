#pragma once
#include "EditorUiSupport.h"
#include "EditorWaveformData.h"
namespace audioalgo::editor {
class WaveformView final : public juce::Component {
public:
    void set_waveform(WaveformChannels waveform) {
        waveform_ = std::move(waveform);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(utf8_literal("Rendered waveform · peak-preserving overview"), 14, 8, getWidth() - 28, 20, juce::Justification::centredLeft);
        if (waveform_.empty() || waveform_.front().empty()) {
            g.setColour(muted);
            g.drawText("Run a project to inspect its audio output", 14, 32, getWidth() - 28, getHeight() - 40,
                       juce::Justification::centred);
            return;
        }

        const int lane_top = 34;
        const int lane_height = std::max(1, (getHeight() - lane_top - 10) / static_cast<int>(waveform_.size()));
        for (std::size_t channel = 0; channel < waveform_.size(); ++channel) {
            const auto y_mid = static_cast<float>(lane_top + static_cast<int>(channel) * lane_height + lane_height / 2);
            g.setColour(raised);
            g.drawHorizontalLine(static_cast<int>(y_mid), 12.0f, static_cast<float>(getWidth() - 12));
            g.setColour(signal.withAlpha(channel == 0 ? 0.95f : 0.62f));
            const auto& ranges = waveform_[channel];
            for (std::size_t column = 0; column < ranges.size(); ++column) {
                const auto x = 14.0f + static_cast<float>(column) * static_cast<float>(getWidth() - 28) /
                                                   static_cast<float>(std::max<std::size_t>(1, ranges.size() - 1));
                const auto scale = static_cast<float>(lane_height) * 0.43f;
                g.drawVerticalLine(static_cast<int>(x), y_mid - ranges[column].maximum * scale,
                                   y_mid - ranges[column].minimum * scale);
            }
            g.setColour(muted);
            g.setFont(juce::FontOptions(10.0f));
            g.drawText("CH " + juce::String(static_cast<int>(channel + 1)), 14,
                       lane_top + static_cast<int>(channel) * lane_height, 42, 14, juce::Justification::centredLeft);
        }
    }

private:
    WaveformChannels waveform_;
};


} // namespace audioalgo::editor
