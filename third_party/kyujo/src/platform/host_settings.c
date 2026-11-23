/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_settings.c - the settings (host_ui.h: HostSettings) and their text file, kyujo.cfg.
 *
 * The file is plain text, one `name=value` per line, `#` starts a comment. The in-window menu writes it (host_menu.c); it can also be edited by
 * hand while the program is not running. Unknown names and bad values are logged and skipped, the rest of the file is still used.
 *
 *   game_rom=C:\games\game.z64     the game's ROM
 *   save=kyujo.sav                             the N64 save file
 *   widescreen=0                                     1: the picture is stretched to fill the window instead of keeping 4:3 with black bars
 *   pak_loading_real_speed=0                         advanced, not in the menu: 1: Game Boy cartridge loading takes the console's time (about 27 s); 0 (default): a fifth of it
 *   scale_style=0                                    picture scaling on the GPU presenter: 0 smooth, 1 sharp, 2 firm
 *   gb_cpu_standard=0                                advanced, not in the menu: 1: the Game Boy processor runs on the module's own (slower) interpreter; 0 (default): the fast native one
 *   stick_dpad_off=0                                 advanced, not in the menu: 1: the control stick does not also press the D-pad (by default it does: the game's menus only read the D-pad)
 *   cheats=1                                         L+R knocks out the opponent's Pokemon (L+R+Z wins the battle) in a battle
 *   cheat_hall_of_fame=0                             1: the Hall of Fame (Victory Palace) shows all 151 Pokemon; the missing ones are made up on screen, nothing is written to the save
 *   cheat_cpu_vs_cpu=0                               1: Kids Club: "Ready!" can be chosen with all four players set to COM (computer against computer)
 *   p1_rom=gb\pokemon-yellow.gbc                     the Game Boy cartridge in the Transfer Pak of controller port 1 (p1..p4)
 *   p1_save=gb\pokemon-yellow.sav                    its save file (empty: the ROM's name with .sav)
 *   fullscreen=0, volume=100                         full screen at start; sound volume in percent
 *   keyboard_port=1                                  which N64 controller the keyboard drives (0 = none)
 *   deadzone=24                                      the left stick dead zone (percent) that a newly found gamepad starts with
 *   key_a=X,SPACE                                    keys for the N64 button A (up to two, "none" = no key); key_<action> for every action
 *   profile1_name=Wireless Controller                one entry per gamepad ever seen (profile1..profile16), found again by its name:
 *   profile1_generic=1                               1 = a generic pad (numbered buttons), 0 = Xbox layout
 *   profile1_port=1                                  which N64 controller it drives (0 = none)
 *   profile1_deadzone=24                             its left stick dead zone in percent
 *   profile1_a=BTN2                                  its inputs for the N64 button A (up to two); profile1_<action> for every action
 * (Older files had pad1_port ... and pad_<action>: they are read and ignored, the pads get default profiles.)
 * Relative paths are relative to the program's folder (host_app_dir).
 * HOST-side file (C library only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include "platform.h"
#include "host_ui.h"

static void copy_str(char *dst, unsigned size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

static void set_key(HostSettings *s, int a, int k0, int k1) { s->key[a][0] = k0; s->key[a][1] = k1; }
static void set_pb(HostPadProfile *p, int a, int b0, int b1) { p->pad[a][0] = b0; p->pad[a][1] = b1; }

/* The assignment a gamepad starts with. Xbox layout: the layout of tests 1 to 7 (the left stick is the N64 stick directly; the right stick gives
 * the C buttons). Generic: a guess that fits the common DirectInput layout of DualShock 4 and Logitech F310 style pads (buttons 1 and 3 = the
 * two face buttons on the left and right, 2 = the one below, 5 / 6 = the shoulder buttons, 7 / 8 = the triggers, 9 / 10 = Share, Options; right stick =
 * axes Z and R). It is only a start: the controller menu lets the player press the button for every N64 button. */
void host_profile_set_defaults(HostPadProfile *p, int generic)
{
    int a;
    p->generic = generic;
    p->deadzone = 24;                                       /* XInput's recommended left dead zone is 7849/32767 = 24 % */
    for (a = 0; a < ACT_COUNT; a++) set_pb(p, a, -1, -1);
    if (!generic) {
        set_pb(p, ACT_A, PB_A, -1);           set_pb(p, ACT_B, PB_X, PB_B);        set_pb(p, ACT_Z, PB_LT, PB_RT);
        set_pb(p, ACT_START, PB_START, -1);
        set_pb(p, ACT_DUP, PB_DUP, -1);       set_pb(p, ACT_DDOWN, PB_DDOWN, -1);  set_pb(p, ACT_DLEFT, PB_DLEFT, -1); set_pb(p, ACT_DRIGHT, PB_DRIGHT, -1);
        set_pb(p, ACT_L, PB_LB, -1);          set_pb(p, ACT_R, PB_RB, -1);
        set_pb(p, ACT_CUP, PB_RS_UP, -1);     set_pb(p, ACT_CDOWN, PB_RS_DOWN, -1); set_pb(p, ACT_CLEFT, PB_RS_LEFT, -1); set_pb(p, ACT_CRIGHT, PB_RS_RIGHT, -1);
        set_pb(p, ACT_MENU, PB_BACK, -1);
    } else {
#define GB(n) (PB_G_BTN1 + (n) - 1)
        set_pb(p, ACT_A, GB(2), -1);          set_pb(p, ACT_B, GB(1), GB(3));      set_pb(p, ACT_Z, GB(7), GB(8));
        set_pb(p, ACT_START, GB(10), -1);
        set_pb(p, ACT_DUP, PB_G_HAT_UP, -1);  set_pb(p, ACT_DDOWN, PB_G_HAT_DOWN, -1); set_pb(p, ACT_DLEFT, PB_G_HAT_LEFT, -1); set_pb(p, ACT_DRIGHT, PB_G_HAT_RIGHT, -1);
        set_pb(p, ACT_L, GB(5), -1);          set_pb(p, ACT_R, GB(6), -1);
        set_pb(p, ACT_CUP, PB_G_AX_X_NEG + 6, -1);   set_pb(p, ACT_CDOWN, PB_G_AX_X_NEG + 7, -1);          /* axis R: up is negative */
        set_pb(p, ACT_CLEFT, PB_G_AX_X_NEG + 4, -1); set_pb(p, ACT_CRIGHT, PB_G_AX_X_NEG + 5, -1);         /* axis Z */
        set_pb(p, ACT_MENU, GB(9), -1);
#undef GB
    }
}

int host_pak_time_percent(void)
{
    static int dev = -2;   /* development: KYUJO_PAK_PERCENT=<n> overrides the Transfer Pak time (percent of the console's) */
    if (dev == -2) { const char *e = getenv("KYUJO_PAK_PERCENT"); dev = (e && atoi(e) > 0) ? atoi(e) : -1; }
    if (dev > 0) return dev;
    return host_settings()->pak_real_speed ? 100 : 20;
}

void host_settings_defaults(HostSettings *s)
{
    int a;
    memset(s, 0, sizeof *s);
    s->cheats = 1;
    s->stick_dpad = 1;
    s->pak_real_speed = 0;
    s->gb_fast_cpu = 1;                                     /* the fast Game Boy processor is the default (GB Tower lags with the standard one); gb_cpu_standard=1 in the file switches it off */
    s->pak_setup_done = 0;
    s->scale_style = 0;
#if defined(__wasm__)
    s->widescreen = 1;                     /* console and handheld screens are 16:9: the picture fills them unless the player asks for 4:3 */
#else
    s->widescreen = 0;
#endif
    s->volume = 100;
    s->keyboard_port = 1;
    s->deadzone = 24;                                       /* XInput's recommended left dead zone is 7849/32767 = 24 % */
    for (a = 0; a < ACT_COUNT; a++) set_key(s, a, HK_NONE, HK_NONE);
    /* keyboard: the layout of test 1 to 7 */
    set_key(s, ACT_A, 'X', HK_SPACE);      set_key(s, ACT_B, 'Z', HK_BACKSPACE); set_key(s, ACT_Z, 'C', HK_NONE);
    set_key(s, ACT_START, HK_ENTER, HK_NONE);
    set_key(s, ACT_DUP, 'W', HK_NONE);     set_key(s, ACT_DDOWN, 'S', HK_NONE);  set_key(s, ACT_DLEFT, 'A', HK_NONE); set_key(s, ACT_DRIGHT, 'D', HK_NONE);
    set_key(s, ACT_L, 'Q', HK_NONE);       set_key(s, ACT_R, 'E', HK_NONE);
    set_key(s, ACT_CUP, 'I', HK_NONE);     set_key(s, ACT_CDOWN, 'K', HK_NONE);  set_key(s, ACT_CLEFT, 'J', HK_NONE); set_key(s, ACT_CRIGHT, 'L', HK_NONE);
    set_key(s, ACT_STICK_UP, HK_UP, HK_NONE);   set_key(s, ACT_STICK_DOWN, HK_DOWN, HK_NONE);
    set_key(s, ACT_STICK_LEFT, HK_LEFT, HK_NONE); set_key(s, ACT_STICK_RIGHT, HK_RIGHT, HK_NONE);
}

/* ---- gamepad profiles ---- */

int host_profile_find(const HostSettings *s, const HostRawPad *raw)
{
    int i;
    for (i = 0; i < HS_PROFILES; i++) {
        if (s->profile[i].name[0] && strcmp(s->profile[i].name, raw->name) == 0 && s->profile[i].generic == (raw->generic ? 1 : 0)) return i;
    }
    return -1;
}

int host_profiles_sync(HostSettings *s, const HostRawInput *raw)
{
    int created = 0, used = 0, i, j;
    for (i = 0; i < HOST_PADS; i++) {                       /* which N64 controllers the connected pads that already have a profile drive */
        int k;
        if (!raw->pad[i].connected) continue;
        k = host_profile_find(s, &raw->pad[i]);
        if (k >= 0 && s->profile[k].port > 0) used |= 1 << s->profile[k].port;
    }
    for (i = 0; i < HOST_PADS; i++) {
        int slot = -1, port = 0;
        const HostRawPad *p = &raw->pad[i];
        if (!p->connected || !p->name[0] || host_profile_find(s, p) >= 0) continue;
        for (j = 0; j < HS_PROFILES && slot < 0; j++) if (!s->profile[j].name[0]) slot = j;
        for (j = 0; j < HS_PROFILES && slot < 0; j++) {      /* table full: forget a gamepad that is not connected now */
            int k, in_use = 0;
            for (k = 0; k < HOST_PADS; k++) if (raw->pad[k].connected && host_profile_find(s, &raw->pad[k]) == j) in_use = 1;
            if (!in_use) slot = j;
        }
        if (slot < 0) continue;
        for (j = 1; j <= 4 && !port; j++) if (!(used >> j & 1)) port = j;
        memset(&s->profile[slot], 0, sizeof s->profile[slot]);
        snprintf(s->profile[slot].name, sizeof s->profile[slot].name, "%s", p->name);
        host_profile_set_defaults(&s->profile[slot], p->generic ? 1 : 0);
        s->profile[slot].deadzone = s->deadzone;
        s->profile[slot].port = port;
        if (port) used |= 1 << port;
        created++;
    }
    return created;
}

HostSettings *host_settings(void)
{
    static HostSettings s;
    static int init;
    if (!init) { host_settings_defaults(&s); init = 1; }
    return &s;
}

const char *host_settings_path(void)
{
    static char path[1100];
    const char *env = getenv("KYUJO_CFG");
    if (env && env[0]) copy_str(path, sizeof path, env);
    else host_app_path(path, sizeof path, "kyujo.cfg");
    return path;
}

/* ---- parsing ---- */

static char *trim(char *p)
{
    char *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return p;
}

/* "X,SPACE" / "none" / "A" into up to two values. Returns 0 if a name is unknown or there are more than two. */
static int parse_list(const char *v, int is_key, int out[2])
{
    char buf[128];
    const char *p = buf;
    int n = 0;
    out[0] = out[1] = is_key ? HK_NONE : -1;
    if (strlen(v) >= sizeof buf) return 0;
    strcpy(buf, v);
    for (;;) {
        char *tok = (char *)p, *comma = strchr(tok, ','), *t;
        int id;
        if (comma) *comma = 0;
        t = trim(tok);
        if (!(t[0] == 0 || strcmp(t, "none") == 0 || strcmp(t, "NONE") == 0)) {
            id = is_key ? host_key_from_name(t) : host_pad_button_from_name(t);
            if ((is_key && id == HK_NONE) || (!is_key && id < 0)) return 0;
            if (n >= 2) return 0;
            out[n++] = id;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 1;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *trim(end) != 0 || x < lo || x > hi) return 0;
    *out = (int)x;
    return 1;
}

#if defined(__wasm__)
#define WIDE_KEY "fill_screen"
#else
#define WIDE_KEY "widescreen"
#endif

static int apply_line(HostSettings *s, const char *name, const char *val)
{
    int a, i;
    if (strcmp(name, "game_rom") == 0) { copy_str(s->rom, sizeof s->rom, val); return 1; }
    if (strcmp(name, "save") == 0) { copy_str(s->save, sizeof s->save, val); return 1; }
    if (strcmp(name, "cheats") == 0) return parse_int(val, 0, 1, &s->cheats);
    if (strcmp(name, "cheat_hall_of_fame") == 0) return parse_int(val, 0, 1, &s->cheat_hof);
    if (strcmp(name, "cheat_cpu_vs_cpu") == 0) return parse_int(val, 0, 1, &s->cheat_cpuvs);
#if defined(__wasm__)
    if (strcmp(name, "fill_screen") == 0) return parse_int(val, 0, 1, &s->widescreen);     /* the shell build's own key: "widescreen=" saved by earlier test builds never had an effect there, so it is not read */
    if (strcmp(name, "widescreen") == 0) return 1;
#else
    if (strcmp(name, "widescreen") == 0) return parse_int(val, 0, 1, &s->widescreen);
#endif
    if (strcmp(name, "pak_loading_real_speed") == 0) return parse_int(val, 0, 1, &s->pak_real_speed);
    if (strcmp(name, "pak_real_speed") == 0 || strcmp(name, "gb_fast_cpu") == 0) return 1;     /* the menu keys of test builds 10-30: ignored, so that the new defaults (faster loading, fast processor) apply to every settings file */
    if (strcmp(name, "scale_style") == 0) return parse_int(val, 0, 2, &s->scale_style);
    if (strcmp(name, "gb_cpu_standard") == 0) { int v = 0, r = parse_int(val, 0, 1, &v); if (r) s->gb_fast_cpu = !v; return r; }
    if (strcmp(name, "pak_setup_done") == 0) return parse_int(val, 0, 1, &s->pak_setup_done);
    if (strcmp(name, "fast_pak") == 0) return 1;                             /* the key of test 10 (default was "real speed"); ignored so that the new default (faster) applies */
    if (strcmp(name, "stick_dpad") == 0) return 1;                           /* the menu key of earlier builds: ignored, the stick moving the D-pad is the default now */
    if (strcmp(name, "stick_dpad_off") == 0) { int v = 0, r = parse_int(val, 0, 1, &v); if (r) s->stick_dpad = !v; return r; }
    if (strcmp(name, "fullscreen") == 0) return parse_int(val, 0, 1, &s->fullscreen);
    if (strcmp(name, "volume") == 0) return parse_int(val, 0, 100, &s->volume);
    if (strcmp(name, "keyboard_port") == 0) return parse_int(val, 0, 4, &s->keyboard_port);
    if (strcmp(name, "deadzone") == 0) return parse_int(val, 0, 50, &s->deadzone);
    for (i = 0; i < 4; i++) {
        char k[16];
        snprintf(k, sizeof k, "p%d_rom", i + 1);  if (strcmp(name, k) == 0) { copy_str(s->gb_rom[i], HS_PATH, val); return 1; }
        snprintf(k, sizeof k, "p%d_save", i + 1); if (strcmp(name, k) == 0) { copy_str(s->gb_save[i], HS_PATH, val); return 1; }
        snprintf(k, sizeof k, "pad%d_port", i + 1); if (strcmp(name, k) == 0) return 1;      /* the format before the gamepad profiles: ignored */
    }
    for (a = 0; a < ACT_KEY_COUNT; a++) {
        char k[32];
        snprintf(k, sizeof k, "key_%s", host_action_name(a)); if (strcmp(name, k) == 0) return parse_list(val, 1, s->key[a]);
        snprintf(k, sizeof k, "pad_%s", host_action_name(a)); if (strcmp(name, k) == 0) return 1;                    /* ditto */
    }
    if (strncmp(name, "profile", 7) == 0) {                 /* profile<N>_name, _generic, _port, _<action> */
        char *end;
        long n = strtol(name + 7, &end, 10);
        HostPadProfile *p;
        if (end == name + 7 || *end != '_' || n < 1 || n > HS_PROFILES) return -1;
        p = &s->profile[n - 1];
        end++;
        if (strcmp(end, "name") == 0) {
            if (strlen(val) >= sizeof p->name || val[0] == 0) return 0;
            if (!p->name[0]) host_profile_set_defaults(p, 0);              /* a profile that the file describes only in part keeps the defaults for the rest */
            copy_str(p->name, sizeof p->name, val);
            return 1;
        }
        if (strcmp(end, "generic") == 0) {
            int g;
            if (!parse_int(val, 0, 1, &g)) return 0;
            if (g != p->generic) host_profile_set_defaults(p, g);          /* the kind decides the numbering of the inputs: defaults of that kind, then the file's lines */
            return 1;
        }
        if (strcmp(end, "port") == 0) return parse_int(val, 0, 4, &p->port);
        if (strcmp(end, "deadzone") == 0) return parse_int(val, 0, 50, &p->deadzone);
        for (a = 0; a < ACT_COUNT; a++) if (strcmp(end, host_action_name(a)) == 0) return parse_list(val, 0, p->pad[a]);
    }
    return -1;                                              /* unknown name */
}

int host_settings_parse(HostSettings *s, const char *text)
{
    int problems = 0, lineno = 0;
    const char *p = text;
    while (*p) {
        char line[1100], *eq, *name, *val;
        size_t n = strcspn(p, "\n");
        int r;
        lineno++;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += strcspn(p, "\n");
        if (*p == '\n') p++;
        name = trim(line);
        if (name[0] == 0 || name[0] == '#') continue;
        eq = strchr(name, '=');
        if (!eq) { fprintf(stderr, "kyujo: settings: line %d has no '=': %s\n", lineno, name); problems++; continue; }
        *eq = 0;
        val = trim(eq + 1);
        name = trim(name);
        r = apply_line(s, name, val);
        if (r < 0) { fprintf(stderr, "kyujo: settings: line %d: unknown name '%s' (ignored)\n", lineno, name); problems++; }
        else if (r == 0) { fprintf(stderr, "kyujo: settings: line %d: bad value '%s' for '%s' (ignored)\n", lineno, val, name); problems++; }
    }
    return problems;
}

int host_settings_load(HostSettings *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long n;
    int problems;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1000000) { fclose(f); fprintf(stderr, "kyujo: settings: '%s' has a strange size\n", path); return -1; }
    text = (char *)malloc((size_t)n + 1);
    if (!text) { fclose(f); return -1; }
    if (fread(text, 1, (size_t)n, f) != (size_t)n) { free(text); fclose(f); return -1; }
    text[n] = 0;
    fclose(f);
    problems = host_settings_parse(s, text);
    free(text);
    return problems;
}

/* ---- writing ---- */

typedef struct Out { char *buf; unsigned size, len; int overflow; } Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = o->len < o->size ? vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap) : 0;
    va_end(ap);
    if (n < 0 || o->len + (unsigned)n >= o->size) { o->overflow = 1; return; }
    o->len += (unsigned)n;
}

static void put_list(Out *o, const char *prefix, const char *action, const int v[2], int is_key)
{
    const char *n0 = is_key ? host_key_name(v[0]) : host_pad_button_name(v[0]);
    const char *n1 = is_key ? host_key_name(v[1]) : host_pad_button_name(v[1]);
    put(o, "%s_%s=", prefix, action);
    if (n0 && n1) put(o, "%s,%s\n", n0, n1);
    else if (n0) put(o, "%s\n", n0);
    else if (n1) put(o, "%s\n", n1);
    else put(o, "none\n");
}

int host_settings_format(const HostSettings *s, char *out, unsigned size)
{
    Out o = { out, size, 0, 0 };
    int a, i;
    if (size) out[0] = 0;
    put(&o, "# Kyujo: settings. The in-game menu (F1, Esc, or a gamepad's menu button) writes this file;\n"
            "# you can also edit it while the program is not running. Relative paths are relative to the program's folder.\n\n");
    put(&o, "game_rom=%s\nsave=%s\ncheats=%d\ncheat_hall_of_fame=%d\ncheat_cpu_vs_cpu=%d\nstick_dpad_off=%d\npak_loading_real_speed=%d\ngb_cpu_standard=%d\npak_setup_done=%d\nscale_style=%d\n" WIDE_KEY "=%d\nfullscreen=%d\nvolume=%d\n\n", s->rom, s->save, s->cheats, s->cheat_hof, s->cheat_cpuvs, !s->stick_dpad, s->pak_real_speed, !s->gb_fast_cpu, s->pak_setup_done, s->scale_style, s->widescreen, s->fullscreen, s->volume);
    put(&o, "# Game Boy cartridges in the Transfer Paks of controller ports 1 to 4 (for GB Tower and the Pokemon Lab).\n"
            "# The save file is changed when the game saves; the original is kept once as <save>.bak.\n");
    for (i = 0; i < 4; i++) put(&o, "p%d_rom=%s\np%d_save=%s\n", i + 1, s->gb_rom[i], i + 1, s->gb_save[i]);
    put(&o, "\n# Controllers: which N64 port (1-4, 0 = none) the keyboard drives. deadzone = the left stick dead zone (percent) of a newly found gamepad; every gamepad has its own below.\n");
    put(&o, "keyboard_port=%d\ndeadzone=%d\n\n# Keyboard keys for each N64 button (up to two each; none = unused).\n", s->keyboard_port, s->deadzone);
    for (a = 0; a < ACT_KEY_COUNT; a++) put_list(&o, "key", host_action_name(a), s->key[a], 1);
    put(&o, "\n# One entry per gamepad that was connected (found again by its name): its N64 controller and its inputs for each N64 button.\n");
    for (i = 0; i < HS_PROFILES; i++) {
        char prefix[24];
        const HostPadProfile *p = &s->profile[i];
        if (!p->name[0]) continue;
        put(&o, "profile%d_name=%s\nprofile%d_generic=%d\nprofile%d_port=%d\nprofile%d_deadzone=%d\n", i + 1, p->name, i + 1, p->generic, i + 1, p->port, i + 1, p->deadzone);
        snprintf(prefix, sizeof prefix, "profile%d", i + 1);
        for (a = 0; a < ACT_COUNT; a++) put_list(&o, prefix, host_action_name(a), p->pad[a], 0);
    }
    if (o.overflow) return -1;
    return (int)o.len;
}

int host_settings_save(const HostSettings *s, const char *path)
{
    /* On the heap, not a static array: 64 KiB of static data would use up room below 0x01000000 that the memory model needs (tools/native/imagelimit.py). */
    char *text = (char *)malloc(65536);
    char tmp[1200];
    FILE *f;
    int n;
    if (!text) return 1;
    n = host_settings_format(s, text, 65536);
    if (n < 0) { free(text); return 1; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(text, 1, (size_t)n, f) != (size_t)n || fflush(f) != 0) {
        if (f) fclose(f);
        free(text);
        fprintf(stderr, "kyujo: settings: cannot write '%s'\n", tmp);
        return 1;
    }
    free(text);
    fclose(f);
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { fprintf(stderr, "kyujo: settings: cannot replace '%s'\n", path); return 1; }   /* rename() would refuse to replace */
#else
    if (rename(tmp, path) != 0) { fprintf(stderr, "kyujo: settings: cannot replace '%s'\n", path); return 1; }
#endif
    return 0;
}
