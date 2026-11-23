/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_input.c - the four N64 controller ports for the emulated PIF (see platform.h, host_input_read).
 *
 * Where the buttons come from, in the order they are looked at:
 *   1. TEST SCRIPT KYUJO_INPUT, for testing without a window: a comma-separated list of
 *          <when>:<buttons>[:<stick_x>:<stick_y>]
 *      where <when> is seconds after program start, or a frame number followed by 'f' (e.g. 1200f: the 1200th distinct video frame,
 *      the numbering of the frame dump; steadier than seconds because the game does not run in real time yet).
 *      Meaning "from <when> on, hold <buttons> (joined with '+', empty = none) and the stick at x,y (-128..127)".
 *      Example: KYUJO_INPUT="20:START,20.2:,25:A,25.2:,30:DOWN:0:-100"   (entries must be in time order)
 *      Button names: A B Z START UP DOWN LEFT RIGHT L R CUP CDOWN CLEFT CRIGHT. Only controller port 1 exists while it is set.
 *   2. The raw devices (keyboard, gamepads) of the window layer, turned into N64 buttons with the bindings of the settings (host_input_map).
 *      For tests the raw devices can come from a script too, KYUJO_RAW: <when>p:<token>+<token>...,  where <when>p is the number of the
 *      menu poll (host_menu_poll runs once per video field and 60 times a second while the menu is open) and a token is a key name (F1, ESC,
 *      ENTER, UP, X ...), padN.BUTTON (pad1.A, pad2.DUP, pad3.BTN4, pad3.HAT_UP ...; N = 1..8), padN.LX=<-32768..32767> (also LY RX RY, LT RT 0..255), padN.AX0=<value>
 *      (generic axes 0..5 = X Y Z R U V), padN.generic (a generic device instead of an Xbox-layout one) or padN.name=Some_Name (underscores become spaces). Example:
 *      KYUJO_RAW="100p:F1,110p:,115p:DOWN,120p:" opens the menu at poll 100 and moves the cursor down.
 * A port is "connected" when the keyboard or a connected gamepad drives it (settings), or a Game Boy cartridge is configured for it.
 * HOST-side file: may use the C library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"

/* Bit layout of the console (lib/ultralib/include/PR/os_cont.h), same as in platform.h. */
static const struct { const char *name; unsigned short bit; } kButtons[] = {
    {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0200},
    {"RIGHT", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CUP", 0x0008}, {"CDOWN", 0x0004}, {"CLEFT", 0x0002}, {"CRIGHT", 0x0001},
};

typedef struct ScriptStep {
    double at;                    /* seconds, or frame number when is_frame */
    int is_frame;
    unsigned short buttons;
    signed char x, y;
} ScriptStep;

#define MAX_STEPS 4096
static ScriptStep *sSteps;      /* on the heap, only when a test script is given: 64 KiB of static data would use up room below 0x01000000 (tools/native/imagelimit.py) */
static int sStepCount, sScriptLoaded;
static unsigned long long sBaseNs;        /* host_wall_ns() (the clock that goes on while the menu pauses the game) when the script was loaded = 'program start' for the script's times */

/* Parses a '+'-joined list of button names; unknown names stop the program (a typo in a test script must not be silently ignored). */
static unsigned short parse_buttons(const char *s, size_t len)
{
    unsigned short v = 0;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        unsigned k;
        int found = 0;
        while (j < len && s[j] != '+') j++;
        for (k = 0; k < sizeof kButtons / sizeof kButtons[0]; k++) {
            if (strlen(kButtons[k].name) == j - i && strncmp(kButtons[k].name, s + i, j - i) == 0) {
                v |= kButtons[k].bit;
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "kyujo: KYUJO_INPUT: unknown button name '%.*s'\n", (int)(j - i), s + i);
            exit(2);
        }
        i = j + 1;
    }
    return v;
}

