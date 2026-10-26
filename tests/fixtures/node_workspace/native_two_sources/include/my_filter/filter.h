#pragma once

// Frozen NWS native_two_sources fixture header. The generated staging project
// owns the C ABI adapter; this header only exposes the node's own DSP seam.
namespace audioalgo_example {

// y = -2 * primary + 0.125 * reference + step * n, where n is the running
// sample index owned by the node (stateful across blocks).
void mix(const float* primary, const float* reference, float* output,
         int samples, float step, long long first_frame) noexcept;

}  // namespace audioalgo_example
