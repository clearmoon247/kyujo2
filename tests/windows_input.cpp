#define SDL_MAIN_HANDLED
#include "windows_input.hpp"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS))
        return 2;
    try {
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            std::printf("Physical SDL device: %s (mapped %d, VID %04x, PID %04x)\n",
                        SDL_JoystickNameForIndex(i), SDL_IsGameController(i), SDL_JoystickGetDeviceVendor(i),
                        SDL_JoystickGetDeviceProduct(i));
        if (argc == 3 && std::string(argv[2]) == "--listen") {
            pads::init(std::filesystem::u8path(argv[1]));
            const auto names = pads::names();
            for (int i = 0; i < 4; ++i)
                if (pads::assigned(i) >= 0)
                    std::printf("Listening to port %d, SDL device %d\n", i + 1, pads::assigned(i));
            std::fflush(stdout);
            auto start = SDL_GetTicks();
            bool cross = false, options = false;
            while (SDL_GetTicks() - start < 150000 && !(cross && options)) {
                SDL_PumpEvents();
                auto v = pads::sample(0);
                if (!cross && (v.buttons & 0x8000)) {
                    std::puts("Physical Cross -> N64 A");
                    cross = true;
                    std::fflush(stdout);
                }
                if (!options && (v.buttons & 0x1000)) {
                    std::puts("Physical Options -> N64 Start");
                    options = true;
                    std::fflush(stdout);
                }
                SDL_Delay(8);
            }
            return cross && options ? 0 : 3;
        }
        SDL_VirtualJoystickDesc virtualPad{};
        virtualPad.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        virtualPad.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        virtualPad.nbuttons = 12;
        virtualPad.naxes = 4;
        virtualPad.nhats = 1;
        virtualPad.name = "Kyujo2 Generic Test";
        int index = SDL_JoystickAttachVirtualEx(&virtualPad);
        require(index >= 0, "Could not attach test joystick");
        pads::init(std::filesystem::u8path(argv[1]));
        auto names = pads::names();
        int selected = -1;
        for (unsigned i = 0; i < names.size(); ++i)
            if (names[i].find(L"Kyujo2 Generic Test") != std::wstring::npos)
                selected = i;
        require(selected >= 0, "Already-connected generic joystick was missed");
        pads::assign(2, selected);
        auto *joy = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(index));
        SDL_JoystickSetVirtualButton(joy, 0, 1);
        SDL_JoystickSetVirtualAxis(joy, 0, 22000);
        SDL_JoystickUpdate();
        auto state = pads::sample(2);
        require((state.buttons & 0x8000) && state.x > 0.5f, "Guest A or analog stick was missing");
        pads::begin_bind(2, 13, false);
        SDL_Event ev{};
        ev.type = SDL_JOYBUTTONDOWN;
        ev.jbutton.which = SDL_JoystickInstanceID(joy);
        ev.jbutton.button = 9;
        require(pads::capture(ev), "Could not bind a generic joystick button");
        SDL_JoystickSetVirtualButton(joy, 0, 0);
        SDL_JoystickSetVirtualButton(joy, 9, 1);
        SDL_JoystickUpdate();
        require((pads::sample(2).buttons & 0x8000) != 0, "New A binding was ignored");
        SDL_JoystickDetachVirtual(index);
        SDL_PumpEvents();
        pads::refresh();
        require(!pads::connected(2), "Disconnected pad remained active");
        index = SDL_JoystickAttachVirtualEx(&virtualPad);
        SDL_PumpEvents();
        pads::refresh();
        require(pads::connected(2) && pads::label(2, 13, false) == L"Button 10",
                "Reconnection lost port or binding");
        SDL_JoystickDetachVirtual(index);
        SDL_PumpEvents();
        pads::refresh();
        std::puts("PASS: pre-connected raw joystick, port assignment, guest buttons and stick, remapping, "
                  "removal, reconnection and persistence");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    SDL_Quit();
    return 0;
}
