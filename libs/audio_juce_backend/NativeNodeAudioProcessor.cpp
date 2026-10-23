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
    streaming_processing_ = descriptor.capabilities.uses_streaming_frame_api;
    maximum_no_progress_calls_ = descriptor.capabilities.maximum_no_progress_calls;
    if (!descriptor.capabilities.supports_variable_block_size &&
        descriptor.capabilities.fixed_frame_end_policy == NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad) {
        fixed_frames_ = descriptor.capabilities.fixed_block_frames;
        if (fixed_frames_ > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            structurally_invalid_ = true;
            diagnostic_ = node_error("fixed_frame_size_unsupported", node_id_, {}, "Fixed frame exceeds the native buffer range");
        } else {
            // For arbitrary host partitions, the first complete frame becomes
            // available at sample F-1. This is buffering delay, not lost input.
            setLatencySamples(fixed_frames_ == 0 ? 0 : static_cast<int>(fixed_frames_ - 1));
        }
    }
    if (!descriptor.capabilities.latency_frames) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("latency_unknown", node_id_, {}, "Algorithm latency must be known before native execution");
    } else {
        const auto buffering = fixed_frames_ == 0 ? 0 : fixed_frames_ - 1;
        const auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
        if (*descriptor.capabilities.latency_frames > limit || buffering > limit - *descriptor.capabilities.latency_frames) {
            structurally_invalid_ = true;
            diagnostic_ = node_error("latency_unsupported", node_id_, {}, "Algorithm plus buffering latency exceeds the native graph range");
        } else setLatencySamples(static_cast<int>(buffering + *descriptor.capabilities.latency_frames));
    }
    if (streaming_processing_ && (fixed_frames_ == 0 || maximum_no_progress_calls_ == 0 || maximum_no_progress_calls_ > 1024)) {
        structurally_invalid_ = true;
        diagnostic_ = node_error("stream_frame_profile_required", node_id_, {}, "Streaming requires a fixed padded frame and no-progress budget 1..1024");
    }
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
    timeline_cursor_ = 0;
    diagnostic_.reset();

    // A node without the allocation-free block path cannot run inside the native audio callback:
    // the owned-port route allocates and may perform whole-task work, so it is refused here rather
    // than silently falling back inside processBlock.
    if (streaming_processing_ ? !processor_->supports_stream_processing() :
        (named_processing_ ? !processor_->supports_named_block_processing() : !processor_->supports_block_processing())) {
        diagnostic_ = node_error(streaming_processing_ ? "stream_processing_unsupported" : "block_processing_unsupported", node_id_, {},
                                 "Node does not implement the allocation-free block interface "
                                 "and cannot run inside the native audio callback");
        sample_rate_hz_ = 0;
        maximum_block_frames_ = 0;
        return;
    }

    const auto channels = static_cast<std::size_t>(channel_count_);
    const auto input_count = input_port_ids_.size() * channels;
    const auto output_count = output_port_ids_.size() * channels;
    if ((!finite_output_ends_.empty() && finite_output_ends_.size() != output_port_ids_.size()) ||
        (!finite_input_ends_.empty() && finite_input_ends_.size() != input_port_ids_.size())) {
        diagnostic_ = node_error("stream_timeline_invalid", node_id_, {}, "Finite end declarations must cover the actual port groups");
        return;
    }
    input_channels_.assign(input_count, nullptr);
    output_channels_.assign(output_count, nullptr);
    if (fixed_frames_ != 0) {
        fixed_input_.assign(input_count * fixed_frames_, 0.0f);
        fixed_output_.assign(output_count * fixed_frames_, 0.0f);
        fixed_fill_ = fixed_pending_ = fixed_read_ = 0;
        fixed_leading_ = fixed_frames_ - 1;
        fixed_stats_ = {fixed_frames_, fixed_frames_, 0, 0};
        for (std::size_t i = 0; i < input_count; ++i) input_channels_[i] = fixed_input_.data() + i * fixed_frames_;
        for (std::size_t i = 0; i < output_count; ++i) output_channels_[i] = fixed_output_.data() + i * fixed_frames_;
    }
    if (streaming_processing_) {
        stream_input_totals_.assign(input_port_ids_.size(), 0);
        stream_output_totals_.assign(output_port_ids_.size(), 0);
        stream_consumed_.assign(input_port_ids_.size(), 0);
        stream_produced_.assign(output_port_ids_.size(), 0);
        stream_finished_.assign(output_port_ids_.size(), 0);
        stream_output_finished_.assign(output_port_ids_.size(), 0);
        stream_input_channels_.assign(input_count, nullptr);
        stream_output_channels_.assign(output_count, nullptr);
        stream_inputs_.resize(input_port_ids_.size());
        stream_outputs_.resize(output_port_ids_.size());
        for (std::size_t p = 0; p < stream_inputs_.size(); ++p)
            stream_inputs_[p] = {input_port_ids_[p], {stream_input_channels_.data() + p * channels, channels}, 0};
        for (std::size_t p = 0; p < stream_outputs_.size(); ++p)
            stream_outputs_[p] = {output_port_ids_[p], {stream_output_channels_.data() + p * channels, channels}, 0};
    }
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
                                     fixed_frames_ != 0 ? fixed_frames_ : static_cast<std::size_t>(maximum_block_frames_), &parameters_};
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
    fixed_fill_ = fixed_pending_ = fixed_read_ = 0;
    fixed_leading_ = fixed_frames_ == 0 ? 0 : fixed_frames_ - 1;
}

