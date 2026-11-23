#include "android_touch.hpp"
#include <jni.h>
#include <cassert>
#include <cstdio>

extern "C" void Java_org_kyujo2_game_TouchOverlay_nativeTouch(JNIEnv *, jclass, jint, jint, jint);
extern "C" void Java_org_kyujo2_game_TouchOverlay_nativeActive(JNIEnv *, jclass, jboolean);
int main() {
    auto enabled = Java_org_kyujo2_game_TouchOverlay_nativeActive;
    auto input = Java_org_kyujo2_game_TouchOverlay_nativeTouch;
    enabled(nullptr, nullptr, JNI_TRUE);
    constexpr uint16_t expected[]{0x8000, 0x4000, 0x1000, 32,    16,    0x2000, 0,    8,
                                  4,      2,      1,      0x800, 0x400, 0x200,  0x100};
    for (int i = 0; i < 15; ++i) {
        input(nullptr, nullptr, 1 << i, 0, 0);
        uint16_t mask = 0;
        float x = 0, y = 0;
        touch::merge(mask, x, y);
        assert(mask == expected[i] && !x && !y);
    }
    input(nullptr, nullptr, 3, 32767, -32767);
    uint16_t mask = 0x10;
    float x = 0, y = 0;
    touch::merge(mask, x, y);
    assert(mask == 0xc010 && x == 1 && y == -1);
    enabled(nullptr, nullptr, JNI_FALSE);
    mask = 0;
    x = y = 0;
    touch::merge(mask, x, y);
    assert(!mask && !x && !y);
    enabled(nullptr, nullptr, JNI_TRUE);
    touch::merge(mask, x, y);
    assert(!mask && !x && !y);
    std::puts("PASS: all Kyujo touch bits, multitouch, analog axes, external controller and release");
}