static void load_script(void)
{
    const char *env = getenv("KYUJO_INPUT");
    sScriptLoaded = 1;
    sBaseNs = host_wall_ns();
    if (!env || !*env) return;
    sSteps = (ScriptStep *)calloc(MAX_STEPS, sizeof *sSteps);
    if (!sSteps) { fprintf(stderr, "kyujo: KYUJO_INPUT: out of memory\n"); exit(2); }
    while (*env) {
        char *end;
        double t = strtod(env, &end);
        const char *b;
        size_t blen;
        long x = 0, y = 0;
        int is_frame = 0;
        if (end != env && *end == 'f') { is_frame = 1; end++; }
        if (sStepCount >= MAX_STEPS) {
            fprintf(stderr, "kyujo: KYUJO_INPUT: more than %d steps\n", MAX_STEPS);
            exit(2);
        }
        if (end == env || *end != ':') {
            fprintf(stderr, "kyujo: KYUJO_INPUT: cannot parse near '%s' (format <seconds>[f]:<buttons>[:x:y],...)\n", env);
            exit(2);
        }
        b = end + 1;
        blen = strcspn(b, ":,");
        env = b + blen;
        if (*env == ':') {
            x = strtol(env + 1, &end, 10);
            if (*end != ':') { fprintf(stderr, "kyujo: KYUJO_INPUT: stick needs both x and y\n"); exit(2); }
            y = strtol(end + 1, &end, 10);
            env = end;
        }
        if (x < -128 || x > 127 || y < -128 || y > 127) { fprintf(stderr, "kyujo: KYUJO_INPUT: stick out of range\n"); exit(2); }
        sSteps[sStepCount].at = t;
        sSteps[sStepCount].is_frame = is_frame;
        sSteps[sStepCount].buttons = parse_buttons(b, blen);
        sSteps[sStepCount].x = (signed char)x;
        sSteps[sStepCount].y = (signed char)y;
        sStepCount++;
        if (*env == ',') env++;
        else if (*env) { fprintf(stderr, "kyujo: KYUJO_INPUT: unexpected '%s'\n", env); exit(2); }
    }
}

/* ---- raw devices: the window layer's latest snapshot, or the test script KYUJO_RAW ---- */

static HostRawInput sPublished;
static volatile unsigned sSeq;                    /* seqlock: odd while host_raw_publish writes */

void host_raw_publish(const HostRawInput *in)
{
    __atomic_store_n(&sSeq, sSeq + 1, __ATOMIC_SEQ_CST);
    memcpy(&sPublished, in, sizeof sPublished);
    __atomic_store_n(&sSeq, sSeq + 1, __ATOMIC_SEQ_CST);
}

typedef struct RawStep { unsigned poll; HostRawInput state; } RawStep;
static RawStep *sRawSteps;
static int sRawStepCount, sRawLoaded;
static unsigned sPollCount;

/* One token of KYUJO_RAW into `st`; a bad token stops the program (a typo in a test script must not be silently ignored). */
static void raw_token(HostRawInput *st, const char *tok)
{
    int key = host_key_from_name(tok);
    if (key != HK_NONE) { st->key[key] = 1; return; }
    if (strncmp(tok, "pad", 3) == 0 && tok[3] >= '1' && tok[3] <= '0' + HOST_PADS && tok[4] == '.') {
        HostRawPad *p = &st->pad[tok[3] - '1'];
        const char *what = tok + 5, *eq = strchr(what, '=');
        p->connected = 1;
        if (!p->name[0]) snprintf(p->name, sizeof p->name, "%s %d", p->generic ? "Generic pad" : "XInput controller", tok[3] - '0');
        if (eq) {
            long v = strtol(eq + 1, NULL, 10);
            size_t n = (size_t)(eq - what);
            if (n == 2 && strncmp(what, "LX", 2) == 0) { p->lx = (short)v; return; }
            if (n == 2 && strncmp(what, "LY", 2) == 0) { p->ly = (short)v; return; }
            if (n == 2 && strncmp(what, "RX", 2) == 0) { p->rx = (short)v; return; }
            if (n == 2 && strncmp(what, "RY", 2) == 0) { p->ry = (short)v; return; }
            if (n == 2 && strncmp(what, "LT", 2) == 0) { p->lt = (unsigned char)v; return; }
            if (n == 2 && strncmp(what, "RT", 2) == 0) { p->rt = (unsigned char)v; return; }
            if (n == 3 && strncmp(what, "AX", 2) == 0 && what[2] >= '0' && what[2] <= '5') { p->axis[what[2] - '0'] = (short)v; if (what[2] == '0') p->lx = (short)v; if (what[2] == '1') p->ly = (short)-v; return; }
            if (n == 4 && strncmp(what, "name", 4) == 0) {
                size_t i;
                snprintf(p->name, sizeof p->name, "%s", eq + 1);
                for (i = 0; p->name[i]; i++) if (p->name[i] == '_') p->name[i] = ' ';
                return;
            }
        } else if (strcmp(what, "ON") == 0) {
            return;                                                   /* "pad1.ON": connected, nothing pressed */
        } else if (strcmp(what, "generic") == 0) {
            p->generic = 1;
            if (strncmp(p->name, "XInput controller", 17) == 0) snprintf(p->name, sizeof p->name, "Generic pad %d", tok[3] - '0');
            return;
        } else {
            int b = host_pad_button_from_name(what);
            if (b >= 0 && (b < PB_XBOX_PLAIN || (b >= PB_G_BTN1 && b < PB_G_AX_X_NEG))) { p->down[b] = 1; return; }
        }
    }
    fprintf(stderr, "kyujo: KYUJO_RAW: unknown token '%s'\n", tok);
    exit(2);
}

