#include "audio_juce_backend/JuceGraph.h"
#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "audio_juce_backend/NativeNodeAudioProcessor.h"
#include "plugins/builtin/GainNodeAdapter.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Controlled allocation counter. Only counts while armed, so unrelated test setup is excluded.
// ---------------------------------------------------------------------------------------------
namespace {
std::atomic<std::size_t> g_allocations{0};
std::atomic<bool> g_armed{false};

void note_allocation() {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void* operator new(std::size_t size) {
    note_allocation();
    if (void* pointer = std::malloc(size == 0 ? 1 : size))
        return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    note_allocation();
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    note_allocation();
    if (void* pointer = _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment)))
        return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

namespace {

struct AllocationScope {
    AllocationScope() {
        g_allocations.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }
    ~AllocationScope() { g_armed.store(false, std::memory_order_relaxed); }
    [[nodiscard]] std::size_t count() const { return g_allocations.load(std::memory_order_relaxed); }
};

audioalgo::NodeDescriptor block_descriptor(std::string type_id, std::string input_port, std::string output_port) {
    return {std::move(type_id), "1.0.0", "Block processor",
            {{std::move(input_port), "audio.pcm@1", audioalgo::PortDirection::input, true},
             {std::move(output_port), "audio.pcm@1", audioalgo::PortDirection::output, false}},
            {}, audioalgo::NodeExecutionKind::block};
}

// Passes audio through unchanged using only the borrowed block path.
class PassthroughBlockProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported", "passthrough only implements the borrowed block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        ++blocks;
        if (block.channel_count == 0 || block.frame_count == 0)
            return audioalgo::Diagnostic{"invalid_audio_view", "empty block"};
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            std::copy_n(block.input_channels[channel], block.frame_count, block.output_channels[channel]);
        return audioalgo::AudioBlockResult{block.frame_count};
    }
    std::size_t blocks = 0;
};

// Records the lifecycle so a single instance, its prepare count and its reset are observable.
class LifecycleProbeProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported", "lifecycle probe only implements the block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        ++processed_blocks;
        recorded_instances.push_back(this);
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            std::copy_n(block.input_channels[channel], block.frame_count, block.output_channels[channel]);
        return audioalgo::AudioBlockResult{block.frame_count};
    }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& context) override {
        ++prepare_calls;
        prepared_format = context.format;
        prepared_maximum_block = context.maximum_block_frames;
        return std::nullopt;
    }
    void reset() noexcept override { ++reset_calls; }

    std::size_t prepare_calls = 0;
    std::size_t reset_calls = 0;
    std::size_t processed_blocks = 0;
    audioalgo::AudioFormat prepared_format{};
    std::size_t prepared_maximum_block = 0;
    std::vector<const void*> recorded_instances;
};

// Fails on the borrowed path so the wrapper must translate the diagnostic with node/port context.
class FailingBlockProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit FailingBlockProcessor(audioalgo::Diagnostic failure) : failure_(std::move(failure)) {}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported", "failing probe only implements the block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView&) override {
        return failure_;
    }

private:
    audioalgo::Diagnostic failure_;
};

// Reports fewer processed frames than requested, so no progress cannot become a silent success.
class ShortBlockProcessor final : public audioalgo::AudioNodeProcessor {public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported", "short probe only implements the block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        return audioalgo::AudioBlockResult{block.frame_count / 2};
    }
};

// Writes a non-finite sample while still reporting a complete block.
class NonFiniteBlockProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit NonFiniteBlockProcessor(int mode) : mode_(mode) {}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_block_unsupported", "non-finite probe only implements the block path"};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            std::copy_n(block.input_channels[channel], block.frame_count, block.output_channels[channel]);
        const auto poisoned = std::min<std::size_t>(1, block.frame_count - 1);
        switch (mode_) {
        case 0: block.output_channels[0][poisoned] = std::numeric_limits<float>::quiet_NaN(); break;
        case 1: block.output_channels[0][poisoned] = std::numeric_limits<float>::infinity(); break;
        case 2: block.output_channels[0][poisoned] = -std::numeric_limits<float>::infinity(); break;
        default:
            // Finite input, finite arithmetic, infinite result.
            block.output_channels[0][poisoned] = std::numeric_limits<float>::max();
            block.output_channels[0][poisoned] *= 4.0f;
            break;
        }
        return audioalgo::AudioBlockResult{block.frame_count};
    }

private:
    int mode_ = 0;
};

// Throws from prepare, which previously escaped the wrapper and the whole run service.
class ThrowingPrepareProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::AudioPorts{};
    }
    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }
    [[nodiscard]] audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        return audioalgo::AudioBlockResult{block.frame_count};
    }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override {
        throw std::runtime_error("plugin prepare failed");
    }
};

// Implements only the owned-port entry point, so the native callback must refuse it.
class OwnedOnlyProcessor final : public audioalgo::AudioNodeProcessor {
public:
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest& request) override {
        const auto input = request.inputs.find("audio_in");
        if (input == request.inputs.end())
            return audioalgo::Diagnostic{"input_missing", "owned probe requires audio_in"};
        auto output = input->second;
        return audioalgo::AudioPorts{{"audio_out", std::move(output)}};
    }
};

std::vector<std::vector<float>> fill_block(juce::AudioBuffer<float>& buffer, std::size_t frames, double scale = 1.0) {
    std::vector<std::vector<float>> expected(static_cast<std::size_t>(buffer.getNumChannels()),
                                             std::vector<float>(frames, 0.0f));
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto value = static_cast<float>(scale * (0.05 + 0.1 * channel) *
                                                  std::sin(static_cast<double>(frame + 1) * 0.017));
            buffer.setSample(channel, static_cast<int>(frame), value);
            expected[static_cast<std::size_t>(channel)][frame] = value;
        }
    }
    return expected;
}

} // namespace

TEST_CASE("AR-07.b declared channel and block capabilities drive real audio", "[R1][AR-07.b]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    constexpr double gain_db = 6.020599913279624;
    const auto gain = std::pow(10.0, gain_db / 20.0);
    const std::vector<int> block_sizes{1, 127, 256, 480, 512, 1024};

    for (const int channels : {1, 2, 4}) {
        for (const int block_size : block_sizes) {
            INFO("channels=" << channels << " block=" << block_size);
            audioalgo::NativeNodeAudioProcessor wrapper(
                std::make_unique<audioalgo::GainNodeAdapter>(), "gain-b", block_descriptor("builtin.gain", "audio_in", "audio_out"),
                {{"gain_db", gain_db}}, {}, channels);
            wrapper.prepareToPlay(48000.0, block_size);
            REQUIRE_FALSE(wrapper.processing_diagnostic());

            juce::AudioBuffer<float> buffer(channels, block_size);
            const auto expected = fill_block(buffer, static_cast<std::size_t>(block_size));
            juce::MidiBuffer midi;
            wrapper.processBlock(buffer, midi);
            REQUIRE_FALSE(wrapper.processing_diagnostic());
            for (int channel = 0; channel < channels; ++channel)
                for (std::size_t frame = 0; frame < expected.front().size(); ++frame)
                    CHECK(buffer.getSample(channel, static_cast<int>(frame)) ==
                          Catch::Approx(static_cast<float>(static_cast<double>(expected[static_cast<std::size_t>(channel)][frame]) * gain))
                              .margin(1.0e-6));
        }
    }

    // A tail block shorter than the prepared maximum is supported.
    audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<audioalgo::GainNodeAdapter>(), "gain-tail",
                                                block_descriptor("builtin.gain", "audio_in", "audio_out"),
                                                {{"gain_db", gain_db}}, {}, 2);
    wrapper.prepareToPlay(48000.0, 1024);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    juce::AudioBuffer<float> tail(2, 5);
    (void)fill_block(tail, 5);
    juce::MidiBuffer midi;
    wrapper.processBlock(tail, midi);
    REQUIRE_FALSE(wrapper.processing_diagnostic());

    // Blocks larger than the prepared maximum and wrong channel counts are rejected, not truncated.
    juce::AudioBuffer<float> oversized(2, 1025);
    wrapper.processBlock(oversized, midi);
    REQUIRE(wrapper.processing_diagnostic().has_value());
    CHECK(wrapper.processing_diagnostic()->code == "block_size_unsupported");

    audioalgo::NativeNodeAudioProcessor wrong_layout(std::make_unique<audioalgo::GainNodeAdapter>(), "gain-layout",
                                                     block_descriptor("builtin.gain", "audio_in", "audio_out"),
                                                     {{"gain_db", gain_db}}, {}, 2);
    wrong_layout.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> monophonic(1, 64);
    wrong_layout.processBlock(monophonic, midi);
    REQUIRE(wrong_layout.processing_diagnostic().has_value());
    CHECK(wrong_layout.processing_diagnostic()->code == "block_size_unsupported");
}

