/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_keys.c - the names of keyboard keys, gamepad buttons and N64 button actions (see host_ui.h). Used by the settings file and the menus.
 * HOST-side file (C library only).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"

static const struct { int key; const char *name; } kKeyNames[] = {
    {HK_BACKSPACE, "BACKSPACE"}, {HK_TAB, "TAB"}, {HK_ENTER, "ENTER"}, {HK_ESC, "ESC"}, {HK_SPACE, "SPACE"},
    {HK_UP, "UP"}, {HK_DOWN, "DOWN"}, {HK_LEFT, "LEFT"}, {HK_RIGHT, "RIGHT"},
    {HK_LSHIFT, "LSHIFT"}, {HK_RSHIFT, "RSHIFT"}, {HK_LCTRL, "LCTRL"}, {HK_RCTRL, "RCTRL"}, {HK_LALT, "LALT"}, {HK_RALT, "RALT"},
    {HK_INSERT, "INSERT"}, {HK_DELETE, "DELETE"}, {HK_HOME, "HOME"}, {HK_END, "END"}, {HK_PGUP, "PGUP"}, {HK_PGDN, "PGDN"},
    {HK_F1, "F1"}, {HK_F2, "F2"}, {HK_F3, "F3"}, {HK_F4, "F4"}, {HK_F5, "F5"}, {HK_F6, "F6"},
    {HK_F7, "F7"}, {HK_F8, "F8"}, {HK_F9, "F9"}, {HK_F10, "F10"}, {HK_F11, "F11"}, {HK_F12, "F12"},
    {HK_NUM0, "NUM0"}, {HK_NUM1, "NUM1"}, {HK_NUM2, "NUM2"}, {HK_NUM3, "NUM3"}, {HK_NUM4, "NUM4"},
    {HK_NUM5, "NUM5"}, {HK_NUM6, "NUM6"}, {HK_NUM7, "NUM7"}, {HK_NUM8, "NUM8"}, {HK_NUM9, "NUM9"},
    {HK_NUMADD, "NUMADD"}, {HK_NUMSUB, "NUMSUB"}, {HK_NUMMUL, "NUMMUL"}, {HK_NUMDIV, "NUMDIV"}, {HK_NUMDOT, "NUMDOT"}, {HK_NUMENTER, "NUMENTER"},
    {HK_SEMICOLON, "SEMICOLON"}, {HK_QUOTE, "QUOTE"}, {HK_COMMA, "COMMA"}, {HK_PERIOD, "PERIOD"}, {HK_SLASH, "SLASH"},
    {HK_LBRACKET, "LBRACKET"}, {HK_RBRACKET, "RBRACKET"}, {HK_BACKSLASH, "BACKSLASH"}, {HK_MINUS, "MINUS"}, {HK_EQUALS, "EQUALS"},
    {HK_BACKQUOTE, "BACKQUOTE"},
};

const char *host_key_name(int key)
{
    static char one[4][2];                             /* letters and digits: a one-character name (four buffers so a caller may hold up to four names at once) */
    static int flip;
    unsigned i;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        char *b = one[flip = (flip + 1) & 3];
        b[0] = (char)key;
        b[1] = 0;
        return b;
    }
    for (i = 0; i < sizeof kKeyNames / sizeof kKeyNames[0]; i++) if (kKeyNames[i].key == key) return kKeyNames[i].name;
    return NULL;
}

