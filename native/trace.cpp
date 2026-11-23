#include <cstdio>
#include "desktop.hpp"
#ifdef KYUJO2_SHARED_MENU
#include "shared_menu.hpp"
#include "windows_cheats.h"
#include <atomic>
#endif
#include "region.hpp"
#include "crash_log.hpp"
#ifdef KYUJO2_WINDOWS_UI
#include <SDL.h>
#include "windows_cheats.h"
#include <atomic>
#include <windows.h>
#include <shellapi.h>
#endif
#include <cstdlib>
#include <filesystem>
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "recomp_overlays.inl"
#ifdef __SWITCH__
#include <switch.h>
#endif

using namespace ultramodern::renderer;
RspExitReason kyujo2_us_audio(uint8_t*, uint32_t);
RspExitReason kyujo2_us_jpeg(uint8_t*, uint32_t);
#ifndef KYUJO2_JP
RspExitReason kyujo2_us_gbcore(uint8_t*, uint32_t);
RspExitReason kyujo2_us_gbcore_a(uint8_t*, uint32_t);
RspExitReason kyujo2_us_gbcore_b(uint8_t*, uint32_t);
extern "C" void stadium2_gb_rsp_finished(void);
// Each program's final SP_STATUS write sets signal 3, which the Game Boy core polls.
template <RspExitReason (*program)(uint8_t*, uint32_t)>
static RspExitReason kyujo2_gbcore_task(uint8_t* rdram, uint32_t ucode) {
    RspExitReason reason = program(rdram, ucode);
    stadium2_gb_rsp_finished();
    static std::atomic<unsigned> runs{};
    if (std::getenv("KYUJO2_TRACE_GB") && ++runs % 60 == 1)
        std::fprintf(stderr, "GB core RSP program %p: %u tasks\n", reinterpret_cast<void*>(program), runs.load());
    return reason;
}
#endif

#ifdef __ANDROID__
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
#elif defined(KYUJO2_WINDOWS_UI)
static int run_windows_game(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc > 3) {
        std::fprintf(stderr, "Usage: %s [ROM [data-directory]]\n", argv[0]);
        return 2;
    }
#ifdef __SWITCH__
    auto data = std::filesystem::path("sdmc:/switch/Kyujo2");
#else
    auto data = argc == 3 ? std::filesystem::u8path(argv[2]) : default_data_directory();
#endif
    std::filesystem::create_directories(data);
    start_log_file(data);
#ifdef __SWITCH__
    // The renderer draws on the CPU. The system's boost mode (the one games use on loading screens) runs the CPU at
    // 1785 MHz instead of 1020; it is available to programs started in application mode.
    if (R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad))) std::fprintf(stderr, "Switch: CPU boost mode on\n");
    else std::fprintf(stderr, "Switch: CPU boost mode not available (applet mode?)\n");
#endif
    recomp::register_config_path(data);
    auto rom = argc >= 2 ? std::filesystem::u8path(argv[1]) : data / cartridge_file;
#ifndef KYUJO2_SHARED_MENU
#ifdef __SWITCH__
    if (!std::filesystem::exists(rom)) {
        show_missing_switch_rom();
        return 2;
    }
#else
#ifndef KYUJO2_WINDOWS_UI
    if (argc == 1 && !std::filesystem::exists(rom)) rom = choose_rom();
#endif
#endif
#endif
    if (rom.empty()) return 0;
    recomp::GameEntry game{
        .rom_hash = cartridge_hash,
        .internal_name = cartridge_name,
        .display_name = cartridge_title,
        .game_id = cartridge_id,
        .mod_game_id = "",
        .save_type = recomp::SaveType::Flashram,
        .is_enabled = true,
        .entrypoint_address = (gpr)(int32_t)0x80000400,
        .entrypoint = recomp_entrypoint,
        .on_init_callback = [](uint8_t* ram, recomp_context*) {
#ifdef KYUJO2_SHARED_MENU
            shared_menu::start_guest(ram);
#endif
            std::fprintf(stderr, "Starting %s entry 80000400\n", cartridge_name);
        },
    };
    recomp::register_game(game);
#ifdef KYUJO2_SHARED_MENU
    if (!shared_menu::setup(desktop_window(), data, rom, argc, argv)) return 0;
#else
    if (recomp::select_rom(rom, game.game_id) != recomp::RomValidationError::Good) {
        std::fprintf(stderr, "ROM validation failed for %s\n", cartridge_name);
        return 2;
    }