TEST_CASE("AR-07.c one instance is prepared, reused across blocks and reset between runs", "[R1][AR-07.c]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    auto probe = std::make_unique<LifecycleProbeProcessor>();
    auto* probe_view = probe.get();
    audioalgo::NativeNodeAudioProcessor wrapper(std::move(probe), "lifecycle-node",
                                                block_descriptor("test.lifecycle", "audio_in", "audio_out"),
                                                {}, {}, 2);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 128);
    (void)fill_block(buffer, 128);

    wrapper.prepareToPlay(48000.0, 128);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    CHECK(probe_view->prepare_calls == 1);
    CHECK(probe_view->prepared_format == (audioalgo::AudioFormat{48000, 2}));
    CHECK(probe_view->prepared_maximum_block == 128);

    for (int block = 0; block < 3; ++block) {
        buffer.clear();
        (void)fill_block(buffer, 128);
        wrapper.processBlock(buffer, midi);
        REQUIRE_FALSE(wrapper.processing_diagnostic());
    }
    CHECK(probe_view->processed_blocks == 3);
    REQUIRE(probe_view->recorded_instances.size() == 3);
    CHECK(std::all_of(probe_view->recorded_instances.begin(), probe_view->recorded_instances.end(),
                      [&](const void* instance) { return instance == probe_view; }));

    // A new run prepares the same instance again instead of reusing incompatible state.
    wrapper.prepareToPlay(48000.0, 64);
    CHECK(probe_view->prepare_calls == 2);
    CHECK(probe_view->prepared_maximum_block == 64);

    wrapper.releaseResources();
    CHECK(probe_view->reset_calls == 1);
    juce::AudioBuffer<float> after_release(2, 32);
    wrapper.processBlock(after_release, midi);
    REQUIRE(wrapper.processing_diagnostic().has_value());
    CHECK(wrapper.processing_diagnostic()->code == "node_not_prepared");
}

TEST_CASE("AR-07.d the prepared block callback allocates nothing", "[R1][AR-07.d]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<audioalgo::GainNodeAdapter>(), "gain-alloc",
                                                block_descriptor("builtin.gain", "audio_in", "audio_out"),
                                                {{"gain_db", -3.0}}, {}, 2);
    wrapper.prepareToPlay(48000.0, 512);
    REQUIRE_FALSE(wrapper.processing_diagnostic());

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    // Warm-up: any one-time JUCE state is established before measurement.
    for (int warmup = 0; warmup < 4; ++warmup) {
        buffer.clear();
        (void)fill_block(buffer, 512);
        wrapper.processBlock(buffer, midi);
    }
    REQUIRE_FALSE(wrapper.processing_diagnostic());

    // Only the wrapper's callback runs inside the measured region; the fixture never allocates there.
    std::size_t allocations = 0;
    {
        const AllocationScope scope;
        for (int block = 0; block < 16; ++block)
            wrapper.processBlock(buffer, midi);
        allocations = scope.count();
    }
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    CHECK(allocations == 0);
}

TEST_CASE("AR-07.e node failures carry node and port context and never fail silently", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 64);
    (void)fill_block(buffer, 64);

    {
        audioalgo::NativeNodeAudioProcessor wrapper(
            std::make_unique<FailingBlockProcessor>(audioalgo::Diagnostic{"dsp_failure", "kernel exploded"}),
            "node-with-error", block_descriptor("test.failing", "audio_in", "audio_out"), {}, {}, 2);
        wrapper.prepareToPlay(48000.0, 64);
        buffer.clear();
        (void)fill_block(buffer, 64);
        wrapper.processBlock(buffer, midi);
        REQUIRE(wrapper.processing_diagnostic().has_value());
        const auto& diagnostic = *wrapper.processing_diagnostic();
        CHECK(diagnostic.code == "dsp_failure");
        CHECK(diagnostic.node_id == "node-with-error");
        CHECK(diagnostic.port_id == "audio_in");
        CHECK(diagnostic.message.find("node-with-error") != std::string::npos);
        CHECK(diagnostic.message.find("audio_in") != std::string::npos);
        CHECK(diagnostic.message.find("kernel exploded") != std::string::npos);
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < 64; ++frame)
                CHECK(buffer.getSample(channel, frame) == 0.0f);
    }

    {
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<ShortBlockProcessor>(), "node-short",
                                                    block_descriptor("test.short", "audio_in", "audio_out"), {}, {}, 2);
        wrapper.prepareToPlay(48000.0, 64);
        buffer.clear();
        (void)fill_block(buffer, 64);
        wrapper.processBlock(buffer, midi);
        REQUIRE(wrapper.processing_diagnostic().has_value());
        CHECK(wrapper.processing_diagnostic()->code == "output_incomplete");
        CHECK(wrapper.processing_diagnostic()->message.find("node-short") != std::string::npos);
    }

    // A descriptor the generic wrapper cannot host is refused before any audio is produced.
    {
        audioalgo::NodeDescriptor two_inputs{"test.two_inputs", "1.0.0", "Two inputs",
                                             {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
                                              {"side_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
                                              {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
                                             {}, audioalgo::NodeExecutionKind::block};
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<PassthroughBlockProcessor>(), "node-two-in",
                                                    two_inputs, {}, {}, 2);
        REQUIRE(wrapper.processing_diagnostic().has_value());
        CHECK(wrapper.processing_diagnostic()->code == "node_wrapper_unsupported");
    }
}

TEST_CASE("AR-06.e unsupported capabilities are refused before execution starts", "[R1][AR-06.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));

    audioalgo::AudioBuffer source(48000, {"left", "right"}, 256);
    for (std::size_t channel = 0; channel < source.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < source.frame_count(); ++frame)
            source.mutable_channel(channel)[frame] = static_cast<float>(0.2 * std::sin(0.01 * frame));

    const auto project_for = [](audioalgo::NodeInstance node) {
        audioalgo::ProjectModel project;
        project.project_id = "ar06e";
        project.name = "capability rejection";
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
                         std::move(node), {"output", "builtin.output", {}, 200.0, 0.0}};
        project.selected_output_node_id = "output";
        return project;
    };

    // Fixed-frame requirement that does not match the requested block size is unavailable.
    {
        auto descriptor = block_descriptor("test.fixed_256", "audio_in", "audio_out");
        descriptor.capabilities.supports_variable_block_size = false;
        descriptor.capabilities.fixed_block_frames = 256;
        audioalgo::NodeRegistry fixed_registry;
        REQUIRE(audioalgo::register_builtin_nodes(fixed_registry));
        REQUIRE(fixed_registry.register_node(descriptor, [] {
            return std::make_unique<PassthroughBlockProcessor>();
        }).registered);
        auto project = project_for({"fixed", "test.fixed_256", {}, 100.0, 0.0});
        project.connections = {{"file", "audio", "fixed", "audio_in"}, {"fixed", "audio_out", "output", "audio"}};
        const auto refused = audioalgo::render_audio_graph(project, fixed_registry, source, 128);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(refused));
        CHECK(std::get<audioalgo::Diagnostic>(refused).code == "capability_unsupported");
        CHECK(std::get<audioalgo::Diagnostic>(refused).node_id == "fixed");

        const auto accepted = audioalgo::render_audio_graph(project, fixed_registry, source, 256);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(accepted));
    }

    // A sample-rate restriction the source cannot satisfy is unavailable.
    {
        auto descriptor = block_descriptor("test.only_16k", "audio_in", "audio_out");
        descriptor.audio_format.sample_rates_hz = {16000};
        audioalgo::NodeRegistry rate_registry;
        REQUIRE(audioalgo::register_builtin_nodes(rate_registry));
        REQUIRE(rate_registry.register_node(descriptor, [] {
            return std::make_unique<PassthroughBlockProcessor>();
        }).registered);
        auto project = project_for({"rate", "test.only_16k", {}, 100.0, 0.0});
        project.connections = {{"file", "audio", "rate", "audio_in"}, {"rate", "audio_out", "output", "audio"}};
        const auto refused = audioalgo::render_audio_graph(project, rate_registry, source, 128);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(refused));
        CHECK(std::get<audioalgo::Diagnostic>(refused).code == "capability_unsupported");
        CHECK(std::get<audioalgo::Diagnostic>(refused).node_id == "rate");
    }

    // A block node with no SDK factory cannot silently pass audio through.
    {
        audioalgo::NodeRegistry missing_registry;
        REQUIRE(audioalgo::register_builtin_nodes(missing_registry));
        REQUIRE(missing_registry.register_descriptor(block_descriptor("test.no_factory", "audio_in", "audio_out")));
        auto project = project_for({"ghost", "test.no_factory", {}, 100.0, 0.0});
        project.connections = {{"file", "audio", "ghost", "audio_in"}, {"ghost", "audio_out", "output", "audio"}};
        const auto refused = audioalgo::render_audio_graph(project, missing_registry, source, 128);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(refused));
        CHECK(std::get<audioalgo::Diagnostic>(refused).code == "node_factory_missing");
        CHECK(std::get<audioalgo::Diagnostic>(refused).node_id == "ghost");
    }
}

