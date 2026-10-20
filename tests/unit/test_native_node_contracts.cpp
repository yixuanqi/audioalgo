#include "audio_juce_backend/JuceGraph.h"
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
