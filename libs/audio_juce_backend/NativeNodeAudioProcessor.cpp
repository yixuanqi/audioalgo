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
    return {std::move(code), std::move(location) + ": " + std::move(message),
            std::move(node_id), std::move(port_id)};
}

} // namespace

NativeNodeAudioProcessor::BusesProperties NativeNodeAudioProcessor::buses_for(const NodeDescriptor& descriptor, int channel_count) {
    const auto channels = juce::AudioChannelSet::discreteChannels(std::max(1, channel_count));
    BusesProperties properties;
    for (const auto& port : descriptor.ports) {
        if (port.type_id != pcm_port_type_id)
            continue;
        if (port.direction == PortDirection::input)
            properties = properties.withInput(juce::String::fromUTF8(port.id.c_str()), channels, true);
        else if (port.direction == PortDirection::output)
            properties = properties.withOutput(juce::String::fromUTF8(port.id.c_str()), channels, true);
    }
    return properties;
}

NativeNodeAudioProcessor::NativeNodeAudioProcessor(std::unique_ptr<AudioNodeProcessor> processor,
                                                   std::string node_id,
                                                   const NodeDescriptor& descriptor,
                                                   NodeParameters parameters,
                                                   NodeExecutionContext context,
                                                   int channel_count)
    : AudioProcessor(buses_for(descriptor, channel_count)),
      processor_(std::move(processor)), node_id_(std::move(node_id)),
      input_port_id_(port_id(descriptor, PortDirection::input)),
      output_port_id_(port_id(descriptor, PortDirection::output)),
      parameters_(std::move(parameters)), context_(std::move(context)), channel_count_(channel_count),
      named_processing_(descriptor.capabilities.supports_named_audio_ports) {
    for (const auto& port : descriptor.ports) {
        if (port.direction == PortDirection::input)
            input_port_ids_.push_back(port.id);
        else if (port.direction == PortDirection::output)
            output_port_ids_.push_back(port.id);
    }
    if (node_id_.empty() || channel_count_ <= 0 || input_port_id_.empty() || output_port_id_.empty() ||
        input_port_ids_.empty() || output_port_ids_.empty() ||
        (!named_processing_ && (input_port_ids_.size() != 1 || output_port_ids_.size() != 1))) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("node_wrapper_unsupported", node_id_, {},
                                 "Generic JUCE wrapper requires declared nonempty audio groups and one group per direction for legacy nodes");
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
    if (named_processing_ ? !processor_->supports_named_block_processing() : !processor_->supports_block_processing()) {
        diagnostic_ = node_error("block_processing_unsupported", node_id_, {},
                                 "Node does not implement the allocation-free block interface "
                                 "and cannot run inside the native audio callback");
        sample_rate_hz_ = 0;
        maximum_block_frames_ = 0;
        return;
    }

    const auto channels = static_cast<std::size_t>(channel_count_);
    const auto input_count = input_port_ids_.size() * channels;
    const auto output_count = output_port_ids_.size() * channels;
    input_channels_.assign(input_count, nullptr);
    output_channels_.assign(output_count, nullptr);
    if (!named_processing_ && processor_->supports_in_place_block_processing())
        input_scratch_.clear();
    else
        input_scratch_.assign(input_count * static_cast<std::size_t>(maximum_block_frames_), 0.0f);
    named_inputs_.clear();
    named_outputs_.clear();
    if (named_processing_) {
        named_inputs_.resize(input_port_ids_.size());
        named_outputs_.resize(output_port_ids_.size());
        for (std::size_t port = 0; port < named_inputs_.size(); ++port)
            named_inputs_[port] = {input_port_ids_[port], {input_channels_.data() + port * channels, channels}};
        for (std::size_t port = 0; port < named_outputs_.size(); ++port)
            named_outputs_[port] = {output_port_ids_[port], {output_channels_.data() + port * channels, channels}};
    }

    const NodePrepareContext context{AudioFormat{sample_rate_hz_, static_cast<std::size_t>(channel_count_)},
                                     static_cast<std::size_t>(maximum_block_frames_), &parameters_};
    try {
        if (const auto error = processor_->prepare(context)) {
            diagnostic_ = node_error(error->code, node_id_, error->port_id, error->message);
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
        named_inputs_.clear();
        named_outputs_.clear();
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
    if (sample_count < 0 || buffer.getNumChannels() != static_cast<int>(std::max(input_port_ids_.size(), output_port_ids_.size())) * channel_count_ || sample_count > maximum_block_frames_) {
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
        const auto input_count = input_port_ids_.size() * channels;
        const auto output_count = output_port_ids_.size() * channels;
        const auto in_place = !named_processing_ && processor_->supports_in_place_block_processing();
        if (input_channels_.size() != input_count || output_channels_.size() != output_count ||
            (!in_place && input_scratch_.size() < input_count * static_cast<std::size_t>(maximum_block_frames_))) {
            diagnostic_ = node_error("node_not_prepared", node_id_, {}, "SDK block buffers were not prepared for this layout");
            buffer.clear();
            return;
        }
        const auto stage = static_cast<std::size_t>(sample_count);
        // Stage every input group before exposing any output aliases in JUCE's combined buffer.
        for (std::size_t port = 0; port < input_port_ids_.size(); ++port) {
            const auto bus = getBusBuffer(buffer, true, static_cast<int>(port));
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto flat = port * channels + channel;
                const auto* source = bus.getReadPointer(static_cast<int>(channel));
                if (in_place)
                    input_channels_[flat] = source;
                else {
                    auto* staged = input_scratch_.data() + flat * static_cast<std::size_t>(maximum_block_frames_);
                    std::copy_n(source, stage, staged);
                    input_channels_[flat] = staged;
                }
            }
        }
        for (std::size_t port = 0; port < output_port_ids_.size(); ++port) {
            auto bus = getBusBuffer(buffer, false, static_cast<int>(port));
            for (std::size_t channel = 0; channel < channels; ++channel)
                output_channels_[port * channels + channel] = bus.getWritePointer(static_cast<int>(channel));
        }
        const auto outcome = named_processing_
            ? processor_->process_named_block({sample_rate_hz_, stage, named_inputs_, named_outputs_})
            : processor_->process_block({sample_rate_hz_, stage, channels, input_channels_.data(), output_channels_.data()});
        if (const auto* error = std::get_if<Diagnostic>(&outcome)) {
            diagnostic_ = node_error(error->code, node_id_,
                                     error->port_id.empty() ? input_port_id_ : error->port_id, error->message);
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
        // The flat channel index maps back onto the named port that owns it, so a multi-output
        // failure is attributed to the poisoned port and never to the descriptor's first output.
        for (std::size_t channel = 0; channel < output_count; ++channel) {
            const auto* samples = output_channels_[channel];
            for (std::size_t frame = 0; frame < stage; ++frame) {
                if (!std::isfinite(samples[frame])) {
                    diagnostic_ = node_error("output_non_finite", node_id_,
                                             output_port_ids_[channel / channels],
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
    if (layouts.inputBuses.size() != static_cast<int>(input_port_ids_.size()) ||
        layouts.outputBuses.size() != static_cast<int>(output_port_ids_.size()) || input_port_ids_.empty())
        return false;
    const auto& reference = layouts.getMainInputChannelSet();
    if (reference.size() != channel_count_)
        return false;
    for (const auto& input : layouts.inputBuses)
        if (input != reference)
            return false;
    for (const auto& output : layouts.outputBuses)
        if (output != reference)
            return false;
    return true;
}

} // namespace audioalgo