TEST_CASE("AR-06.f a new node type runs without touching generic SDK or view branches", "[R1][AR-06.f]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));
    const auto registration = registry.register_node(block_descriptor("test.passthrough_alt", "audio_in", "audio_out"),
                                                    [] { return std::make_unique<PassthroughBlockProcessor>(); });
    REQUIRE(registration.registered);
    REQUIRE_FALSE(registration.diagnostic.has_value());

    audioalgo::AudioBuffer source(48000, {"left", "right"}, 200);
    for (std::size_t channel = 0; channel < source.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < source.frame_count(); ++frame)
            source.mutable_channel(channel)[frame] =
                static_cast<float>((channel == 0 ? 0.3 : -0.2) * std::sin(0.03 * frame));

    audioalgo::ProjectModel project;
    project.project_id = "ar06f";
    project.name = "new type";
    project.nodes = {{"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
                     {"alt", "test.passthrough_alt", {}, 100.0, 0.0},
                     {"output", "builtin.output", {}, 200.0, 0.0}};
    project.connections = {{"file", "audio", "alt", "audio_in"}, {"alt", "audio_out", "output", "audio"}};
    project.selected_output_node_id = "output";
    const auto rendered = audioalgo::render_audio_graph(project, registry, source, 64);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& output = std::get<audioalgo::AudioBuffer>(rendered);
    REQUIRE(output.frame_count() == source.frame_count());
    for (std::size_t channel = 0; channel < source.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < source.frame_count(); ++frame)
            CHECK(output.channel(channel)[frame] == Catch::Approx(source.channel(channel)[frame]).margin(1.0e-7));
}

// ---------------------------------------------------------------------------------------------
// Review framework-20261018T040137637400Z F1/F3 and continuation-...-042655330487Z N1/N2.
// ---------------------------------------------------------------------------------------------
TEST_CASE("AR-07.e block output must be finite and prepare must not throw", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 64);

    const auto run_block_node = [&](std::unique_ptr<audioalgo::AudioNodeProcessor> processor,
                                    const char* node_id) {
        audioalgo::NativeNodeAudioProcessor wrapper(std::move(processor), node_id,
                                                    block_descriptor("test.output_guard", "audio_in", "audio_out"),
                                                    {}, {}, 2);
        wrapper.prepareToPlay(48000.0, 64);
        buffer.clear();
        (void)fill_block(buffer, 64);
        wrapper.processBlock(buffer, midi);
        return wrapper.processing_diagnostic();
    };

    // NaN on the borrowed block path must not become a silent success.
    {
        const auto diagnostic = run_block_node(std::make_unique<NonFiniteBlockProcessor>(0), "node-nan");
        REQUIRE(diagnostic.has_value());
        CHECK(diagnostic->code == "output_non_finite");
        CHECK(diagnostic->message.find("node-nan") != std::string::npos);
        CHECK(diagnostic->message.find("audio_out") != std::string::npos);
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < 64; ++frame)
                CHECK(buffer.getSample(channel, frame) == 0.0f);
    }
    // Positive and negative infinity are equally rejected.
    for (const int mode : {1, 2}) {
        const auto diagnostic = run_block_node(std::make_unique<NonFiniteBlockProcessor>(mode), "node-inf");
        REQUIRE(diagnostic.has_value());
        CHECK(diagnostic->code == "output_non_finite");
    }
    // A finite input whose finite arithmetic overflows to infinity is rejected too.
    {
        const auto diagnostic = run_block_node(std::make_unique<NonFiniteBlockProcessor>(3), "node-overflow");
        REQUIRE(diagnostic.has_value());
        CHECK(diagnostic->code == "output_non_finite");
    }

    // A throwing prepare must be converted into a node-located diagnostic, never escape.
}

namespace {
// Writes finite audio to every output except one poisoned sample in one named output port, so the
// diagnostic must attribute the failure to the port that actually holds the bad sample.
template <typename Port>
const Port* find_named_port(std::span<const Port> ports, std::string_view id) {
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const auto& port) { return port.id == id; });
    return found == ports.end() ? nullptr : &*found;
}
class NamedNonFiniteProcessor final : public audioalgo::AudioNodeProcessor {
public:
    NamedNonFiniteProcessor(std::string poisoned_port, std::size_t poisoned_channel, int mode)
        : poisoned_port_(std::move(poisoned_port)), poisoned_channel_(poisoned_channel), mode_(mode) {}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"named_required", "Use named borrowed ports"};
    }
    bool supports_named_block_processing() const noexcept override { return true; }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override { return std::nullopt; }
    audioalgo::AudioBlockOutcome process_named_block(const audioalgo::NamedAudioBlockView& block) override {
        const auto* input = find_named_port(block.inputs, "input");
        const auto* main = find_named_port(block.outputs, "main");
        const auto* side = find_named_port(block.outputs, "side");
        if (!input || !main || !side || input->channels.size() != 2 || main->channels.size() != 2 || side->channels.size() != 2)
            return audioalgo::Diagnostic{"named_port_shape", "Split port mapping is wrong"};
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t f = 0; f < block.frame_count; ++f) {
                main->channels[c][f] = 2.0f * input->channels[c][f];
                side->channels[c][f] = -0.25f * input->channels[c][f];
            }
        if (poison_) {
            const auto* target = poisoned_port_ == "main" ? main : side;
            switch (mode_) {
            case 0: target->channels[poisoned_channel_][1] = std::numeric_limits<float>::quiet_NaN(); break;
            case 1: target->channels[poisoned_channel_][1] = std::numeric_limits<float>::infinity(); break;
            default: target->channels[poisoned_channel_][1] = -std::numeric_limits<float>::infinity(); break;
            }
        }
        return audioalgo::AudioBlockResult{block.frame_count};
    }
    bool poison_ = true;
private:
    std::string poisoned_port_;
    std::size_t poisoned_channel_;
    int mode_ = 0;
};
} // namespace

// Review progress-20261021 F1: a non-finite sample on the second output must be reported on that
// port, not on the descriptor's first output; healthy outputs are never blamed.
TEST_CASE("AR-07.e multi-output non-finite failures identify the poisoned output port", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI messages;
    audioalgo::NodeDescriptor descriptor{"test.named_nonfinite", "1.0.0", "Split",
        {{"input", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"main", "audio.pcm@1", audioalgo::PortDirection::output, false},
         {"side", "audio.pcm@1", audioalgo::PortDirection::output, false}}, {}, audioalgo::NodeExecutionKind::block};
    descriptor.capabilities.supports_named_audio_ports = true;
    for (const std::string poisoned_port : {"main", "side"})
        for (const std::size_t poisoned_channel : {std::size_t{0}, std::size_t{1}})
            for (const int mode : {0, 1, 2}) {
                CAPTURE(poisoned_port, poisoned_channel, mode);
                auto processor = std::make_unique<NamedNonFiniteProcessor>(poisoned_port, poisoned_channel, mode);
                auto* probe = processor.get();
                audioalgo::NativeNodeAudioProcessor wrapper(std::move(processor), "bad-second-output",
                                                             descriptor, {}, {}, 2);
                wrapper.prepareToPlay(48000.0, 64);
                REQUIRE_FALSE(wrapper.processing_diagnostic());
                juce::AudioBuffer<float> buffer(4, 64);
                juce::MidiBuffer midi;
                for (int c = 0; c < 2; ++c)
                    for (int f = 0; f < 64; ++f)
                        buffer.setSample(c, f, static_cast<float>((f + 5 * c) % 13 - 6) / 64.0f);
                wrapper.processBlock(buffer, midi);
                REQUIRE(wrapper.processing_diagnostic().has_value());
                CHECK(wrapper.processing_diagnostic()->code == "output_non_finite");
                CHECK(wrapper.processing_diagnostic()->node_id == "bad-second-output");
                CHECK(wrapper.processing_diagnostic()->port_id == poisoned_port);
                for (int channel = 0; channel < 4; ++channel)
                    for (int frame = 0; frame < 64; ++frame)
                        CHECK(buffer.getSample(channel, frame) == 0.0f);

                // Recovery: the same instance is usable again after a fresh prepare without the poison.
                probe->poison_ = false;
                wrapper.prepareToPlay(48000.0, 64);
                REQUIRE_FALSE(wrapper.processing_diagnostic());
                for (int c = 0; c < 2; ++c)
                    for (int f = 0; f < 64; ++f)
                        buffer.setSample(c, f, static_cast<float>((f + 5 * c) % 13 - 6) / 64.0f);
                wrapper.processBlock(buffer, midi);
                REQUIRE_FALSE(wrapper.processing_diagnostic());
                for (int c = 0; c < 2; ++c)
                    for (int f = 0; f < 64; ++f) {
                        const auto original = static_cast<float>((f + 5 * c) % 13 - 6) / 64.0f;
                        CHECK(buffer.getSample(c, f) == original * 2.0f);
                        CHECK(std::isfinite(buffer.getSample(c + 2, f)));
                    }
            }
}

TEST_CASE("AR-07.e a throwing prepare becomes a node-located diagnostic", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 64);
    (void)fill_block(buffer, 64);
    {
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<ThrowingPrepareProcessor>(), "node-prepare",
                                                    block_descriptor("test.prepare_throw", "audio_in", "audio_out"),
                                                    {}, {}, 2);
        CHECK_NOTHROW(wrapper.prepareToPlay(48000.0, 64));
        REQUIRE(wrapper.processing_diagnostic().has_value());
        CHECK(wrapper.processing_diagnostic()->code == "node_prepare_exception");
        CHECK(wrapper.processing_diagnostic()->node_id == "node-prepare");
        CHECK(wrapper.processing_diagnostic()->port_id.empty());
        CHECK(wrapper.processing_diagnostic()->message.find("node-prepare") != std::string::npos);
        CHECK(wrapper.processing_diagnostic()->message.find("plugin prepare failed") != std::string::npos);
        buffer.clear();
        (void)fill_block(buffer, 64);
        CHECK_NOTHROW(wrapper.processBlock(buffer, midi));
        REQUIRE(wrapper.processing_diagnostic().has_value());
    }
}

