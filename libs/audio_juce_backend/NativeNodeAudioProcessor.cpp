#include "audio_juce_backend/NativeNodeAudioProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace audioalgo {
namespace {

std::string port_id(const NodeDescriptor& descriptor, PortDirection direction) {
    const auto found = std::find_if(descriptor.ports.begin(), descriptor.ports.end(), [&](const PortDescriptor& port) {
        return port.direction == direction;
    });
    return found == descriptor.ports.end() ? std::string{} : found->id;
}

constexpr std::string_view pcm_port_type_id = "audio.pcm@1";

// The native audio region only carries PCM audio; any other typed port must be rejected instead of
// being flattened into JUCE channels.
const PortDescriptor* first_unsupported_port(const NodeDescriptor& descriptor) {
    const auto found = std::find_if(descriptor.ports.begin(), descriptor.ports.end(), [](const PortDescriptor& port) {
        return port.type_id != pcm_port_type_id;
    });
    return found == descriptor.ports.end() ? nullptr : &*found;
}

Diagnostic node_error(std::string code, std::string node_id, std::string port_id, std::string message) {
    auto location = "Node " + node_id;
    if (!port_id.empty())
        location += " port " + port_id;
    return {std::move(code), std::move(location) + ": " + std::move(message)};
}

} // namespace

NativeNodeAudioProcessor::BusesProperties NativeNodeAudioProcessor::buses_for(int channel_count) {
    const auto channels = juce::AudioChannelSet::discreteChannels(std::max(1, channel_count));
    return BusesProperties().withInput("Input", channels, true).withOutput("Output", channels, true);
}

NativeNodeAudioProcessor::NativeNodeAudioProcessor(std::unique_ptr<AudioNodeProcessor> processor,
                                                   std::string node_id,
                                                   const NodeDescriptor& descriptor,
                                                   NodeParameters parameters,
                                                   NodeExecutionContext context,
                                                   int channel_count)
    : AudioProcessor(buses_for(channel_count)),
      processor_(std::move(processor)),
      node_id_(std::move(node_id)),
      input_port_id_(port_id(descriptor, PortDirection::input)),
      output_port_id_(port_id(descriptor, PortDirection::output)),
      parameters_(std::move(parameters)),
      context_(std::move(context)),
      channel_count_(channel_count) {
    const auto input_count = std::count_if(descriptor.ports.begin(), descriptor.ports.end(), [](const PortDescriptor& port) {
        return port.direction == PortDirection::input;
    });
    const auto output_count = std::count_if(descriptor.ports.begin(), descriptor.ports.end(), [](const PortDescriptor& port) {
        return port.direction == PortDirection::output;
    });
    if (input_count != 1 || output_count != 1 ||
        node_id_.empty() || channel_count_ <= 0 || input_port_id_.empty() || output_port_id_.empty()) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("node_wrapper_unsupported", node_id_, {},
                                 "Generic JUCE wrapper requires one named audio input and one named audio output");
    } else if (const auto* unsupported_port = first_unsupported_port(descriptor)) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("port_type_unsupported", node_id_, unsupported_port->id,
                                 "Native audio region only accepts " + std::string(pcm_port_type_id) +
                                     " ports, got " + unsupported_port->type_id);
    } else if (!processor_) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("node_factory_returned_null", node_id_, {}, "No SDK processor instance was supplied");
    }
}

void NativeNodeAudioProcessor::prepareToPlay(double sample_rate_hz, int maximum_expected_samples_per_block) {
    if (structurally_invalid_)
        return;
    // A new prepare is authoritative: clear any failure from an earlier, different configuration.
    diagnostic_.reset();
    if (!std::isfinite(sample_rate_hz) || sample_rate_hz < 1.0 ||
        sample_rate_hz > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        diagnostic_ = node_error("invalid_audio_format", node_id_, {}, "JUCE supplied an invalid sample rate");
        return;
    }
    if (maximum_expected_samples_per_block <= 0) {
        diagnostic_ = node_error("invalid_block_size", node_id_, {}, "JUCE supplied an invalid maximum block size");
        return;
    }
    sample_rate_hz_ = static_cast<std::uint32_t>(sample_rate_hz);
    maximum_block_frames_ = maximum_expected_samples_per_block;
    diagnostic_.reset();

    // A node without the allocation-free block path cannot run inside the native audio callback:
    // the owned-port route allocates and may perform whole-task work, so it is refused here rather
    // than silently falling back inside processBlock.
    if (!processor_->supports_block_processing()) {
        diagnostic_ = node_error("block_processing_unsupported", node_id_, {},
                                 "Node does not implement the allocation-free block interface "
                                 "and cannot run inside the native audio callback");
        sample_rate_hz_ = 0;
        maximum_block_frames_ = 0;
        return;
    }

    input_channels_.assign(static_cast<std::size_t>(channel_count_), nullptr);
    output_channels_.assign(static_cast<std::size_t>(channel_count_), nullptr);
    if (processor_->supports_in_place_block_processing())
        input_scratch_.clear();
    else
        input_scratch_.assign(static_cast<std::size_t>(channel_count_) *
                                  static_cast<std::size_t>(maximum_block_frames_), 0.0f);

    const NodePrepareContext context{AudioFormat{sample_rate_hz_, static_cast<std::size_t>(channel_count_)},
                                     static_cast<std::size_t>(maximum_block_frames_), &parameters_};
    try {
        if (const auto error = processor_->prepare(context)) {
            diagnostic_ = node_error(error->code, node_id_, {}, error->message);
        }
    } catch (const std::exception& error) {
        diagnostic_ = node_error("node_prepare_exception", node_id_, {}, error.what());
    } catch (...) {
        diagnostic_ = node_error("node_prepare_exception", node_id_, {},
                                 "SDK node prepare raised an unknown exception");
    }
    if (diagnostic_) {
        // Release any partial preparation so a later run cannot inherit half-prepared state.
        try {
            processor_->reset();
        } catch (...) {
        }
        sample_rate_hz_ = 0;
        maximum_block_frames_ = 0;
        input_scratch_.clear();
        input_channels_.clear();
        output_channels_.clear();
    }
}

