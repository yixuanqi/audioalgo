// Frozen NWS native_two_sources fixture: the auxiliary translation unit.
#include "my_filter/filter.h"

namespace audioalgo_example {

void mix(const float* primary, const float* reference, float* output,
         int samples, float step, long long first_frame) noexcept {
    for (int i = 0; i < samples; ++i)
        output[i] = -2.0f * primary[i] + 0.125f * reference[i] +
                    step * static_cast<float>(first_frame + i);
}

}  // namespace audioalgo_example