TEST_CASE("AR-07.d the prepared callback allocates nothing for generic nodes too", "[R1][AR-07.d]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 256);
    (void)fill_block(buffer, 256);

    // A non-Gain block node must satisfy the same no-allocation contract as Gain.
    {
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<PassthroughBlockProcessor>(), "generic-block",
                                                    block_descriptor("test.generic_block", "audio_in", "audio_out"),
                                                    {}, {}, 2);
        wrapper.prepareToPlay(48000.0, 256);
        REQUIRE_FALSE(wrapper.processing_diagnostic());
        for (int warmup = 0; warmup < 4; ++warmup)
            wrapper.processBlock(buffer, midi);
        REQUIRE_FALSE(wrapper.processing_diagnostic());

        std::size_t allocations = 0;
        {
            const AllocationScope scope;
            for (int block = 0; block < 16; ++block)
                wrapper.processBlock(buffer, midi);
            allocations = scope.count();
        }
        REQUIRE_FALSE(wrapper.processing_diagnostic());
        CHECK(allocations == 0);
    }

    // An owned-only node has no allocation-free path, so the native callback must refuse it up front.
    {
        audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<OwnedOnlyProcessor>(), "owned-only",
                                                    block_descriptor("test.owned_only", "audio_in", "audio_out"),
                                                    {}, {}, 2);
        CHECK_NOTHROW(wrapper.prepareToPlay(48000.0, 256));
        REQUIRE(wrapper.processing_diagnostic().has_value());
        CHECK(wrapper.processing_diagnostic()->code == "block_processing_unsupported");
        CHECK(wrapper.processing_diagnostic()->message.find("owned-only") != std::string::npos);

        buffer.clear();
        (void)fill_block(buffer, 256);
        std::size_t allocations = 0;
        {
            const AllocationScope scope;
            wrapper.processBlock(buffer, midi);
            allocations = scope.count();
        }
        CHECK(allocations == 0);
        REQUIRE(wrapper.processing_diagnostic().has_value());
    }
}

TEST_CASE("AR-06.e fixed-frame tails and non-audio ports are refused before execution", "[R1][AR-06.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    audioalgo::AudioBuffer source(48000, {"left", "right"}, 257);
    for (std::size_t channel = 0; channel < source.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < source.frame_count(); ++frame)
            source.mutable_channel(channel)[frame] = static_cast<float>(0.2 * std::sin(0.01 * frame));

    audioalgo::NodeRegistry registry;
    REQUIRE(audioalgo::register_builtin_nodes(registry));

    auto fixed_descriptor = block_descriptor("test.fixed_256_tail", "audio_in", "audio_out");
    fixed_descriptor.capabilities.supports_variable_block_size = false;
    fixed_descriptor.capabilities.fixed_block_frames = 256;
    REQUIRE(registry.register_node(fixed_descriptor, [] {
        return std::make_unique<PassthroughBlockProcessor>();
    }).registered);

    const auto project_for = [](std::string type_id) {
        audioalgo::ProjectModel project;
        project.project_id = "ar06e-contracts";
        project.name = "fixed frame and port types";
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("unused.wav")}}, 0.0, 0.0},
                         {"node", std::move(type_id), {}, 100.0, 0.0},
                         {"output", "builtin.output", {}, 200.0, 0.0}};
        project.connections = {{"file", "audio", "node", "audio_in"}, {"node", "audio_out", "output", "audio"}};
        project.selected_output_node_id = "output";
        return project;
    };

    // 257 frames cannot be delivered as whole 256-frame blocks, so the run must be refused.
    const auto short_tail = audioalgo::render_audio_graph(project_for("test.fixed_256_tail"), registry, source, 256);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(short_tail));
    CHECK(std::get<audioalgo::Diagnostic>(short_tail).code == "capability_unsupported");
    CHECK(std::get<audioalgo::Diagnostic>(short_tail).message.find("fixed") != std::string::npos);

    // A whole number of fixed blocks is accepted.
    audioalgo::AudioBuffer exact(48000, {"left", "right"}, 512);
    const auto aligned = audioalgo::render_audio_graph(project_for("test.fixed_256_tail"), registry, exact, 256);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(aligned));
}

TEST_CASE("AR-07.e non-audio ports are refused by the native boundary", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI message_manager;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(2, 32);
    (void)fill_block(buffer, 32);

    // The wrapper itself must reject a descriptor whose ports are not PCM audio.
    audioalgo::NodeDescriptor geometry{"test.geometry_block", "1.0.0", "Geometry block",
                                       {{"geometry_in", "array.geometry@1", audioalgo::PortDirection::input, true},
                                        {"geometry_out", "array.geometry@1", audioalgo::PortDirection::output, false}},
                                       {}, audioalgo::NodeExecutionKind::block};
    audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<PassthroughBlockProcessor>(), "geometry-node",
                                                geometry, {}, {}, 2);
    REQUIRE(wrapper.processing_diagnostic().has_value());
    CHECK(wrapper.processing_diagnostic()->code == "port_type_unsupported");
    CHECK(wrapper.processing_diagnostic()->message.find("geometry") != std::string::npos);

    // A whole geometry-typed graph must not be rendered as PCM audio.
    audioalgo::NodeRegistry registry;
    REQUIRE(registry.register_descriptor({"test.geometry_source", "1.0.0", "Geometry source",
                                          {{"geometry", "array.geometry@1", audioalgo::PortDirection::output, false}},
                                          {}, audioalgo::NodeExecutionKind::source}));
    REQUIRE(registry.register_descriptor({"test.geometry_sink", "1.0.0", "Geometry sink",
                                          {{"geometry", "array.geometry@1", audioalgo::PortDirection::input, true}},
                                          {}, audioalgo::NodeExecutionKind::sink}));
    REQUIRE(registry.register_node(geometry, [] { return std::make_unique<PassthroughBlockProcessor>(); }).registered);

    audioalgo::ProjectModel project;
    project.project_id = "n2-geometry";
    project.name = "geometry graph";
    project.nodes = {{"source", "test.geometry_source", {}, 0.0, 0.0},
                     {"node", "test.geometry_block", {}, 100.0, 0.0},
                     {"sink", "test.geometry_sink", {}, 200.0, 0.0}};
    project.connections = {{"source", "geometry", "node", "geometry_in"},
                           {"node", "geometry_out", "sink", "geometry"}};
    project.selected_output_node_id = "sink";
    audioalgo::AudioBuffer source(48000, {"left", "right"}, 64);
    const auto refused = audioalgo::render_audio_graph(project, registry, source, 64);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(refused));
    CHECK(std::get<audioalgo::Diagnostic>(refused).code == "port_type_unsupported");
}

namespace {
class ContextRecoveryProcessor final : public audioalgo::AudioNodeProcessor {
public:
    bool fail_prepare = true;
    bool fail_block = false;
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"block_required", "Controlled fixture uses borrowed blocks"};
    }
    bool supports_block_processing() const noexcept override { return true; }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override {
        if (fail_prepare)
            return audioalgo::Diagnostic{"coefficient_invalid", "Controlled coefficient failure", "foreign-node", "coefficient"};
        return std::nullopt;
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        if (fail_block)
            return audioalgo::Diagnostic{"controlled_output_failure", "Controlled output failure", "foreign-node", "audio_out"};
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            std::copy_n(block.input_channels[channel], block.frame_count, block.output_channels[channel]);
        return audioalgo::AudioBlockResult{block.frame_count};
    }
};
}

TEST_CASE("AR-07.e structured failures preserve host identity parameter ports and recovery", "[R1][AR-07.e]") {
    juce::ScopedJuceInitialiser_GUI messages;
    auto processor = std::make_unique<ContextRecoveryProcessor>();
    auto* probe = processor.get();
    audioalgo::NativeNodeAudioProcessor wrapper(std::move(processor), "host-instance",
        block_descriptor("test.context_recovery", "audio_in", "audio_out"), {}, {}, 2);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    wrapper.prepareToPlay(48000.0, 64);
    REQUIRE(wrapper.processing_diagnostic().has_value());
    CHECK(wrapper.processing_diagnostic()->code == "coefficient_invalid");
    CHECK(wrapper.processing_diagnostic()->node_id == "host-instance");
    CHECK(wrapper.processing_diagnostic()->port_id == "coefficient");
    CHECK(wrapper.processing_diagnostic()->message.find("host-instance") != std::string::npos);
    probe->fail_prepare = false;
    wrapper.prepareToPlay(48000.0, 64);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    auto expected = fill_block(buffer, 64);
    wrapper.processBlock(buffer, midi);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 64; ++frame)
            CHECK(buffer.getSample(channel, frame) == expected[channel][frame]);
    probe->fail_block = true;
    (void)fill_block(buffer, 64);
    wrapper.processBlock(buffer, midi);
    REQUIRE(wrapper.processing_diagnostic().has_value());
    CHECK(wrapper.processing_diagnostic()->code == "controlled_output_failure");
    CHECK(wrapper.processing_diagnostic()->node_id == "host-instance");
    CHECK(wrapper.processing_diagnostic()->port_id == "audio_out");
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 64; ++frame)
            CHECK(buffer.getSample(channel, frame) == 0.0f);
    probe->fail_block = false;
    wrapper.prepareToPlay(48000.0, 64);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    expected = fill_block(buffer, 64);
    wrapper.processBlock(buffer, midi);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 64; ++frame)
            CHECK(buffer.getSample(channel, frame) == expected[channel][frame]);
}

