#include <cstdio>
#include "android_touch.hpp"
#include <jni.h>
#include <atomic>

namespace {
std::atomic<uint64_t> state{};
std::atomic<bool> active{};
} // namespace

extern "C" JNIEXPORT void JNICALL Java_org_kyujo2_game_TouchOverlay_nativeTouch(JNIEnv *, jclass, jint mask,
                                                                                jint lx, jint ly) {
    state.store(uint16_t(mask) | (uint64_t(uint16_t(lx)) << 16) | (uint64_t(uint16_t(ly)) << 32));
}
extern "C" JNIEXPORT void JNICALL Java_org_kyujo2_game_TouchOverlay_nativeActive(JNIEnv *, jclass,
                                                                                 jboolean on) {
    state.store(0);
    active.store(on);
}
void touch::merge(uint16_t &buttons, float &x, float &y) {
    if (!active.load())
        return;
    const auto sample = state.load();
    buttons |= touch::n64_buttons(uint16_t(sample));
    if (!x)
        x = int16_t(sample >> 16) / 32767.f;
    if (!y)
        y = int16_t(sample >> 32) / 32767.f;
}


extern "C" {
#include "host_ui.h"
void kyujo2_touch_pad(HostRawPad *pad) {
    if (!active.load()) return;
    const auto sample = state.load();
    const uint16_t m = uint16_t(sample);
    pad->connected = 1;
    std::snprintf(pad->name, sizeof(pad->name), "Touch controls");
    const int inputs[] = {PB_A,PB_B,PB_START,PB_LB,PB_RB,-1,PB_BACK,-1,-1,-1,-1,PB_DUP,PB_DDOWN,PB_DLEFT,PB_DRIGHT};
    for(unsigned i=0;i<15;++i)
        if (inputs[i]>=0) pad->down[inputs[i]]=(m>>i)&1;
    pad->lt=(m & 32) ? 255 : 0;
    pad->lx=int16_t(sample>>16); pad->ly=int16_t(sample>>32);
    pad->rx=((m>>10&1)-(m>>9&1))*32767;
    pad->ry=((m>>7&1)-(m>>8&1))*32767;
}
}
