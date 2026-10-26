// Frozen NWS native_two_sources fixture: the node translation unit.
//
// It deliberately includes neither JUCE, the host, nor any AudioAlgo runtime
// header. W4 generates the thin C ABI adapter in its own staging directory from
// the released SDK and only compiles these declared sources.
#include "my_filter/filter.h"

namespace {

long long g_frame = 0;

}  // namespace

namespace audioalgo_example {

void node_reset() noexcept { g_frame = 0; }

void node_process(const float* primary, const float* reference, float* output,
                  int samples, float step) noexcept {
    mix(primary, reference, output, samples, step, g_frame);
    g_frame += samples;
}

}  // namespace audioalgo_example
