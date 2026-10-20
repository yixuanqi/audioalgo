#pragma once

#include "audio_node_sdk/NodeSDK.h"

#include <cmath>
#include <filesystem>
#include <fstream>

namespace audioalgo::tests {

inline NodeDescriptor parameter_probe_descriptor() {
    ParameterDescriptor enabled{"enabled", ParameterKind::boolean, false, {}, {}, {}};
    ParameterDescriptor repeat{"repeat", ParameterKind::integer, std::int64_t{3}, 1.0, 8.0, "times"};
    repeat.step = 1.0;
    ParameterDescriptor scale{"scale", ParameterKind::floating, 0.5, 0.25, 2.0, "x"};
    scale.step = 0.25;
    ParameterDescriptor polarity{"polarity", ParameterKind::enumeration, std::string("negative"), {}, {}, {}};
    polarity.choices = {{"negative", "Negative"}, {"positive", "Positive"}};
    ParameterDescriptor tag{"tag", ParameterKind::string, std::string("initial"), {}, {}, {}};
    ParameterDescriptor coefficient{"coefficient", ParameterKind::path, std::string{}, {}, {}, {}};
    coefficient.input_resource = true;
    return {"test.parameter_probe", "1.0.0", "Six parameter probe",
            {{"in", "audio.pcm@1", PortDirection::input, true},
             {"out", "audio.pcm@1", PortDirection::output, false}},
            {enabled, repeat, scale, polarity, tag, coefficient}, NodeExecutionKind::block};
}

// Only the test plugin implements the reference experiment; the generic view and backend
// know neither this type ID nor the parameter names or arithmetic.
class ParameterProbeNode final : public AudioNodeProcessor {
public:
    NodeProcessOutcome process(const NodeProcessRequest&) override {
        return Diagnostic{"block_required", "Probe requires the native block path"};
    }
    bool supports_block_processing() const noexcept override { return true; }
    std::optional<Diagnostic> prepare(const NodePrepareContext& context) override {
        const auto& p = *context.parameters;
        std::ifstream stream(std::filesystem::u8path(std::get<std::string>(p.at("coefficient"))));
        double coefficient = 0.0;
        if (!(stream >> coefficient) || !std::isfinite(coefficient))
            return Diagnostic{"coefficient_invalid", "Coefficient resource must contain a finite number", {}, "coefficient"};
        factor_ = std::get<bool>(p.at("enabled"))
            ? static_cast<double>(std::get<std::int64_t>(p.at("repeat"))) * std::get<double>(p.at("scale")) *
                (std::get<std::string>(p.at("polarity")) == "positive" ? 1.0 : -1.0) * coefficient
            : 1.0;
        return std::nullopt;
    }
    AudioBlockOutcome process_block(const AudioBlockView& block) override {
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            for (std::size_t frame = 0; frame < block.frame_count; ++frame)
                block.output_channels[channel][frame] = static_cast<float>(block.input_channels[channel][frame] * factor_);
        return AudioBlockResult{block.frame_count};
    }
private:
    double factor_ = 1.0;
};

} // namespace audioalgo::tests
