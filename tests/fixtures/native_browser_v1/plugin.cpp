#include <audioalgo/node_abi_v1.h>
#include "manifest.h"

#include <cstring>
#include <new>
#include <string_view>

namespace {
struct Instance {};

uint32_t fail(aa_error* error, const char* code, const char* message) noexcept {
    if (error && error->struct_size == sizeof(aa_error)) {
        std::strncpy(error->code, code, sizeof(error->code) - 1);
        std::strncpy(error->message, message, sizeof(error->message) - 1);
        error->code[sizeof(error->code) - 1] = '\0';
        error->message[sizeof(error->message) - 1] = '\0';
    }
    return 1;
}

uint32_t AA_CALL create(aa_string type, aa_instance* instance, aa_error* error) noexcept {
    if (!instance || !type.data ||
        std::string_view(type.data, type.size) != "fixture.native_browser")
        return fail(error, "type_invalid", "Unexpected node type");
    *instance = new (std::nothrow) Instance;
    return *instance ? 0 : fail(error, "allocation_failed", "Allocation failed");
}

uint32_t AA_CALL prepare(aa_instance instance, const aa_prepare_v1* context, aa_error* error) noexcept {
    if (!instance || !context || context->struct_size != sizeof(*context) ||
        context->sample_rate_hz != 48000 || context->channels != 2)
        return fail(error, "prepare_invalid", "Expected stereo 48 kHz audio");
    return 0;
}

uint32_t AA_CALL process(aa_instance instance, const aa_block_v1* block,
                         aa_result_v1* result, aa_error* error) noexcept {
    if (!instance || !block || block->struct_size != sizeof(*block) ||
        !result || result->struct_size != sizeof(*result) ||
        block->input_count != 1 || block->output_count != 1)
        return fail(error, "block_invalid", "Expected one input and output");
    const auto& input = block->inputs[0];
    const auto& output = block->outputs[0];
    if (input.channels != output.channels || input.channels != 2)
        return fail(error, "channels_invalid", "Input/output channels must match");
    for (uint32_t channel = 0; channel < input.channels; ++channel)
        std::memcpy(output.data[channel], input.data[channel], sizeof(float) * block->frames);
    result->consumed_frames = block->frames;
    result->produced_frames = block->frames;
    return 0;
}

void AA_CALL reset(aa_instance) noexcept {}
void AA_CALL destroy(aa_instance instance) noexcept { delete static_cast<Instance*>(instance); }
} // namespace

extern "C" AA_EXPORT uint32_t AA_CALL audioalgo_node_api_v1(uint32_t abi, uint32_t size,
    aa_node_api_v1* api, aa_error* error) noexcept {
    if (abi != AA_NODE_ABI_V1 || size != sizeof(*api) || !api)
        return fail(error, "abi_invalid", "Unsupported ABI or table size");
    *api = {sizeof(*api), AA_NODE_ABI_V1,
        {audioalgo::test_fixtures::native_browser_v1::manifest,
         static_cast<uint32_t>(sizeof(audioalgo::test_fixtures::native_browser_v1::manifest) - 1)},
        create, prepare, process, reset, destroy};
    return 0;
}