TEST_CASE("AR-14.a unsupported native plans are rejected before constructing processors", "[R5][AR-14.a]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for (const std::string kind : {"typed", "rate", "fixed"}) {
        INFO(kind);
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto descriptor = block_descriptor("test.native_admission", "audio_in", "audio_out");
        if (kind == "typed")
            descriptor.ports.push_back({"metric_out", "metrics.scalar@1", audioalgo::PortDirection::output, false});
        else if (kind == "rate")
            descriptor.audio_format.sample_rates_hz = {16000};
        else {
            descriptor.capabilities.supports_variable_block_size = false;
            descriptor.capabilities.fixed_block_frames = 128;
        }
        int constructions = 0;
        REQUIRE(nodes.register_node(descriptor, [&] {
            ++constructions;
            return std::make_unique<PassthroughBlockProcessor>();
        }).registered);
        audioalgo::ProjectModel project;
        project.project_id = "native-admission";
        project.nodes = {{"file", "builtin.file_source", {{"path", std::string("not-read-by-the-native-region.wav")}}},
                         {"typed-node", descriptor.type_id, {}}, {"output", "builtin.output", {}}};
        project.connections = {{"file", "audio", "typed-node", "audio_in"},
                               {"typed-node", "audio_out", "output", "audio"}};
        project.selected_output_node_id = "output";
        audioalgo::AudioBuffer input(48000, {"left", "right"}, 64);
        const auto result = audioalgo::render_audio_graph(project, nodes, input, 64);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&result);
        REQUIRE(error != nullptr);
        CHECK(error->code == (kind == "typed" ? "port_type_unsupported" : "capability_unsupported"));
        CHECK(error->node_id == "typed-node");
        CHECK(error->port_id == (kind == "typed" ? "metric_out" : ""));
        CHECK(constructions == 0);
    }
}

namespace {
struct NamedPortCounters {
    int constructions = 0, prepares = 0, blocks = 0;
    std::size_t frames = 0;
};
template <typename Port>
const Port* named_port(std::span<const Port> ports, std::string_view id) {
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const auto& port) { return port.id == id; });
    return found == ports.end() ? nullptr : &*found;
}
class NamedSplitProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit NamedSplitProcessor(std::shared_ptr<NamedPortCounters> counters) : counters_(std::move(counters)) { ++counters_->constructions; }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override { return audioalgo::Diagnostic{"named_required", "Use named borrowed ports"}; }
    bool supports_named_block_processing() const noexcept override { return true; }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override { ++counters_->prepares; return std::nullopt; }
    audioalgo::AudioBlockOutcome process_named_block(const audioalgo::NamedAudioBlockView& block) override {
        const auto* input = named_port(block.inputs, "input");
        const auto* main = named_port(block.outputs, "main");
        const auto* side = named_port(block.outputs, "side");
        if (!input || !main || !side || input->channels.size()!=2 || main->channels.size()!=2 || side->channels.size()!=2)
            return audioalgo::Diagnostic{"named_port_shape", "Split port mapping is wrong"};
        ++counters_->blocks; counters_->frames+=block.frame_count;
        for (std::size_t c=0;c<2;++c)
            for (std::size_t f=0;f<block.frame_count;++f) {
                main->channels[c][f]=2.0f*input->channels[c][f];
                side->channels[c][f]=-0.25f*input->channels[c][f];
            }
        return audioalgo::AudioBlockResult{block.frame_count};
    }
private:
    std::shared_ptr<NamedPortCounters> counters_;
};
class NamedMixProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit NamedMixProcessor(std::shared_ptr<NamedPortCounters> counters) : counters_(std::move(counters)) { ++counters_->constructions; }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override { return audioalgo::Diagnostic{"named_required", "Use named borrowed ports"}; }
    bool supports_named_block_processing() const noexcept override { return true; }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override { ++counters_->prepares; cursor_=0; return std::nullopt; }
    audioalgo::AudioBlockOutcome process_named_block(const audioalgo::NamedAudioBlockView& block) override {
        const auto* primary = named_port(block.inputs, "primary");
        const auto* reference = named_port(block.inputs, "reference");
        const auto* output = named_port(block.outputs, "sum");
        if (!primary || !reference || !output || primary->channels.size()!=2 || reference->channels.size()!=2 || output->channels.size()!=2)
            return audioalgo::Diagnostic{"named_port_shape", "Mix port mapping is wrong"};
        ++counters_->blocks; counters_->frames+=block.frame_count;
        for (std::size_t c=0;c<2;++c)
            for (std::size_t f=0;f<block.frame_count;++f)
                output->channels[c][f]=2.0f*primary->channels[c][f]+3.0f*reference->channels[c][f]+static_cast<float>(cursor_+f)/4096.0f;
        cursor_+=block.frame_count;
        return audioalgo::AudioBlockResult{block.frame_count};
    }
