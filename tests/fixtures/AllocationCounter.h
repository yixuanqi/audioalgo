#pragma once
#include <cstddef>

namespace audioalgo::test {
// Implemented beside the executable's controlled global allocation hooks.
void begin_allocation_count() noexcept;
std::size_t end_allocation_count() noexcept;
}