void NativeNodeAudioProcessor::process_fixed_block(juce::AudioBuffer<float>& buffer) {
    const auto channels = static_cast<std::size_t>(channel_count_);
    const auto input_count = input_port_ids_.size() * channels;
    const auto output_count = output_port_ids_.size() * channels;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        if (finite_output_frames_ && timeline_cursor_ >= *finite_output_frames_) {
            for (std::size_t channel = 0; channel < output_count; ++channel)
                buffer.setSample(static_cast<int>(channel), sample, 0.0f);
            continue;
        }
        ++timeline_cursor_;
        // Stage all named input buses for this sample before writing any aliases.
        for (std::size_t channel = 0; channel < input_count; ++channel)
            fixed_input_[channel * fixed_frames_ + fixed_fill_] = buffer.getSample(static_cast<int>(channel), sample);
        ++fixed_fill_;
        fixed_stats_.peak_input = std::max(fixed_stats_.peak_input, fixed_fill_);
        if (fixed_fill_ == fixed_frames_) {
            if (fixed_pending_ != 0) {
                diagnostic_ = node_error("fixed_fifo_overflow", node_id_, output_port_id_, "Output frame was not consumed before the next frame");
                buffer.clear(); return;
            }
            if (streaming_processing_) {
                if (!process_stream_frame(timeline_cursor_ - fixed_frames_, fixed_frames_)) { buffer.clear(); return; }
            } else {
            const auto outcome = named_processing_
                ? processor_->process_named_block({sample_rate_hz_, fixed_frames_, named_inputs_, named_outputs_})
                : processor_->process_block({sample_rate_hz_, fixed_frames_, channels, input_channels_.data(), output_channels_.data()});
            if (const auto* error = std::get_if<Diagnostic>(&outcome)) {
                diagnostic_ = node_error(error->code, node_id_, error->port_id.empty() ? input_port_id_ : error->port_id, error->message);
                buffer.clear(); return;
            }
            if (std::get<AudioBlockResult>(outcome).processed_frames != fixed_frames_) {
                diagnostic_ = node_error("output_incomplete", node_id_, output_port_id_, "Fixed-frame node did not produce its whole frame");
                buffer.clear(); return;
            }
            }
            for (std::size_t channel = 0; channel < output_count; ++channel)
                for (std::size_t frame = 0; frame < fixed_frames_; ++frame)
                    if (!std::isfinite(fixed_output_[channel * fixed_frames_ + frame])) {
                        diagnostic_ = node_error("output_non_finite", node_id_, output_port_ids_[channel / channels], "Fixed-frame output contains NaN or infinity");
                        buffer.clear(); return;
                    }
            fixed_fill_ = fixed_read_ = 0;
            fixed_pending_ = fixed_frames_;
            fixed_stats_.peak_output = std::max(fixed_stats_.peak_output, fixed_pending_);
        }
        if (fixed_leading_ != 0) {
            --fixed_leading_;
            for (std::size_t channel = 0; channel < output_count; ++channel)
                buffer.setSample(static_cast<int>(channel), sample, 0.0f);
        } else {
            if (fixed_pending_ == 0) {
                diagnostic_ = node_error("fixed_fifo_underflow", node_id_, output_port_id_, "No complete frame is available after buffering latency");
                buffer.clear(); return;
            }
            for (std::size_t channel = 0; channel < output_count; ++channel) {
                const auto ended = !finite_output_ends_.empty() && timeline_cursor_ > finite_output_ends_[channel / channels];
                buffer.setSample(static_cast<int>(channel), sample, ended ? 0.0f : fixed_output_[channel * fixed_frames_ + fixed_read_]);
            }
            ++fixed_read_; --fixed_pending_;
        }
    }
}