private:
    std::shared_ptr<NamedPortCounters> counters_;
    std::size_t cursor_ = 0;
};
audioalgo::NodeDescriptor named_split_descriptor() {
    audioalgo::NodeDescriptor descriptor{"test.named_split","1.0.0","Split",
        {{"input","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"main","audio.pcm@1",audioalgo::PortDirection::output,false},
         {"side","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}, audioalgo::NodeExecutionKind::block};
    descriptor.capabilities.supports_named_audio_ports=true;
    descriptor.audio_format.channel_counts={2};
    return descriptor;
}
audioalgo::NodeDescriptor named_mix_descriptor() {
    audioalgo::NodeDescriptor descriptor{"test.named_mix","1.0.0","Mix",
        {{"primary","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"reference","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"sum","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}, audioalgo::NodeExecutionKind::block};
    descriptor.capabilities.supports_named_audio_ports=true;
    descriptor.audio_format.channel_counts={2};
    return descriptor;
}
audioalgo::ProjectModel named_port_project(bool swapped) {
    audioalgo::ProjectModel project;
    project.project_id="named-native-ports";
    project.nodes={{"source","builtin.file_source",{{"path",std::string("input.wav")}}},
                   {"split","test.named_split",{}},{"mix","test.named_mix",{}},{"output","builtin.output",{}}};
    project.selected_output_node_id="output";
    project.connections={{"source","audio","split","input"},
        {"split",swapped?"side":"main","mix","primary"},
        {"split",swapped?"main":"side","mix","reference"},
        {"mix","sum","output","audio"}};
    return project;
}
}

TEST_CASE("AR-14.b named split and combine ports preserve roles channels and state", "[R5][AR-14.b][native_integration]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for (const std::size_t frames : {0u,1u,129u,513u})
        for (const int host : {1,127,256})
            for (const bool swapped : {false,true}) {
                CAPTURE(frames,host,swapped);
                audioalgo::NodeRegistry nodes;
                REQUIRE(audioalgo::register_builtin_nodes(nodes));
                auto split_state=std::make_shared<NamedPortCounters>();
                auto mix_state=std::make_shared<NamedPortCounters>();
                const auto split=named_split_descriptor();
                const auto mix=named_mix_descriptor();
                REQUIRE(nodes.register_node(split,[=]{return std::make_unique<NamedSplitProcessor>(split_state);}).registered);
                REQUIRE(nodes.register_node(mix,[=]{return std::make_unique<NamedMixProcessor>(mix_state);}).registered);
                auto project=named_port_project(swapped);
                audioalgo::AudioBuffer input(48000,{"left","right"},frames);
                for (std::size_t c=0;c<2;++c)
                    for (std::size_t f=0;f<frames;++f)
                        input.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
                const auto result=audioalgo::render_audio_graph(project,nodes,input,host);
                INFO((std::holds_alternative<audioalgo::Diagnostic>(result)?std::get<audioalgo::Diagnostic>(result).message:std::string("success")));
                const auto* output=std::get_if<audioalgo::AudioBuffer>(&result);
                REQUIRE(output!=nullptr);
                CHECK(output->frame_count()==frames);
                CHECK(output->channel_ids()==input.channel_ids());
                for (std::size_t c=0;c<2;++c)
                    for (std::size_t f=0;f<frames;++f) {
                        const auto original=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
                        const auto expected=original*(swapped?5.5f:3.25f)+static_cast<float>(f)/4096.0f;
                        CHECK(output->channel(c)[f]==expected);
                        CHECK(input.channel(c)[f]==original);
                    }
                for (const auto& state : {split_state,mix_state}) {
                    CHECK(state->constructions==1);
                    CHECK(state->prepares==1);
                    CHECK(state->blocks==static_cast<int>((frames+host-1)/host));
                    CHECK(state->frames==frames);
                }
            }
}

TEST_CASE("AR-14.b named borrowed callbacks allocate nothing and preserve staged inputs", "[R5][AR-14.b]") {
    juce::ScopedJuceInitialiser_GUI messages;
    auto counters=std::make_shared<NamedPortCounters>();
    audioalgo::NodeDescriptor descriptor{"test.named_allocation","1.0.0","Named split",
        {{"input","audio.pcm@1",audioalgo::PortDirection::input,true},
         {"main","audio.pcm@1",audioalgo::PortDirection::output,false},
         {"side","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}, audioalgo::NodeExecutionKind::block};
    descriptor.capabilities.supports_named_audio_ports=true;
    audioalgo::NativeNodeAudioProcessor wrapper(std::make_unique<NamedSplitProcessor>(counters),
        "named-allocation",descriptor,{}, {},2);
    wrapper.prepareToPlay(48000.0,128);
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    juce::AudioBuffer<float> buffer(4,128);
    juce::MidiBuffer midi;
    const auto fill=[&] {
        for (int c=0;c<2;++c)
            for (int f=0;f<128;++f)
                buffer.setSample(c,f,static_cast<float>((f+3*c)%17-8)/64.0f);
    };
    for (int warmup=0;warmup<4;++warmup) { fill();wrapper.processBlock(buffer,midi); }
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    std::size_t allocations=0;
    {
        AllocationScope scope;
        for (int block=0;block<16;++block) { fill();wrapper.processBlock(buffer,midi); }
        allocations=scope.count();
    }
    REQUIRE_FALSE(wrapper.processing_diagnostic());
    CHECK(allocations==0);
    for (int c=0;c<2;++c)
        for (int f=0;f<128;++f) {
            const auto original=static_cast<float>((f+3*c)%17-8)/64.0f;
            CHECK(buffer.getSample(c,f)==original*2.0f);
            CHECK(buffer.getSample(c+2,f)==original*-0.25f);
        }
}


TEST_CASE("AR-14.b named port plans render real WAV artifacts through RunService", "[R5][AR-14.b]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::current_path()/".audioalgo-test-tmp"/("named-file-"+juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
    audioalgo::AudioBuffer input(48000,{"left","right"},513);
    for (std::size_t c=0;c<2;++c)
        for (std::size_t f=0;f<513;++f)
            input.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
    const auto input_file=root/"input.wav";
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(input_file,input,audioalgo::WavEncoding::float32)));
    for (const bool swapped:{false,true}) {
        const std::string name=swapped?"swapped":"normal";
        INFO(name);
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto split_state=std::make_shared<NamedPortCounters>();
        auto mix_state=std::make_shared<NamedPortCounters>();
        REQUIRE(nodes.register_node(named_split_descriptor(),[=]{return std::make_unique<NamedSplitProcessor>(split_state);}).registered);
        REQUIRE(nodes.register_node(named_mix_descriptor(),[=]{return std::make_unique<NamedMixProcessor>(mix_state);}).registered);
        const auto project=named_port_project(swapped);
        const auto project_file=root/(name+".aaproj");
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
        audioalgo::RunService service(nodes);
        const auto result=service.run({project,project_file,root/(name+"-run"),127});
        INFO((std::holds_alternative<audioalgo::Diagnostic>(result)?std::get<audioalgo::Diagnostic>(result).message:std::string("success")));
        const auto* success=std::get_if<audioalgo::RunSuccess>(&result);
        REQUIRE(success!=nullptr);
        const auto read=audioalgo::read_wav_file(success->output_file);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(read));
        const auto& output=std::get<audioalgo::AudioBuffer>(read);
        CHECK(output.frame_count()==513);
        for (std::size_t c=0;c<2;++c)
            for (std::size_t f=0;f<513;++f)
                CHECK(output.channel(c)[f]==input.channel(c)[f]*(swapped?5.5f:3.25f)+static_cast<float>(f)/4096.0f);
        for (const auto& state:{split_state,mix_state}) {
            CHECK(state->constructions==1);CHECK(state->prepares==1);CHECK(state->blocks==5);CHECK(state->frames==513);
        }
        if (const auto* location=std::getenv("AUDIOALGO_NAMED_PORT_ARTIFACT_OUTPUT");location&&*location) {
            const auto exported=std::filesystem::u8path(location)/name;
            REQUIRE_FALSE(std::filesystem::exists(exported));
            std::filesystem::create_directories(exported);
            REQUIRE(std::filesystem::copy_file(input_file,exported/"input.wav"));
            REQUIRE(std::filesystem::copy_file(project_file,exported/"project.aaproj"));
            std::filesystem::copy(root/(name+"-run"),exported/"run",std::filesystem::copy_options::recursive);
        }
    }
}

TEST_CASE("AR-14.b independent WAV sources preserve named roles and replay both versions", "[R5][AR-14.b]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::current_path()/".audioalgo-test-tmp"/("independent-sources-"+juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
    audioalgo::AudioBuffer a(48000,{"left","right"},513),b(48000,{"left","right"},513);
    for (std::size_t c=0;c<2;++c)
        for (std::size_t f=0;f<513;++f) {
            a.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
            b.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((11*f+5*c)%31)-15)/128.0f;
        }
    for (const int host:{1,127,256})
        for (const bool swapped:{false,true}) {
            CAPTURE(host,swapped);
            // Each iteration deletes the caller's originals before replaying, so restore them
            // for the next combination.
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"a.wav",a,audioalgo::WavEncoding::float32)));
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"b.wav",b,audioalgo::WavEncoding::float32)));
            audioalgo::NodeRegistry nodes;
            REQUIRE(audioalgo::register_builtin_nodes(nodes));
            auto state=std::make_shared<NamedPortCounters>();
            REQUIRE(nodes.register_node(named_mix_descriptor(),[=]{return std::make_unique<NamedMixProcessor>(state);}).registered);
            audioalgo::ProjectModel project;
            project.project_id="independent-source-roles";project.selected_output_node_id="output";
            project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},
                           {"b","builtin.file_source",{{"path",std::string("b.wav")}}},
                           {"mix","test.named_mix",{}},{"output","builtin.output",{}}};
            project.connections={{swapped?"b":"a","audio","mix","primary"},
                                 {swapped?"a":"b","audio","mix","reference"},{"mix","sum","output","audio"}};
            const auto project_file=root/"project.aaproj";
            REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
            audioalgo::RunService service(nodes);
            const auto run_dir=root/(std::to_string(host)+(swapped?"-swapped":"-normal"));
            const auto result=service.run({project,project_file,run_dir,host});
            INFO((std::holds_alternative<audioalgo::Diagnostic>(result)?std::get<audioalgo::Diagnostic>(result).code+": "+std::get<audioalgo::Diagnostic>(result).message:std::string("success")));
            const auto* success=std::get_if<audioalgo::RunSuccess>(&result);
            REQUIRE(success!=nullptr);
            const auto decoded=audioalgo::read_wav_file(success->output_file);
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(decoded));
            const auto& output=std::get<audioalgo::AudioBuffer>(decoded);
            REQUIRE(output.frame_count()==513);
            for (std::size_t c=0;c<2;++c)
                for (std::size_t f=0;f<513;++f) {
                    const float primary=swapped?b.channel(c)[f]:a.channel(c)[f];
                    const float reference=swapped?a.channel(c)[f]:b.channel(c)[f];
                    CHECK(output.channel(c)[f]==2.0f*primary+3.0f*reference+static_cast<float>(f)/4096.0f);
                }
            CHECK(state->constructions==1);CHECK(state->prepares==1);
            CHECK(state->blocks==static_cast<int>((513+host-1)/host));CHECK(state->frames==513);
            const auto manifest=juce::JSON::parse(juce::File(success->manifest_file.string()).loadFileAsString());
            REQUIRE(manifest["resources"].isArray());CHECK(manifest["resources"].getArray()->size()==2);
            const auto replay=audioalgo::load_project_file(run_dir/"replay.aaproj");
            REQUIRE(std::holds_alternative<audioalgo::ProjectModel>(replay));
            std::filesystem::path export_root;
            if (const auto* location=std::getenv("AUDIOALGO_INDEPENDENT_SOURCE_ARTIFACT_OUTPUT");location&&*location) {
                export_root=std::filesystem::u8path(location)/run_dir.filename();
                REQUIRE_FALSE(std::filesystem::exists(export_root));std::filesystem::create_directories(export_root);
                for (const auto& file:{"a.wav","b.wav","project.aaproj"})REQUIRE(std::filesystem::copy_file(root/file,export_root/file));
                std::filesystem::copy(run_dir,export_root/"run",std::filesystem::copy_options::recursive);
            }
            // Replay must depend only on the captured resource copies, never on the caller's
            // mutable originals: delete them (and the editable project file) before replaying.
            for (const auto& file:{"a.wav","b.wav","project.aaproj"})
                REQUIRE(std::filesystem::remove(root/file));
            const auto repeated=service.run({std::get<audioalgo::ProjectModel>(replay),run_dir/"replay.aaproj",root/(run_dir.filename().string()+"-replay"),host});
            REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(repeated));
            const auto again=audioalgo::read_wav_file(std::get<audioalgo::RunSuccess>(repeated).output_file);
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(again));
            for (std::size_t c=0;c<2;++c)
                CHECK(std::equal(output.channel(c).begin(),output.channel(c).end(),std::get<audioalgo::AudioBuffer>(again).channel(c).begin()));
            if (!export_root.empty())
                std::filesystem::copy(root/(run_dir.filename().string()+"-replay"),export_root/"replay",std::filesystem::copy_options::recursive);
        }
}

