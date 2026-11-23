/* SPDX-License-Identifier: GPL-3.0-only
 * SDL keyboard and mapped controller translation from Kyujo shell/sdl/main_sdl.c. */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "host_ui.h"
static SDL_Window *sWin;
#ifdef __ANDROID__
extern void kyujo2_touch_pad(HostRawPad *);
#endif
static SDL_GameController *sPad[4];
static SDL_JoystickID sPadId[4];
static SDL_Joystick *generic[4];

static int key_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (int)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (int)(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return HK_F1 + (int)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return HK_NUM1 + (int)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_KP_0: return HK_NUM0;
    case SDL_SCANCODE_RETURN: return HK_ENTER;
    case SDL_SCANCODE_ESCAPE: return HK_ESC;
    case SDL_SCANCODE_AC_BACK: return HK_ESC;                 /* Android's Back button opens the game's menu (SDL passes it on instead of closing the activity: see the hints in main) */
    case SDL_SCANCODE_BACKSPACE: return HK_BACKSPACE;
    case SDL_SCANCODE_TAB: return HK_TAB;
    case SDL_SCANCODE_SPACE: return HK_SPACE;
    case SDL_SCANCODE_UP: return HK_UP;
    case SDL_SCANCODE_DOWN: return HK_DOWN;
    case SDL_SCANCODE_LEFT: return HK_LEFT;
    case SDL_SCANCODE_RIGHT: return HK_RIGHT;
    case SDL_SCANCODE_LSHIFT: return HK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return HK_RSHIFT;
    case SDL_SCANCODE_LCTRL: return HK_LCTRL;
    case SDL_SCANCODE_RCTRL: return HK_RCTRL;
    case SDL_SCANCODE_LALT: return HK_LALT;
    case SDL_SCANCODE_RALT: return HK_RALT;
    case SDL_SCANCODE_INSERT: return HK_INSERT;
    case SDL_SCANCODE_DELETE: return HK_DELETE;
    case SDL_SCANCODE_HOME: return HK_HOME;
    case SDL_SCANCODE_END: return HK_END;
    case SDL_SCANCODE_PAGEUP: return HK_PGUP;
    case SDL_SCANCODE_PAGEDOWN: return HK_PGDN;
    case SDL_SCANCODE_KP_PLUS: return HK_NUMADD;
    case SDL_SCANCODE_KP_MINUS: return HK_NUMSUB;
    case SDL_SCANCODE_KP_MULTIPLY: return HK_NUMMUL;
    case SDL_SCANCODE_KP_DIVIDE: return HK_NUMDIV;
    case SDL_SCANCODE_KP_PERIOD: return HK_NUMDOT;
    case SDL_SCANCODE_KP_ENTER: return HK_NUMENTER;
    case SDL_SCANCODE_SEMICOLON: return HK_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return HK_QUOTE;
    case SDL_SCANCODE_COMMA: return HK_COMMA;
    case SDL_SCANCODE_PERIOD: return HK_PERIOD;
    case SDL_SCANCODE_SLASH: return HK_SLASH;
    case SDL_SCANCODE_LEFTBRACKET: return HK_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET: return HK_RBRACKET;
    case SDL_SCANCODE_BACKSLASH: return HK_BACKSLASH;
    case SDL_SCANCODE_MINUS: return HK_MINUS;
    case SDL_SCANCODE_EQUALS: return HK_EQUALS;
    case SDL_SCANCODE_GRAVE: return HK_BACKQUOTE;
    default: return HK_NONE;
    }
}

static void pad_open(int device_index)
{
    int slot;
    SDL_GameController *c;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    for (slot=0;slot<4;++slot) if ((sPad[slot] || generic[slot]) && sPadId[slot] == id) return;
    for (slot = 0; slot < 4 && (sPad[slot] || generic[slot]); slot++) {}
    if (slot == 4) return;
    if (!SDL_IsGameController(device_index)) {
        generic[slot] = SDL_JoystickOpen(device_index);
        if (generic[slot]) sPadId[slot] = id;
        return;
    }
    c = SDL_GameControllerOpen(device_index);
    if (!c) { fprintf(stderr, "sdl: cannot open controller %d: %s\n", device_index, SDL_GetError()); return; }
    sPad[slot] = c;
    sPadId[slot] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    fprintf(stderr, "sdl: controller %d: %s\n", slot, SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
}

static void pad_close(SDL_JoystickID id)
{
    int slot;
    for (slot = 0; slot < 4; slot++) {
        if (generic[slot] && sPadId[slot] == id) { SDL_JoystickClose(generic[slot]); generic[slot] = NULL; }
        if (sPad[slot] && sPadId[slot] == id) { SDL_GameControllerClose(sPad[slot]); sPad[slot] = NULL; }
    }
}

static short axis16(SDL_GameController *c, SDL_GameControllerAxis a) { return (short)SDL_GameControllerGetAxis(c, a); }

static void read_input(HostRawInput *raw)
{
    static const struct { SDL_GameControllerButton sdl; int pb; } map[] = {
#ifdef __SWITCH__
        /* SDL names the buttons by position (A = bottom), the Switch's own labels are the other way round: the right-hand button is "A". The game's
         * "A" (accept) must be the one labelled A on the controller, and B the one labelled B. */
        { SDL_CONTROLLER_BUTTON_B, PB_A }, { SDL_CONTROLLER_BUTTON_A, PB_B }, { SDL_CONTROLLER_BUTTON_Y, PB_X }, { SDL_CONTROLLER_BUTTON_X, PB_Y },
#else
        { SDL_CONTROLLER_BUTTON_A, PB_A }, { SDL_CONTROLLER_BUTTON_B, PB_B }, { SDL_CONTROLLER_BUTTON_X, PB_X }, { SDL_CONTROLLER_BUTTON_Y, PB_Y },
#endif
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PB_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PB_RB },
        { SDL_CONTROLLER_BUTTON_BACK, PB_BACK }, { SDL_CONTROLLER_BUTTON_START, PB_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, PB_LSTICK }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PB_RSTICK },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PB_DUP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PB_DDOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PB_DLEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PB_DRIGHT },
    };
    int i, k, nkeys = 0;
    const Uint8 *ks = SDL_GetKeyboardState(&nkeys);
    memset(raw, 0, sizeof *raw);
    {   /* the window's size, so that the game's menu can be drawn as wide as the window */
        int vw = 0, vh = 0;
        if (sWin) SDL_GetWindowSize(sWin, &vw, &vh);
        raw->view_w = (unsigned short)(vw > 0 && vw < 65536 ? vw : 0); raw->view_h = (unsigned short)(vh > 0 && vh < 65536 ? vh : 0);
    }
