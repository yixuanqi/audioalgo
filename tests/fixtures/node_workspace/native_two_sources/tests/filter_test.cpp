// Author test for the frozen NWS native_two_sources fixture. It lives under
// tests/ and therefore never enters the runtime content manifest; W4 builds
// only the sources declared in node.json.
#include "my_filter/filter.h"

#include <cmath>
#include <cstdio>

int main() {
    const float primary[2] = {1.0f, -2.0f};
    const float reference[2] = {4.0f, 8.0f};
    float output[2] = {};
    audioalgo_example::mix(primary, reference, output, 2, 0.5f, 0);
    const float expected[2] = {-1.5f, 5.0f};
    for (int i = 0; i < 2; ++i)
        if (std::fabs(output[i] - expected[i]) > 1e-6f) return 1;
    return 0;
}