void NativeNodeAudioProcessor::releaseResources() {
    if (processor_ != nullptr)
        processor_->reset();
    sample_rate_hz_ = 0;
    maximum_block_frames_ = 0;
}

void NativeNodeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    midi.clear();
    if (diagnostic_) {
        buffer.clear();
        return;
    }
    const auto sample_count = buffer.getNumSamples();
    if (sample_rate_hz_ == 0 || processor_ == nullptr) {
        diagnostic_ = node_error("node_not_prepared", node_id_, {}, "SDK processor is not ready to process a block");
        buffer.clear();
        return;
    }
    if (sample_count < 0 || buffer.getNumChannels() != channel_count_ || sample_count > maximum_block_frames_) {
        diagnostic_ = node_error("block_size_unsupported", node_id_, {}, "JUCE block does not match the prepared channel/block limits");
        buffer.clear();
        return;
    }
    if (sample_count == 0) {
        // JUCE legitimately calls with an empty block while re-laying-out the graph; nothing to render.
        return;
    }

    try {
        const auto channels = static_cast<std::size_t>(channel_count_);
        const auto in_place = processor_->supports_in_place_block_processing();
        if (input_channels_.size() != channels || output_channels_.size() != channels ||
            (!in_place && input_scratch_.size() < channels * static_cast<std::size_t>(maximum_block_frames_))) {
            diagnostic_ = node_error("node_not_prepared", node_id_, {},
                                     "SDK block buffers were not prepared for this layout");
            buffer.clear();
            return;
        }
        const auto stage = static_cast<std::size_t>(sample_count);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto* source = buffer.getReadPointer(static_cast<int>(channel));
            output_channels_[channel] = buffer.getWritePointer(static_cast<int>(channel));
            if (in_place) {
                input_channels_[channel] = source;
            } else {
                auto* staged = input_scratch_.data() + channel * static_cast<std::size_t>(maximum_block_frames_);
                std::copy_n(source, stage, staged);
                input_channels_[channel] = staged;
            }
        }
        const AudioBlockView block{sample_rate_hz_, stage, channels,
                                   input_channels_.data(), output_channels_.data()};
        const auto outcome = processor_->process_block(block);
        if (const auto* error = std::get_if<Diagnostic>(&outcome)) {
            diagnostic_ = node_error(error->code, node_id_, input_port_id_, error->message);
            buffer.clear();
            return;
        }
        if (std::get<AudioBlockResult>(outcome).processed_frames != stage) {
            diagnostic_ = node_error("output_incomplete", node_id_, output_port_id_,
                                     "SDK node did not produce the whole requested block");
            buffer.clear();
            return;
        }
        // The block path must reject the same class of bad output the owned path did: a finite input
        // can still overflow to infinity, and a node can simply write NaN. Allocation-free scan.
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto* samples = output_channels_[channel];
            for (std::size_t frame = 0; frame < stage; ++frame) {
                if (!std::isfinite(samples[frame])) {
                    diagnostic_ = node_error("output_non_finite", node_id_, output_port_id_,
                                             "SDK block output contains NaN or infinity");
                    buffer.clear();
                    return;
                }
            }
        }
    } catch (const std::exception& error) {
        diagnostic_ = node_error("node_process_exception", node_id_, input_port_id_, error.what());
        buffer.clear();
    } catch (...) {
        diagnostic_ = node_error("node_process_exception", node_id_, input_port_id_, "SDK processor raised an unknown exception");
        buffer.clear();
    }
}

bool NativeNodeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();
    return input == output && input.size() == channel_count_;
}

} // namespace audioalgo