#ifdef __SWITCH__
    raw->console = 1;                                            /* the menu words buttons the console's way (L, R, ZL, Minus) */
#endif
#ifdef __ANDROID__
    raw->touch = 1;                                              /* the menu talks about the Menu button (or Back), not about keys */
#endif
    if (ks) {
        for (i = 0; i < nkeys && i < SDL_NUM_SCANCODES; i++) {
            if (ks[i]) { k = key_from_scancode((SDL_Scancode)i); if (k > 0 && k < HK_COUNT) raw->key[k] = 1; }
        }
        raw->keyboards = 1;
    }
    for (i = 0; i < 4; i++) {
        HostRawPad *p = &raw->pad[i];
        SDL_GameController *c = sPad[i];
        unsigned j;
        if (generic[i]) {
            SDL_Joystick *js = generic[i];
            p->connected = p->generic = 1;
            snprintf(p->name,sizeof p->name,"%.40s #%d", SDL_JoystickName(js) ? SDL_JoystickName(js) : "Joystick", i+1);
            p->buttons_total = SDL_min(32,SDL_JoystickNumButtons(js));
            for (int b=0;b<p->buttons_total;b++) p->down[PB_G_BTN1+b]=SDL_JoystickGetButton(js,b);
            int hat=SDL_JoystickNumHats(js) ? SDL_JoystickGetHat(js,0) : 0;
            const int bits[]={SDL_HAT_UP,SDL_HAT_DOWN,SDL_HAT_LEFT,SDL_HAT_RIGHT};
            for(int b=0;b<4;b++) p->down[PB_G_HAT_UP+b]=(hat & bits[b]) != 0;
            for(int b=0;b<SDL_min(6,SDL_JoystickNumAxes(js));b++) p->axis[b]=SDL_JoystickGetAxis(js,b);
            p->lx=p->axis[0]; p->ly=(short)SDL_clamp(-(int)p->axis[1],-32767,32767);
        }
        if (!c) continue;
        p->connected = 1;
        p->generic = 0;
        snprintf(p->name, sizeof p->name, "%.40s #%d", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "Controller", i + 1);
        for (j = 0; j < sizeof map / sizeof map[0]; j++) p->down[map[j].pb] = SDL_GameControllerGetButton(c, map[j].sdl) ? 1 : 0;
        p->lx = axis16(c, SDL_CONTROLLER_AXIS_LEFTX);
        p->ly = (short)(-(int)axis16(c, SDL_CONTROLLER_AXIS_LEFTY) - (axis16(c, SDL_CONTROLLER_AXIS_LEFTY) == -32768 ? 1 : 0));    /* up positive */
        p->rx = axis16(c, SDL_CONTROLLER_AXIS_RIGHTX);
        p->ry = (short)(-(int)axis16(c, SDL_CONTROLLER_AXIS_RIGHTY) - (axis16(c, SDL_CONTROLLER_AXIS_RIGHTY) == -32768 ? 1 : 0));
        k = axis16(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7; p->lt = (unsigned char)(k < 0 ? 0 : k > 255 ? 255 : k);
        k = axis16(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7; p->rt = (unsigned char)(k < 0 ? 0 : k > 255 ? 255 : k);
    }
#ifdef __ANDROID__
    kyujo2_touch_pad(&raw->pad[HOST_PADS-1]);      /* the on-screen controls are controller 1 while no gamepad is connected */
#endif
}



void menu_input_init(SDL_Window *window) {
    sWin=window;
    for(int i=0;i<SDL_NumJoysticks();++i) pad_open(i);
}
void menu_input_event(const SDL_Event *e) {
    if (e->type == SDL_JOYDEVICEADDED) pad_open(e->jdevice.which);
    if (e->type == SDL_JOYDEVICEREMOVED) pad_close(e->jdevice.which);
}
void menu_publish_input(void) {
    HostRawInput snapshot;
    read_input(&snapshot);
    host_raw_publish(&snapshot);
}
int menu_key_code(int scancode) { return key_from_scancode((SDL_Scancode)scancode); }
