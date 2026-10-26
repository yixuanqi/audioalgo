#pragma once
#include "EditorUiSupport.h"
#include "EditorWaveformData.h"
#include <cmath>

namespace audioalgo::editor {
class WaveformView final : public juce::Component {
public:
    void set_waveform(WaveformChannels waveform) {
        waveform_ = std::move(waveform);
        reset_view();
    }

    std::pair<double, double> visible_frames() const { return {view_start_, view_start_ + view_span_}; }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        if (waveform_.empty() || waveform_.front().empty() || !std::isfinite(wheel.deltaY) || wheel.deltaY == 0.f)
            return;
        const auto frames = total_frames();
        const auto minimum_span = std::min<double>(frames, std::max<std::size_t>(8, finest_bucket_frames() * 8));
        const auto next_span = std::clamp(view_span_ * std::pow(2.0, -3.0 * wheel.deltaY), minimum_span,
                                          static_cast<double>(frames));
        const auto anchor = std::clamp((event.position.x - plot_left) / plot_width(), 0.f, 1.f);
        view_start_ = std::clamp(view_start_ + anchor * (view_span_ - next_span), 0.0,
                                 static_cast<double>(frames) - next_span);
        view_span_ = next_span;
        setMouseCursor(view_span_ < frames ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!event.mods.isLeftButtonDown() || waveform_.empty() || view_span_ >= total_frames()) return;
        drag_origin_x_ = event.position.x;
        drag_start_frame_ = view_start_;
        dragging_ = true;
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (!dragging_) return;
        view_start_ = std::clamp(drag_start_frame_ - (event.position.x - drag_origin_x_) * view_span_ / plot_width(),
                                 0.0, static_cast<double>(total_frames()) - view_span_);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override { dragging_ = false; }

    void mouseDoubleClick(const juce::MouseEvent& event) override {
        if (event.mods.isLeftButtonDown()) reset_view();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        if (waveform_.empty() || waveform_.front().empty()) {
            g.drawText("Rendered waveform", 14, 8, getWidth() - 28, 20, juce::Justification::centredLeft);
            g.drawText("Run a project to inspect its audio output", 14, 32, getWidth() - 28, getHeight() - 40,
                       juce::Justification::centred);
            return;
        }
        const auto frames = total_frames();
        const auto zoom = static_cast<double>(frames) / view_span_;
        g.drawText("Waveform  " + juce::String(zoom, zoom < 10 ? 1 : 0) + "x", 14, 8,
                   std::max(0, getWidth() / 2 - 14), 20, juce::Justification::centredLeft);
        g.drawText("Wheel: zoom   Drag: pan   Double-click: fit", getWidth() / 2, 8,
                   std::max(0, getWidth() / 2 - 14), 20, juce::Justification::centredRight);

        const auto lanes = static_cast<int>(waveform_.size());
        const auto lane_top = 34;
        const auto lane_area = std::max(1, getHeight() - lane_top - 24);
        const auto lane_height = std::max(1, lane_area / lanes);
        const auto width = static_cast<int>(plot_width());
        const auto frames_per_pixel = view_span_ / width;
        for (int channel = 0; channel < lanes; ++channel) {
            const auto top = lane_top + channel * lane_height;
            const auto mid = static_cast<float>(top + lane_height / 2);
            const auto amplitude = std::max(1.f, lane_height * .43f);
            g.setColour(raised);
            g.drawHorizontalLine(static_cast<int>(mid), plot_left, plot_left + width);
            g.saveState();
            g.reduceClipRegion(plot_left, top, width + 1, lane_height);
            g.setColour(signal.withAlpha(channel == 0 ? .95f : .7f));
            float previous_mean = 0.f;
            for (int pixel = 0; pixel < width; ++pixel) {
                const auto first = std::min<std::size_t>(frames - 1,
                    static_cast<std::size_t>(view_start_ + pixel * frames_per_pixel));
                const auto last = std::min<std::size_t>(frames,
                    std::max(first + 1, static_cast<std::size_t>(std::ceil(view_start_ + (pixel + 1) * frames_per_pixel))));
                const auto range = visible_range(static_cast<std::size_t>(channel), first, last, frames_per_pixel);
                const auto x = plot_left + pixel;
                g.drawVerticalLine(x, mid - range.maximum * amplitude, mid - range.minimum * amplitude);
                const auto mean = (range.minimum + range.maximum) * .5f;
                if (pixel > 0 && frames_per_pixel <= finest_bucket_frames())
                    g.drawLine(static_cast<float>(x - 1), mid - previous_mean * amplitude,
                               static_cast<float>(x), mid - mean * amplitude, 1.f);
                previous_mean = mean;
            }
            g.restoreState();
            g.setColour(muted);
            g.setFont(juce::FontOptions(10.0f));
            g.drawText("CH " + juce::String(channel + 1), 14, top, 42, 14, juce::Justification::centredLeft);
        }
        g.setColour(muted);
        g.setFont(juce::FontOptions(10.f));
        const auto time_label = [this](double frame) {
            if (waveform_.detail && waveform_.detail->sample_rate_hz != 0)
                return juce::String(frame / waveform_.detail->sample_rate_hz, 3) + " s";
            return juce::String(static_cast<int>(frame)) + " bins";
        };
        const auto bottom = getHeight() - 18;
        g.drawText(time_label(view_start_), plot_left, bottom, 100, 15, juce::Justification::centredLeft);
        g.drawText(time_label(view_start_ + view_span_ / 2), getWidth() / 2 - 50, bottom, 100, 15,
                   juce::Justification::centred);
        g.drawText(time_label(view_start_ + view_span_), getWidth() - 114, bottom, 100, 15,
                   juce::Justification::centredRight);
    }

private:
    static constexpr int plot_left = 14;
    float plot_width() const { return static_cast<float>(std::max(1, getWidth() - 28)); }
    std::size_t total_frames() const { return waveform_.detail ? waveform_.detail->frames : waveform_.front().size(); }
    std::size_t finest_bucket_frames() const { return waveform_.detail ? waveform_.detail->finest_bucket_frames : 1; }
    WaveformRange visible_range(std::size_t channel, std::size_t first, std::size_t last,
                                double frames_per_pixel) const {
        if (waveform_.detail)
            return waveform_visible_range(waveform_, channel, first, last, frames_per_pixel);
        const auto& ranges = waveform_[channel];
        auto result = ranges[first];
        for (auto index = first + 1; index < last; ++index) {
            result.minimum = std::min(result.minimum, ranges[index].minimum);
            result.maximum = std::max(result.maximum, ranges[index].maximum);
        }
        return result;
    }
    void reset_view() {
        dragging_ = false;
        view_start_ = 0;
        view_span_ = waveform_.empty() || waveform_.front().empty() ? 0.0 : static_cast<double>(total_frames());
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
    }
    WaveformChannels waveform_;
    double view_start_ = 0.0;
    double view_span_ = 0.0;
    double drag_start_frame_ = 0.0;
    float drag_origin_x_ = 0.f;
    bool dragging_ = false;
};
} // namespace audioalgo::editor
