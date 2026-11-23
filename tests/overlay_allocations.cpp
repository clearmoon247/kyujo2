#include "librecomp/overlays.hpp"
#include <cstdio>

static void first(uint8_t*, recomp_context*) {}
static void second(uint8_t*, recomp_context*) {}

int main() {
    // Sizes on cartridge include discarded relocation data. These live allocations
    // come from the r6 battle unload and are adjacent in the guest pool.
    FuncEntry functions[] = {{first, 0, 8}, {second, 0, 8}};
    SectionTableEntry sections[] = {
        {.rom_addr=0x36f890, .ram_addr=0x84100000, .size=0xa9bf0, .funcs=&functions[1], .num_funcs=1, .index=0},
        {.rom_addr=0x434720, .ram_addr=0x84400000, .size=0x1350, .funcs=&functions[0], .num_funcs=1, .index=1}
    };
    recomp::overlays::register_overlays({sections, 2, 2}, {nullptr, 0});
    recomp::overlays::init_overlays();
    const int32_t first_base = static_cast<int32_t>(0x801462c0);
    const int32_t second_base = static_cast<int32_t>(0x80147430);
    const int32_t extra_copy = static_cast<int32_t>(0x80300000);
    load_overlay(1, first_base);
    load_overlay(0, second_base);
    load_overlay(1, extra_copy);
    unload_overlay_at(1, extra_copy);
    unload_overlay_at(0, second_base);
    if (get_function(first_base) != first || section_addresses[1] != first_base ||
        static_cast<uint32_t>(section_addresses[0]) != sections[0].ram_addr) return 1;
    unload_overlay_at(1, first_base);
    if (static_cast<uint32_t>(section_addresses[1]) != sections[1].ram_addr) return 1;
    std::puts("PASS: adjacent live allocations and multiple loaded copies are freed independently");
}
