#pragma once
#include <array>
#include <cstdint>

namespace touch {
// Kyujo TouchOverlay reports A, B, Start, L, R, Z, Menu, C and D-pad in this order.
constexpr uint16_t n64_buttons(uint32_t mask) {
    constexpr std::array<uint16_t, 15> bits{0x8000, 0x4000, 0x1000, 0x0020, 0x0010, 0x2000, 0,     0x0008,
                                            0x0004, 0x0002, 0x0001, 0x0800, 0x0400, 0x0200, 0x0100};
    uint16_t result = 0;
    for (unsigned i = 0; i < bits.size(); ++i)
        if (mask & (1u << i))
            result |= bits[i];
    return result;
}
void merge(uint16_t &buttons, float &x, float &y);
} // namespace touch