#endif
    recomp::overlays::register_overlays({section_table, ARRLEN(section_table), num_sections},
                                       {overlay_sections_by_index, ARRLEN(overlay_sections_by_index)});
    recomp::Configuration cfg{};
    cfg.project_version = {0, 1, 0, ""};
    cfg.rsp_callbacks.get_rsp_microcode = [](const OSTask* task) -> RspUcodeFunc* {
        if (task->t.type == 2 && uint32_t(task->t.ucode) == 0x80000460u && task->t.ucode_size == 0x1000) return kyujo2_us_audio;
        if (task->t.type == 4 && uint32_t(task->t.ucode) == jpeg_ucode_address && task->t.ucode_size == 0x1000) return kyujo2_us_jpeg;
#ifndef KYUJO2_JP
        // GB Tower's Game Boy core (fragment 89) runs one of its own three boot microcodes: ucode and ucode_size
        // are zero and the boot size identifies the program.
        if (task->t.type == 2 && task->t.ucode == 0 && task->t.ucode_size == 0) {
            switch (task->t.ucode_boot_size) {
            case 0xa04: case 0xa08: case 0xa10: return kyujo2_gbcore_task<kyujo2_us_gbcore_a>;
            case 0xb2c: case 0xb30: return kyujo2_gbcore_task<kyujo2_us_gbcore_b>;
            case 0xa60: return kyujo2_gbcore_task<kyujo2_us_gbcore>;
            }
        }
#endif
        std::fprintf(stderr, "RSP type %u code %08x length %x data %08x length %x\n", task->t.type, task->t.ucode, task->t.ucode_size, task->t.ucode_data, task->t.ucode_data_size);
        return nullptr;
    };
    configure_desktop(cfg);
    cfg.error_handling_callbacks.message_box = [](const char* msg) { std::fprintf(stderr, "%s\n", msg); };
#ifdef KYUJO2_WINDOWS_UI
    cfg.error_handling_callbacks.message_box = [](const char *msg) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Kyujo2", msg, desktop_window()); };
#endif
    recomp::start_game(game.game_id, "");
    recomp::start(cfg);
#ifdef KYUJO2_SHARED_MENU
    shared_menu::finish();
#endif
    return 0;
}

extern "C" void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t offset = static_cast<uint32_t>(ctx->r4) & 0x0FFFFFFF;
    const auto rom = recomp::get_rom();
    const uint32_t dest = static_cast<uint32_t>(ctx->r5) - 0x80000000u;
    if (rom.size() < 4 || offset > rom.size() - 4 || dest > 0x800000 - 4 || ((offset | dest) & 3)) {
        ctx->r2 = static_cast<gpr>(-1);
        return;
    }
    recomp::do_rom_pio(rdram, ctx->r5, 0x10000000 | offset);
    ctx->r2 = 0;
}

extern "C" void osPiGetStatus_recomp(uint8_t*, recomp_context*);
extern "C" gpr stadium2_pi_status(uint8_t* rdram, recomp_context* ctx) {
    const gpr saved = ctx->r2;
    osPiGetStatus_recomp(rdram, ctx);
    const gpr status = ctx->r2;
    ctx->r2 = saved;
    return status;
}
extern "C" uint32_t stadium2_read_cart_word(uint32_t offset) {
    const auto rom = recomp::get_rom();
    if ((offset & 3) || rom.size() < 4 || offset > rom.size() - 4) {
        std::fprintf(stderr, "Invalid cartridge read at %08x\n", offset);
        std::abort();
    }
    return uint32_t(rom[offset]) << 24 | uint32_t(rom[offset+1]) << 16 |
           uint32_t(rom[offset+2]) << 8 | rom[offset+3];
}

#include <map>
#include <mutex>
#include <set>
static std::map<uint32_t, size_t> fragment_allocations;
static std::mutex unrecompiled_mutex;
static std::set<uint32_t> unrecompiled_fragments;

#ifdef KYUJO2_SHARED_MENU
static std::atomic<unsigned> active_battles{};
static constexpr uint32_t battle_fragment_rom = 0x0036F890;
static constexpr uint32_t gb_core_fragment_rom = 0x015877A0;
extern "C" void __osHostGbPakRawRam(int channel, int on);
bool stadium2_battle_loaded() { return active_battles.load() != 0; }
#endif

static void release_fragment(size_t index, uint32_t base) {
#ifdef KYUJO2_SHARED_MENU
    if (section_table[index].rom_addr == battle_fragment_rom && --active_battles == 0) stadium2_end_battle();
    if (section_table[index].rom_addr == gb_core_fragment_rom) for (int port = 0; port < 4; ++port) __osHostGbPakRawRam(port, 0);
#endif
    unload_overlay_at(index, static_cast<int32_t>(base));
}