static void load_raw_script(void)
{
    const char *env = getenv("KYUJO_RAW");
    sRawLoaded = 1;
    if (!env || !*env) return;
    sRawSteps = (RawStep *)calloc(4096, sizeof(RawStep));
    if (!sRawSteps) return;
    while (*env) {
        char *end;
        unsigned long n = strtoul(env, &end, 10);
        const char *p, *seg_end;
        RawStep *st;
        if (end == env || *end != 'p' || end[1] != ':' || sRawStepCount >= 4096) {
            fprintf(stderr, "kyujo: KYUJO_RAW: cannot parse near '%s' (format <poll>p:<token>+<token>,...)\n", env);
            exit(2);
        }
        st = &sRawSteps[sRawStepCount++];
        st->poll = (unsigned)n;
        p = end + 2;
        seg_end = p + strcspn(p, ",");
        while (p < seg_end) {
            char tok[64];
            size_t tl = strcspn(p, "+,");
            if (tl >= sizeof tok) { fprintf(stderr, "kyujo: KYUJO_RAW: token too long\n"); exit(2); }
            memcpy(tok, p, tl);
            tok[tl] = 0;
            if (tl) raw_token(&st->state, tok);
            p += tl;
            if (*p == '+') p++;
        }
        env = seg_end;
        if (*env == ',') env++;
    }
}

/* Called once per menu poll (host_menu.c): the script's clock. */
void host_raw_script_tick(void) { sPollCount++; }
unsigned host_raw_script_count(void) { return sPollCount; }       /* for the menu trace */

void host_raw_get(HostRawInput *out)
{
    unsigned s0, s1;
    if (!sRawLoaded) load_raw_script();
    if (sRawStepCount > 0) {
        int i, cur = -1;
        for (i = 0; i < sRawStepCount && sRawSteps[i].poll <= sPollCount; i++) cur = i;
        if (cur >= 0) memcpy(out, &sRawSteps[cur].state, sizeof *out); else memset(out, 0, sizeof *out);
        return;
    }
    do {                                                              /* seqlock read: retry while the window thread writes */
        s0 = __atomic_load_n(&sSeq, __ATOMIC_SEQ_CST);
        memcpy(out, &sPublished, sizeof *out);
        s1 = __atomic_load_n(&sSeq, __ATOMIC_SEQ_CST);
    } while ((s0 & 1) || s0 != s1);
}

/* ---- bindings: raw devices to N64 buttons ---- */

#define STICK_AS_DPAD 32                                              /* stick tilt (of 80) from which the stick also counts as a D-pad press */
#define STICK_DIGITAL 80                                              /* tilt that a key or button bound to a stick direction gives */
#define STICK_MAX 80                                                  /* what a console stick reaches at full tilt (documented in the N64 controller literature) */
#define AXIS_THRESHOLD 16000                                          /* a stick counts as a button beyond this (XInput's own right-stick threshold) */
#define TRIGGER_THRESHOLD 100