// T25 negative contract: independent sources that disagree on format, frame count, channel
// identity or valid interval are rejected before any block factory or prepare runs, and the
// legacy single-buffer entry refuses multi-source projects instead of duplicating its buffer.
TEST_CASE("AR-14.b mismatched independent sources are rejected before factories", "[R5][AR-14.b]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::current_path()/".audioalgo-test-tmp"/("independent-sources-mismatch-"+juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
    const auto make_source_project=[&] {
        audioalgo::ProjectModel project;
        project.project_id="independent-source-mismatch";project.selected_output_node_id="output";
        project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},
                       {"b","builtin.file_source",{{"path",std::string("b.wav")}}},
                       {"mix","test.named_mix",{}},{"output","builtin.output",{}}};
        project.connections={{"a","audio","mix","primary"},{"b","audio","mix","reference"},{"mix","sum","output","audio"}};
        return project;
    };
    audioalgo::AudioBuffer a(48000,{"left","right"},513),b(48000,{"left","right"},513);
    for (std::size_t c=0;c<2;++c)
        for (std::size_t f=0;f<513;++f) {
            a.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
            b.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((11*f+5*c)%31)-15)/128.0f;
        }
    struct Mismatch {const char* kind;audioalgo::AudioBuffer variant;std::string code;};
    std::vector<Mismatch> wav_mismatches;
    wav_mismatches.push_back({"rate",audioalgo::AudioBuffer(16000,{"left","right"},513),"source_format_mismatch"});
    wav_mismatches.push_back({"frames",audioalgo::AudioBuffer(48000,{"left","right"},512),"source_frame_count_mismatch"});
    wav_mismatches.push_back({"channels",audioalgo::AudioBuffer(48000,{"mono"},513),"source_channel_mismatch"});
    for (const auto& mismatch:wav_mismatches) {
        CAPTURE(mismatch.kind);
        for (std::size_t c=0;c<2;++c)
            for (std::size_t f=0;f<513;++f)
                a.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"a.wav",a,audioalgo::WavEncoding::float32)));
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"b.wav",mismatch.variant,audioalgo::WavEncoding::float32)));
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto state=std::make_shared<NamedPortCounters>();
        REQUIRE(nodes.register_node(named_mix_descriptor(),[=]{return std::make_unique<NamedMixProcessor>(state);}).registered);
        const auto project=make_source_project();
        const auto project_file=root/"project.aaproj";
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
        audioalgo::RunService service(nodes);
        const auto run_dir=root/(std::string("reject-")+mismatch.kind);
        const auto result=service.run({project,project_file,run_dir,127});
        const auto* error=std::get_if<audioalgo::Diagnostic>(&result);
        REQUIRE(error!=nullptr);
        CHECK(error->code==mismatch.code);
        CHECK(error->node_id=="b");
        CHECK(error->port_id=="audio");
        CHECK(state->constructions==0);
        CHECK(state->prepares==0);
        CHECK_FALSE(std::filesystem::exists(run_dir));
        REQUIRE(std::filesystem::remove(root/"a.wav"));
        REQUIRE(std::filesystem::remove(root/"b.wav"));
    }

    // Bindings that only disagree on channel identity or valid interval cannot be expressed as
    // WAV files, so the binding boundary itself must reject them before factories.
    struct BindingMismatch {const char* kind;audioalgo::AudioBuffer variant;std::string code;};
    const std::vector<BindingMismatch> binding_mismatches{
        {"channel_ids",audioalgo::AudioBuffer(48000,{"ch0","ch1"},513),"source_channel_mismatch"},
        {"interval",audioalgo::AudioBuffer(48000,{"left","right"},513,audioalgo::AudioInterval{0,512}),"source_interval_mismatch"}};
    for (const auto& mismatch:binding_mismatches) {
        CAPTURE(mismatch.kind);
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto state=std::make_shared<NamedPortCounters>();
        REQUIRE(nodes.register_node(named_mix_descriptor(),[=]{return std::make_unique<NamedMixProcessor>(state);}).registered);
        const auto project=make_source_project();
        const auto result=audioalgo::render_audio_graph(project,nodes,
            std::vector<audioalgo::AudioSourceBinding>{{"a",a},{"b",mismatch.variant}},127);
        const auto* error=std::get_if<audioalgo::Diagnostic>(&result);
        REQUIRE(error!=nullptr);
        CHECK(error->code==mismatch.code);
        CHECK(error->node_id=="b");
        CHECK(error->port_id=="audio");
        CHECK(state->constructions==0);
        CHECK(state->prepares==0);
    }

    // The legacy single-buffer entry must not guess: with two sources it reports ambiguity
    // instead of feeding the same buffer to both.
    {
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        auto state=std::make_shared<NamedPortCounters>();
        REQUIRE(nodes.register_node(named_mix_descriptor(),[=]{return std::make_unique<NamedMixProcessor>(state);}).registered);
        const auto project=make_source_project();
        const auto result=audioalgo::render_audio_graph(project,nodes,a,127);
        const auto* error=std::get_if<audioalgo::Diagnostic>(&result);
        REQUIRE(error!=nullptr);
        CHECK(error->code=="source_binding_ambiguous");
        CHECK(state->constructions==0);
        CHECK(state->prepares==0);
    }
}

namespace {
// GRAPH-04 shared-upstream probe: a named passthrough that counts every prepared block so the
// render path can prove a fan-out node executes once per block, not once per out-edge.
class CountingPassthroughProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit CountingPassthroughProcessor(std::shared_ptr<NamedPortCounters> state)
        : state_(std::move(state)) { ++state_->constructions; }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"named_required", "Use named borrowed ports"};
    }
    bool supports_named_block_processing() const noexcept override { return true; }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override {
        state_->prepares += 1;
        return std::nullopt;
    }
    audioalgo::AudioBlockOutcome process_named_block(const audioalgo::NamedAudioBlockView& block) override {
        const auto* input = find_named_port(block.inputs, "in");
        const auto* output = find_named_port(block.outputs, "out");
        if (!input || !output || input->channels.size() != output->channels.size())
            return audioalgo::Diagnostic{"named_port_shape", "Passthrough port mapping is wrong"};
        for (std::size_t c = 0; c < input->channels.size(); ++c)
            for (std::size_t f = 0; f < block.frame_count; ++f)
                output->channels[c][f] = input->channels[c][f];
        state_->blocks += 1;
        state_->frames += block.frame_count;
        return audioalgo::AudioBlockResult{block.frame_count};
    }
private:
    std::shared_ptr<NamedPortCounters> state_;
};
} // namespace