int host_key_from_name(const char *name)
{
    unsigned i;
    char up[32];
    size_t n = strlen(name), k;
    if (n == 0 || n >= sizeof up) return HK_NONE;
    for (k = 0; k < n; k++) up[k] = (char)toupper((unsigned char)name[k]);
    up[n] = 0;
    if (n == 1 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9'))) return up[0];
    for (i = 0; i < sizeof kKeyNames / sizeof kKeyNames[0]; i++) if (strcmp(kKeyNames[i].name, up) == 0) return kKeyNames[i].key;
    return HK_NONE;
}

/* The names of the gamepad inputs: kPadNames are the words of the settings file, kPadLabels what the menu shows. The generic inputs are built
 * by pad_name()/pad_label() below (button numbers and axes). */
static const char *const kPadNames[PB_G_BTN1] = {
    "A", "B", "X", "Y", "LB", "RB", "BACK", "START", "LSTICK", "RSTICK", "DUP", "DDOWN", "DLEFT", "DRIGHT", "LT", "RT",
    "LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT", "RS_UP", "RS_DOWN", "RS_LEFT", "RS_RIGHT",
};
static const char *const kPadLabels[PB_G_BTN1] = {
    "A", "B", "X", "Y", "Left bumper", "Right bumper", "Back", "Start", "Left stick click", "Right stick click",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left trigger", "Right trigger",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left", "Right stick right",
};
static const char kAxisLetter[6] = {'X', 'Y', 'Z', 'R', 'U', 'V'};
static const char *const kHatNames[4] = {"HAT_UP", "HAT_DOWN", "HAT_LEFT", "HAT_RIGHT"};
static const char *const kHatLabels[4] = {"Hat up", "Hat down", "Hat left", "Hat right"};

const char *host_pad_button_name(int button)
{
    static char buf[4][16];
    static int rot;
    char *b;
    if (button < 0 || button >= PB_COUNT) return NULL;
    if (button < PB_G_BTN1) return kPadNames[button];
    b = buf[rot++ & 3];
    if (button < PB_G_HAT_UP) snprintf(b, 16, "BTN%d", button - PB_G_BTN1 + 1);
    else if (button < PB_G_AX_X_NEG) return kHatNames[button - PB_G_HAT_UP];
    else snprintf(b, 16, "AX%c%c", kAxisLetter[(button - PB_G_AX_X_NEG) / 2], (button - PB_G_AX_X_NEG) % 2 ? '+' : '-');
    return b;
}

const char *host_pad_button_label(int button)
{
    static char buf[4][24];
    static int rot;
    char *b;
    if (button < 0 || button >= PB_COUNT) return NULL;
    if (button < PB_G_BTN1) return kPadLabels[button];
    b = buf[rot++ & 3];
    if (button < PB_G_HAT_UP) snprintf(b, 24, "Button %d", button - PB_G_BTN1 + 1);
    else if (button < PB_G_AX_X_NEG) return kHatLabels[button - PB_G_HAT_UP];
    else snprintf(b, 24, "Axis %c %c", kAxisLetter[(button - PB_G_AX_X_NEG) / 2], (button - PB_G_AX_X_NEG) % 2 ? '+' : '-');
    return b;
}

int host_pad_button_from_name(const char *name)
{
    int i;
    char up[32];
    size_t n = strlen(name), k;
    if (n == 0 || n >= sizeof up) return -1;
    for (k = 0; k < n; k++) up[k] = (char)toupper((unsigned char)name[k]);
    up[n] = 0;
    for (i = 0; i < PB_COUNT; i++) if (strcmp(host_pad_button_name(i), up) == 0) return i;      /* host_pad_button_name's buffer is rotated: compare at once */
    return -1;
}

/* N64 console bit layout: lib/ultralib/include/PR/os_cont.h (same values as platform.h's host_input_read comment). */
static const struct { const char *name, *label; unsigned short bit; } kActions[ACT_COUNT] = {
    {"a", "A", 0x8000}, {"b", "B", 0x4000}, {"z", "Z", 0x2000}, {"start", "Start", 0x1000},
    {"dup", "D-pad up", 0x0800}, {"ddown", "D-pad down", 0x0400}, {"dleft", "D-pad left", 0x0200}, {"dright", "D-pad right", 0x0100},
    {"l", "L", 0x0020}, {"r", "R", 0x0010},
    {"cup", "C-up", 0x0008}, {"cdown", "C-down", 0x0004}, {"cleft", "C-left", 0x0002}, {"cright", "C-right", 0x0001},
    {"stick_up", "Stick up", 0}, {"stick_down", "Stick down", 0}, {"stick_left", "Stick left", 0}, {"stick_right", "Stick right", 0},
    {"menu", "Open menu", 0},
};

const char *host_action_name(int a) { return a >= 0 && a < ACT_COUNT ? kActions[a].name : NULL; }
const char *host_action_label(int a) { return a >= 0 && a < ACT_COUNT ? kActions[a].label : NULL; }
unsigned short host_action_n64_bit(int a) { return a >= 0 && a < ACT_COUNT ? kActions[a].bit : 0; }