void host_pad_resolve(const HostRawPad *p, unsigned char out[PB_COUNT])
{
    int i;
    memset(out, 0, PB_COUNT);
    if (!p->generic) {
        for (i = 0; i < PB_XBOX_PLAIN; i++) out[i] = p->down[i] ? 1 : 0;
        out[PB_LT] = p->lt > TRIGGER_THRESHOLD;
        out[PB_RT] = p->rt > TRIGGER_THRESHOLD;
        out[PB_LS_UP] = p->ly > AXIS_THRESHOLD;
        out[PB_LS_DOWN] = p->ly < -AXIS_THRESHOLD;
        out[PB_LS_LEFT] = p->lx < -AXIS_THRESHOLD;
        out[PB_LS_RIGHT] = p->lx > AXIS_THRESHOLD;
        out[PB_RS_UP] = p->ry > AXIS_THRESHOLD;
        out[PB_RS_DOWN] = p->ry < -AXIS_THRESHOLD;
        out[PB_RS_LEFT] = p->rx < -AXIS_THRESHOLD;
        out[PB_RS_RIGHT] = p->rx > AXIS_THRESHOLD;
    } else {
        for (i = PB_G_BTN1; i < PB_G_AX_X_NEG; i++) out[i] = p->down[i] ? 1 : 0;
        for (i = 0; i < 6; i++) {
            out[PB_G_AX_X_NEG + 2 * i] = p->axis[i] < -AXIS_THRESHOLD;
            out[PB_G_AX_X_NEG + 2 * i + 1] = p->axis[i] > AXIS_THRESHOLD;
        }
    }
}

void host_input_map(const HostSettings *s, const HostRawInput *raw, int port, unsigned short *buttons, signed char *sx, signed char *sy, int *connected)
{
    unsigned short bt = 0;
    int x = 0, y = 0, conn = 0, a, i, k;
    if (s->keyboard_port == port) {
        conn = 1;
        for (a = 0; a < ACT_KEY_COUNT; a++) {
            int down = 0;
            for (k = 0; k < 2; k++) if (s->key[a][k] > 0 && s->key[a][k] < HK_COUNT && raw->key[s->key[a][k]]) down = 1;
            if (!down) continue;
            bt |= host_action_n64_bit(a);
            if (a == ACT_STICK_UP) y += STICK_DIGITAL;
            if (a == ACT_STICK_DOWN) y -= STICK_DIGITAL;
            if (a == ACT_STICK_LEFT) x -= STICK_DIGITAL;
            if (a == ACT_STICK_RIGHT) x += STICK_DIGITAL;
        }
    }
    for (i = 0; i < HOST_PADS; i++) {
        const HostRawPad *p = &raw->pad[i];
        const HostPadProfile *prof;
        unsigned char all[PB_COUNT];
        double lx, ly, mag;
        int pi;
        if (!p->connected) continue;
        pi = host_profile_find(s, p);
        if (pi < 0) continue;                                          /* a pad that has no profile yet drives nothing (host_profiles_sync makes one at the next picture) */
        prof = &s->profile[pi];
        if (prof->port != port) continue;
        conn = 1;
        host_pad_resolve(p, all);
        for (a = 0; a < ACT_KEY_COUNT; a++) {
            int down = 0;
            for (k = 0; k < 2; k++) if (prof->pad[a][k] >= 0 && prof->pad[a][k] < PB_COUNT && all[prof->pad[a][k]]) down = 1;
            if (!down) continue;
            bt |= host_action_n64_bit(a);
            if (a == ACT_STICK_UP) y += STICK_DIGITAL;
            if (a == ACT_STICK_DOWN) y -= STICK_DIGITAL;
            if (a == ACT_STICK_LEFT) x -= STICK_DIGITAL;
            if (a == ACT_STICK_RIGHT) x += STICK_DIGITAL;
        }
        /* the left stick drives the N64 stick directly: radial dead zone, then the rest of the range stretched to 0..STICK_MAX */
        lx = p->lx / 32767.0; ly = p->ly / 32767.0;
        mag = sqrt(lx * lx + ly * ly);
        if (mag > prof->deadzone / 100.0) {
            double dz = prof->deadzone / 100.0, scale = (mag > 1.0 ? 1.0 : (mag - dz) / (1.0 - dz)) / mag;
            x += (int)(lx * scale * STICK_MAX);
            y += (int)(ly * scale * STICK_MAX);
        }
    }
    if (x > STICK_MAX) x = STICK_MAX;
    if (x < -STICK_MAX) x = -STICK_MAX;
    if (y > STICK_MAX) y = STICK_MAX;
    if (y < -STICK_MAX) y = -STICK_MAX;
    /* The game reads only the D-pad in its menus, the overworld, the Pokemon choice and GB Tower (the stick is used by two mini-games and the
     * gallery camera only), so a stick tilted past STICK_AS_DPAD also presses the matching D-pad direction. The stick value itself is unchanged. */
    if (s->stick_dpad && conn) {
        if (y >= STICK_AS_DPAD) bt |= 0x0800;                          /* N64 button bits: D-up 0x0800, D-down 0x0400, D-left 0x0200, D-right 0x0100 */
        if (y <= -STICK_AS_DPAD) bt |= 0x0400;
        if (x <= -STICK_AS_DPAD) bt |= 0x0200;
        if (x >= STICK_AS_DPAD) bt |= 0x0100;
    }
    *buttons = bt; *sx = (signed char)x; *sy = (signed char)y; *connected = conn;
}