// Hook at the entry of the game's main pool allocator (func_80002740: a0 = size, a1 = 0 from the low end, 1 from the
// high end; the pool record is at 0x800CCDB0). The GPU renderer keeps frame buffers it drew on the GPU and copies them
// back into game memory only when the game reads them; a frame buffer whose memory the pool hands to something else
// (display lists, decompressed data) must be dropped first, or a late copy-back overwrites the new contents (seen: a
// battle's display list replaced by 0xDFDF pixels, then a crash). Kyujo does the same in its pool (port patch 0035).
extern "C" void rdp_memory_reused(uint32_t addr, uint32_t len);
extern "C" void stadium2_pool_alloc(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = ((static_cast<uint32_t>(ctx->r4) + 15u) & ~15u) + 16u;   // block header included
    const uint32_t side = static_cast<uint32_t>(ctx->r5);
    const gpr pool = static_cast<gpr>(static_cast<int32_t>(0x800CCDB0u));
    const uint32_t free_bytes = static_cast<uint32_t>(MEM_W(0x1C, pool));
    if (size == 0u || size > free_bytes || side > 1) return;                        // the allocation fails: nothing changes
    const uint32_t start = side == 0 ? static_cast<uint32_t>(MEM_W(0x28, pool))
                                     : static_cast<uint32_t>(MEM_W(0x2C, pool)) - size;
    rdp_memory_reused(start & 0x00FFFFFFu, size);
}

extern "C" void stadium2_fragment_load(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = static_cast<uint32_t>(ctx->r4);
    const uint32_t base = static_cast<uint32_t>(ctx->r5);
    if (id > 0xEF || base < 0x80000000 || base > 0x807FFFE0) return;
    const uint32_t linked = 0x81000000u + (id << 20);
    for (size_t index = 0; index < ARRLEN(section_table); ++index) {
        const auto& section = section_table[index];
        if (section.ram_addr != linked) continue;
        const auto rom = recomp::get_rom();
        if (section.rom_addr + 32 > rom.size()) break;
        for (unsigned i = 0; i < 32; ++i) {
            if (MEM_BU(i, ctx->r5) != rom[section.rom_addr + i]) return;
        }
        // The ROM section includes a relocation table that the guest frees after loading.
        // Dispatch follows the actual allocation and must preserve its adjacent fragments.
        auto previous = fragment_allocations.find(base);
        if (previous != fragment_allocations.end()) release_fragment(previous->second, base);
        load_overlay(index, static_cast<int32_t>(base));
        fragment_allocations[base] = index;
        { std::lock_guard lock(unrecompiled_mutex); unrecompiled_fragments.erase(base); }
#ifdef KYUJO2_SHARED_MENU
        if (section.rom_addr == battle_fragment_rom && ++active_battles == 1) stadium2_begin_battle();
        // GB Tower's Game Boy core runs the cartridge's own game: a Japanese Green must see its real save, not the
        // international-layout view that the Stadium's menus read (host_gbjp.c).
        if (section.rom_addr == gb_core_fragment_rom) for (int port = 0; port < 4; ++port) __osHostGbPakRawRam(port, 1);
#endif
        std::fprintf(stderr, "Fragment ROM %08x loaded at %08x\n", section.rom_addr, base);
        return;
    }
    std::fprintf(stderr, "Fragment id %02x loaded at %08x without a recompiled section (src %08x %08x)\n", id, base,
                 uint32_t(MEM_W(0, ctx->r5)), uint32_t(MEM_W(4, ctx->r5)));
    std::lock_guard lock(unrecompiled_mutex);
    unrecompiled_fragments.insert(base);
}
// Packed assets with code: asset_entry.c interprets their accessor when the game calls the fragment's base.
extern "C" int stadium2_unrecompiled_fragment(uint32_t base) {
    std::lock_guard lock(unrecompiled_mutex);
    return unrecompiled_fragments.count(base) != 0;
}
extern "C" void stadium2_fragment_free(uint8_t*, recomp_context* ctx) {
    const uint32_t base = static_cast<uint32_t>(ctx->r4);
    auto it = fragment_allocations.find(base);
    if (it != fragment_allocations.end()) {
        release_fragment(it->second, base);
        fragment_allocations.erase(it);
    }
}




#ifdef KYUJO2_WINDOWS_UI
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    try {
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 2;
        std::vector<std::string> values;
        for (int i = 0; i < count; ++i) {
            auto value = std::filesystem::path(args[i]).u8string();
            values.emplace_back(value.begin(), value.end());
        }
        LocalFree(args);
        std::vector<char*> pointers;
        for (auto &s : values) pointers.push_back(s.data());
        return run_windows_game(count, pointers.data());
    } catch (const std::exception &e) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Kyujo2", e.what(), nullptr);
        return 2;
    }
}
#endif
