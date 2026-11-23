#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#include "region.hpp"
void show_missing_switch_rom() {
    consoleInit(nullptr);
    std::printf("Kyujo2\nCopy your ROM to sdmc:/switch/Kyujo2/%s\nPress + to exit.\n", cartridge_file);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}
#endif