void NativeNodeAudioProcessor::complete_finite_stream() {
    if (diagnostic_ || !streaming_processing_ || !finite_output_frames_) return;
    if (std::all_of(stream_output_finished_.begin(), stream_output_finished_.end(), [](auto done) { return done != 0; })) return;
    if (*finite_output_frames_ != fixed_frames_ - 1) {
        const auto unfinished=std::find(stream_output_finished_.begin(),stream_output_finished_.end(),0);
        const auto port=static_cast<std::size_t>(std::distance(stream_output_finished_.begin(),unfinished));
        diagnostic_ = node_error("stream_eos_missing", node_id_, output_port_ids_[port], "Finite rendering ended without every output EOS acknowledgement");
        return;
    }
    try {
        (void)process_stream_frame(0, 0);
    } catch (const std::exception& error) {
        diagnostic_ = node_error("node_process_exception", node_id_, {}, error.what());
    } catch (...) {
        diagnostic_ = node_error("node_process_exception", node_id_, {}, "Empty stream EOS raised an unknown exception");
    }
}

bool NativeNodeAudioProcessor::process_stream_frame(std::size_t frame_start, std::size_t extent) {
    const auto channels = static_cast<std::size_t>(channel_count_);
    std::fill(stream_input_totals_.begin(), stream_input_totals_.end(), 0);
    std::fill(stream_output_totals_.begin(), stream_output_totals_.end(), 0);
    std::size_t stalled = 0;
    const auto output_end = [&](std::size_t port) {
        const auto end = !finite_output_ends_.empty() ? finite_output_ends_[port] :
            finite_output_frames_.value_or(std::numeric_limits<std::size_t>::max());
        return end - std::min(end, fixed_frames_ - 1);
    };
    const auto last_output_frame = [&](std::size_t port) {
        return (finite_output_frames_ || !finite_output_ends_.empty()) && output_end(port) <= frame_start + extent;
    };
    const auto before_end = [](std::size_t end, std::size_t cursor, std::size_t capacity) {
        return std::min(capacity, end - std::min(end, cursor));
    };
    for (;;) {
        const auto complete = [&](const auto& totals) {
            return std::all_of(totals.begin(), totals.end(), [&](auto n) { return n == extent; });
        };
        bool outputs_complete = true;
        for (std::size_t p = 0; p < stream_output_totals_.size(); ++p)
            outputs_complete &= last_output_frame(p) ? stream_output_finished_[p] != 0 : stream_output_totals_[p] == extent;
        if (complete(stream_input_totals_) && outputs_complete) return true;
        for (std::size_t p = 0; p < stream_inputs_.size(); ++p) {
            stream_inputs_[p].frame_count = extent - stream_input_totals_[p];
            const auto end = finite_input_ends_.empty() ? std::numeric_limits<std::size_t>::max() : finite_input_ends_[p];
            stream_inputs_[p].frames_before_eos = before_end(end, frame_start + stream_input_totals_[p], stream_inputs_[p].frame_count);
            stream_inputs_[p].end_of_stream = !finite_input_ends_.empty() && end <= frame_start + extent;
            for (std::size_t c = 0; c < channels; ++c)
                stream_input_channels_[p * channels + c] = input_channels_[p * channels + c] + stream_input_totals_[p];
        }
        for (std::size_t p = 0; p < stream_outputs_.size(); ++p) {
            stream_outputs_[p].frame_capacity = stream_output_finished_[p] ? 0 : extent - stream_output_totals_[p];
            stream_outputs_[p].frames_before_eos = before_end(output_end(p), frame_start + stream_output_totals_[p], stream_outputs_[p].frame_capacity);
            stream_outputs_[p].end_of_stream = last_output_frame(p);
            for (std::size_t c = 0; c < channels; ++c)
                stream_output_channels_[p * channels + c] = output_channels_[p * channels + c] + stream_output_totals_[p];
        }
        std::fill(stream_consumed_.begin(), stream_consumed_.end(), 0);
        std::fill(stream_produced_.begin(), stream_produced_.end(), 0);
        std::fill(stream_finished_.begin(), stream_finished_.end(), 0);
        const auto outcome = processor_->process_stream({sample_rate_hz_, stream_inputs_, stream_outputs_},
                                                        {stream_consumed_, stream_produced_, stream_finished_});
        if (const auto* error = std::get_if<Diagnostic>(&outcome)) {
            diagnostic_ = node_error(error->code, node_id_, error->port_id, error->message); return false;
        }
        const auto status = std::get<AudioStreamStatus>(outcome);
        if (status != AudioStreamStatus::progress && status != AudioStreamStatus::need_more_input) {
            diagnostic_ = node_error("stream_status_invalid", node_id_, {}, "Node returned an unknown stream status"); return false;
        }
        bool advanced = false;
        for (std::size_t p = 0; p < stream_inputs_.size(); ++p) {
            if (stream_consumed_[p] > stream_inputs_[p].frame_count) {
                diagnostic_ = node_error("stream_input_count_invalid", node_id_, input_port_ids_[p], "Node consumed more samples than the offered input span"); return false;
            }
            stream_input_totals_[p] += stream_consumed_[p]; advanced |= stream_consumed_[p] != 0;
        }
        for (std::size_t p = 0; p < stream_outputs_.size(); ++p) {
            if (stream_produced_[p] > stream_outputs_[p].frame_capacity) {
                diagnostic_ = node_error("stream_output_count_invalid", node_id_, output_port_ids_[p], "Node produced more samples than the offered output capacity"); return false;
            }
            stream_output_totals_[p] += stream_produced_[p]; advanced |= stream_produced_[p] != 0;
            if (stream_finished_[p] > 1 || (stream_finished_[p] &&
                (!stream_outputs_[p].end_of_stream || stream_produced_[p] < stream_outputs_[p].frames_before_eos || stream_output_finished_[p]))) {
                diagnostic_ = node_error("stream_eos_invalid", node_id_, output_port_ids_[p], "Output finished before its planned logical end or acknowledged EOS twice");
                return false;
            }
            if (stream_finished_[p]) {
                stream_output_finished_[p] = 1; advanced = true;
                for (std::size_t c = 0; c < channels; ++c)
                    std::fill(output_channels_[p * channels + c] + stream_output_totals_[p],
                              output_channels_[p * channels + c] + fixed_frames_, 0.0f);
            }
        }
        if (advanced) stalled = 0;
        else if (++stalled >= maximum_no_progress_calls_) {
            std::string blocked_port;
            for(std::size_t p=0;p<stream_input_totals_.size();++p)if(stream_input_totals_[p]<extent){blocked_port=input_port_ids_[p];break;}
            if(blocked_port.empty())for(std::size_t p=0;p<stream_output_totals_.size();++p)
                if(last_output_frame(p)?!stream_output_finished_[p]:stream_output_totals_[p]<extent){blocked_port=output_port_ids_[p];break;}
            diagnostic_ = node_error("stream_no_progress", node_id_, blocked_port, "No consumed/produced progress within budget " + std::to_string(maximum_no_progress_calls_));
            return false;
        }
        // Each advancing call increases bounded integer totals. Empty calls are
        // limited above, so a frame cannot spin forever or discard pending input.
    }
}

void NativeNodeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    midi.clear();
    if (diagnostic_) {
        buffer.clear();
        return;
    }
    auto sample_count = buffer.getNumSamples();
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
        if (fixed_frames_ != 0) {
            process_fixed_block(buffer);
            return;
        }
        if (finite_output_frames_) {
            const auto remaining = *finite_output_frames_ - std::min(timeline_cursor_, *finite_output_frames_);
            const auto active = std::min<std::size_t>(static_cast<std::size_t>(sample_count), remaining);
            if (active == 0) { buffer.clear(); return; }
            if (active < static_cast<std::size_t>(sample_count))
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer.clear(channel, static_cast<int>(active), sample_count - static_cast<int>(active));
            sample_count = static_cast<int>(active);
            timeline_cursor_ += active;
        }
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