/* The development command reader's state for port 1 (host_control.c). */
static volatile unsigned sLiveButtons;
static volatile int sLiveX, sLiveY, sLiveUsed;
void host_input_set_live(unsigned short buttons, signed char stick_x, signed char stick_y)
{
    sLiveButtons = buttons;
    sLiveX = stick_x;
    sLiveY = stick_y;
    sLiveUsed = 1;
}

int host_input_read(int channel)
{
    unsigned short buttons = 0;
    signed char sx = 0, sy = 0;
    int connected = 0;
    if (channel < 0 || channel > 3) return -1;
    if (!sScriptLoaded) load_script();
    if (sStepCount > 0) {                                             /* the old test script: controller 1 only */
        double now, frame;
        int i, cur = -1;
        if (channel != 0) return -1;
        now = (double)(host_wall_ns() - sBaseNs) / 1e9;
        frame = (double)host_gfx_frame_count();
        for (i = 0; i < sStepCount && (sSteps[i].is_frame ? sSteps[i].at <= frame : sSteps[i].at <= now); i++) cur = i;
        buttons = cur >= 0 ? sSteps[cur].buttons : 0;
        sx = cur >= 0 ? sSteps[cur].x : 0;
        sy = cur >= 0 ? sSteps[cur].y : 0;
        if (host_menu_is_open()) { buttons = 0; sx = sy = 0; }
        if (getenv("KYUJO_INPUT_TRACE")) {          /* development: shows when the game actually reads a changed state */
            static unsigned short last = 0xFFFF;
            if (buttons != last) { fprintf(stderr, "[%.2fs, frame %.0f] input read: buttons %04X\n", now, frame, buttons); last = buttons; }
        }
        return (int)(((unsigned)buttons << 16) | ((unsigned)(unsigned char)sx << 8) | (unsigned char)sy);
    } else {
        HostRawInput raw;
        host_raw_get(&raw);
        host_input_map(host_settings(), &raw, channel + 1, &buttons, &sx, &sy, &connected);
        if (channel == 0 && sLiveUsed) {                              /* development command reader: added to the devices */
            buttons |= (unsigned short)sLiveButtons;
            if (sLiveX || sLiveY) { sx = (signed char)sLiveX; sy = (signed char)sLiveY; }
            connected = 1;
        }
        if (host_menu_is_open()) { buttons = 0; sx = sy = 0; }       /* the menu has the keys: the game sees nothing while it is open */
        if (!connected && !host_gbpak_present(channel)) return -1;   /* a Game Boy cartridge needs its controller to exist */
    }
    return (int)(((unsigned)buttons << 16) | ((unsigned)(unsigned char)sx << 8) | (unsigned char)sy);
}