// GRAPH-04 production contract: the builtin mixer is a real weighted dual-input node (no test
// bias), the shared upstream of a fan-out executes exactly once per block, chained mixer
// instances keep independent weights, and the descriptor stays visible through the default
// catalog used by the GUI.
TEST_CASE("GRAPH-04 production mixer renders weighted dual sources with shared upstream once", "[G2][GRAPH-04]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::current_path()/".audioalgo-test-tmp"/("graph04-production-mixer-"+juce::Uuid().toString().toStdString());
    std::filesystem::create_directories(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
    audioalgo::AudioBuffer a(48000,{"left","right"},513),b(48000,{"left","right"},513);
    for (std::size_t c=0;c<2;++c)
        for (std::size_t f=0;f<513;++f) {
            a.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
            b.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((11*f+5*c)%31)-15)/128.0f;
        }
    const double primary_weight=std::pow(10.0,-6.0/20.0),secondary_weight=std::pow(10.0,6.0/20.0);

    SECTION("descriptor and parameters are visible through the default catalog") {
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        const auto* descriptor=nodes.find("builtin.mixer");
        REQUIRE(descriptor!=nullptr);
        REQUIRE(descriptor->ports.size()==3);
        CHECK(descriptor->ports[0].id=="primary");
        CHECK(descriptor->ports[0].direction==audioalgo::PortDirection::input);
        CHECK(descriptor->ports[0].required);
        CHECK(descriptor->ports[1].id=="secondary");
        CHECK(descriptor->ports[1].direction==audioalgo::PortDirection::input);
        CHECK(descriptor->ports[1].required);
        CHECK(descriptor->ports[2].id=="sum");
        CHECK(descriptor->ports[2].direction==audioalgo::PortDirection::output);
        REQUIRE(descriptor->parameters.size()==2);
        CHECK(descriptor->parameters[0].id=="primary_gain_db");
        CHECK(descriptor->parameters[0].default_value==audioalgo::ParameterValue{0.0});
        CHECK(descriptor->parameters[0].minimum.has_value());
        CHECK(*descriptor->parameters[0].minimum==-60.0);
        CHECK(descriptor->parameters[0].maximum.has_value());
        CHECK(*descriptor->parameters[0].maximum==12.0);
        CHECK(descriptor->parameters[0].unit=="dB");
        CHECK(descriptor->parameters[1].id=="secondary_gain_db");
        CHECK(descriptor->parameters[1].default_value==audioalgo::ParameterValue{0.0});
        CHECK(descriptor->capabilities.supports_named_audio_ports);
    }

    SECTION("weighted sum keeps named roles across host block sizes and wiring") {
        for (const int host:{1,127,256})
            for (const bool swapped:{false,true}) {
                CAPTURE(host,swapped);
                for (const auto* stale:{"a.wav","b.wav"}) {std::error_code ignored;std::filesystem::remove(root/stale,ignored);}
                REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"a.wav",a,audioalgo::WavEncoding::float32)));
                REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"b.wav",b,audioalgo::WavEncoding::float32)));
                audioalgo::ProjectModel project;
                project.project_id="graph04-production-mixer";project.selected_output_node_id="output";
                project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},
                               {"b","builtin.file_source",{{"path",std::string("b.wav")}}},
                               {"mix","builtin.mixer",{{"primary_gain_db",-6.0},{"secondary_gain_db",6.0}}},
                               {"output","builtin.output",{}}};
                project.connections=swapped
                    ?std::vector<audioalgo::Connection>{{"b","audio","mix","primary"},{"a","audio","mix","secondary"},{"mix","sum","output","audio"}}
                    :std::vector<audioalgo::Connection>{{"a","audio","mix","primary"},{"b","audio","mix","secondary"},{"mix","sum","output","audio"}};
                const auto project_file=root/"project.aaproj";
                REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
                audioalgo::NodeRegistry nodes;
                REQUIRE(audioalgo::register_builtin_nodes(nodes));
                audioalgo::RunService service(nodes);
                const auto run=service.run({project,project_file,root/"run",host});
                const auto* error=std::get_if<audioalgo::Diagnostic>(&run);
                INFO("run error: "<<(error!=nullptr?error->code+": "+error->message:std::string("none")));
                REQUIRE(error==nullptr);
                const auto& success=std::get<audioalgo::RunSuccess>(run);
                REQUIRE(success.frames==513);
                const auto output=audioalgo::read_wav_file(success.output_file);
                REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(output));
                const auto& rendered=std::get<audioalgo::AudioBuffer>(output);
                const auto& primary=swapped?b:a;
                const auto& secondary=swapped?a:b;
                for (std::size_t c=0;c<2;++c)
                    for (std::size_t f=0;f<513;++f)
                        CHECK(rendered.channel(c)[f]==Catch::Approx(primary_weight*primary.channel(c)[f]+secondary_weight*secondary.channel(c)[f]).margin(1.0e-6));
                std::error_code remove_error;
                std::filesystem::remove_all(root/"run",remove_error);
            }
    }

    SECTION("fan-out upstream executes once per block, not once per out-edge") {
        for (const int host:{1,127,256}) {
            CAPTURE(host);
            std::error_code stale_error;std::filesystem::remove(root/"a.wav",stale_error);
            REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"a.wav",a,audioalgo::WavEncoding::float32)));
            audioalgo::ProjectModel project;
            project.project_id="graph04-shared-upstream";project.selected_output_node_id="output";
            project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},
                           {"probe","test.counting_passthrough",{}},
                           {"mix","builtin.mixer",{{"primary_gain_db",-6.0},{"secondary_gain_db",6.0}}},
                           {"output","builtin.output",{}}};
            project.connections={{"a","audio","probe","in"},
                                 {"probe","out","mix","primary"},
                                 {"probe","out","mix","secondary"},
                                 {"mix","sum","output","audio"}};
            const auto project_file=root/"project.aaproj";
            REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
            audioalgo::NodeRegistry nodes;
            REQUIRE(audioalgo::register_builtin_nodes(nodes));
            auto state=std::make_shared<NamedPortCounters>();
            audioalgo::NodeDescriptor probe_descriptor{"test.counting_passthrough","1.0.0","CountingPassthrough",
                {{"in","audio.pcm@1",audioalgo::PortDirection::input,true},
                 {"out","audio.pcm@1",audioalgo::PortDirection::output,false}},{},audioalgo::NodeExecutionKind::block};
            probe_descriptor.capabilities.supports_named_audio_ports=true;
            REQUIRE(nodes.register_node(probe_descriptor,[=]{return std::make_unique<CountingPassthroughProcessor>(state);}).registered);
            audioalgo::RunService service(nodes);
            const auto run=service.run({project,project_file,root/"run",host});
            const auto* error=std::get_if<audioalgo::Diagnostic>(&run);
            INFO("run error: "<<(error!=nullptr?error->code+": "+error->message:std::string("none")));
            REQUIRE(error==nullptr);
            const auto& success=std::get<audioalgo::RunSuccess>(run);
            CHECK(state->constructions==1);
            CHECK(state->prepares==1);
            CHECK(state->blocks==(513+host-1)/host);
            CHECK(state->frames==513);
            const auto output=audioalgo::read_wav_file(success.output_file);
            REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(output));
            const auto& rendered=std::get<audioalgo::AudioBuffer>(output);
            for (std::size_t c=0;c<2;++c)
                for (std::size_t f=0;f<513;++f)
                    CHECK(rendered.channel(c)[f]==Catch::Approx((primary_weight+secondary_weight)*a.channel(c)[f]).margin(1.0e-6));
            std::error_code remove_error;
            std::filesystem::remove_all(root/"run",remove_error);
        }
    }

    SECTION("chained mixer instances keep independent weights") {
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"a.wav",a,audioalgo::WavEncoding::float32)));
        REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"b.wav",b,audioalgo::WavEncoding::float32)));
        audioalgo::ProjectModel project;
        project.project_id="graph04-instance-isolation";project.selected_output_node_id="output";
        project.nodes={{"a","builtin.file_source",{{"path",std::string("a.wav")}}},
                       {"b","builtin.file_source",{{"path",std::string("b.wav")}}},
                       {"mix1","builtin.mixer",{{"primary_gain_db",-6.0},{"secondary_gain_db",0.0}}},
                       {"mix2","builtin.mixer",{{"primary_gain_db",0.0},{"secondary_gain_db",6.0}}},
                       {"output","builtin.output",{}}};
        project.connections={{"a","audio","mix1","primary"},
                             {"b","audio","mix1","secondary"},
                             {"mix1","sum","mix2","primary"},
                             {"b","audio","mix2","secondary"},
                             {"mix2","sum","output","audio"}};
        const auto project_file=root/"project.aaproj";
        REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,project)));
        audioalgo::NodeRegistry nodes;
        REQUIRE(audioalgo::register_builtin_nodes(nodes));
        audioalgo::RunService service(nodes);
        const auto run=service.run({project,project_file,root/"run",127});
        const auto* error=std::get_if<audioalgo::Diagnostic>(&run);
        INFO("run error: "<<(error!=nullptr?error->code+": "+error->message:std::string("none")));
        REQUIRE(error==nullptr);
        const auto output=audioalgo::read_wav_file(std::get<audioalgo::RunSuccess>(run).output_file);
        REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(output));
        const auto& rendered=std::get<audioalgo::AudioBuffer>(output);
        // mix1 = pw*a + 1.0*b; mix2 = 1.0*mix1 + sw*b = pw*a + (1+sw)*b. Both instances keep
        // their own weights, so mix1's secondary and mix2's primary stay neutral at 0 dB.
        for (std::size_t c=0;c<2;++c)
            for (std::size_t f=0;f<513;++f)
                CHECK(rendered.channel(c)[f]==Catch::Approx(primary_weight*a.channel(c)[f]+(1.0+secondary_weight)*b.channel(c)[f]).margin(1.0e-6));
    }
}

// AR-14.c: any output port can be the selected render terminal; branches outside the terminal's
// reverse closure create no processors, execute nothing and leave the selected signal untouched.
TEST_CASE("AR-14.c selected output port renders any port and prunes unselected branches",
          "[R5][AR-14.c][native_integration]") {
    juce::ScopedJuceInitialiser_GUI messages;
    for (const int host:{1,127,256})
        for (const bool side:{false,true}) {
            CAPTURE(host,side);
            audioalgo::NodeRegistry nodes;
            REQUIRE(audioalgo::register_builtin_nodes(nodes));
            auto split_state=std::make_shared<NamedPortCounters>();
            auto probe_state=std::make_shared<NamedPortCounters>();
            REQUIRE(nodes.register_node(named_split_descriptor(),
                [=]{return std::make_unique<NamedSplitProcessor>(split_state);}).registered);
            audioalgo::NodeDescriptor probe_descriptor{"test.counting_passthrough","1.0.0","CountingPassthrough",
                {{"in","audio.pcm@1",audioalgo::PortDirection::input,true},
                 {"out","audio.pcm@1",audioalgo::PortDirection::output,false}},{},audioalgo::NodeExecutionKind::block};
            probe_descriptor.capabilities.supports_named_audio_ports=true;
            REQUIRE(nodes.register_node(probe_descriptor,
                [=]{return std::make_unique<CountingPassthroughProcessor>(probe_state);}).registered);

            audioalgo::ProjectModel project;
            project.project_id="ar14c-selected-port";
            project.nodes={{"source","builtin.file_source",{{"path",std::string("input.wav")}}},
                           {"split","test.named_split",{}},
                           {"probe","test.counting_passthrough",{}},
                           {"output","builtin.output",{}}};
            project.connections={{"source","audio","split","input"},
                                 {"split","main","output","audio"},
                                 {"source","audio","probe","in"}};
            project.selected_output_node_id="split";
            project.selected_output_port_id=side?"side":"main";

            audioalgo::AudioBuffer input(48000,{"left","right"},513);
            for (std::size_t c=0;c<2;++c)
                for (std::size_t f=0;f<513;++f)
                    input.mutable_channel(c)[f]=static_cast<float>(static_cast<int>((3*f+7*c)%29)-14)/64.0f;
            const auto result=audioalgo::render_audio_graph(project,nodes,input,host);
            INFO((std::holds_alternative<audioalgo::Diagnostic>(result)
                ?std::get<audioalgo::Diagnostic>(result).message:std::string("success")));
            const auto* output=std::get_if<audioalgo::AudioBuffer>(&result);
            REQUIRE(output!=nullptr);
            CHECK(output->frame_count()==513);
            for (std::size_t c=0;c<2;++c)
                for (std::size_t f=0;f<513;++f)
                    CHECK(output->channel(c)[f]==Catch::Approx(input.channel(c)[f]*(side?-0.25f:2.0f)).margin(1.0e-6));
            // Selected branch executes once per block; the pruned branch never wakes up.
            CHECK(split_state->constructions==1);
            CHECK(split_state->prepares==1);
            CHECK(split_state->blocks==(513+host-1)/host);
            CHECK(split_state->frames==513);
            CHECK(probe_state->constructions==0);
            CHECK(probe_state->prepares==0);
            CHECK(probe_state->blocks==0);
            CHECK(probe_state->frames==0);
        }
}
