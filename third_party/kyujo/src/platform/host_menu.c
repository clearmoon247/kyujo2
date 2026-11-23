/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_menu.c - the in-window menu: Game Boy cartridges, controllers (keyboard and every gamepad), Game ROM and save file, options (full screen,
 * sound, cheat), information, restart and quit. Also the short messages ("notices") drawn over the game picture.
 *
 * Looks: the game's own font and, as the background, the background of the game's Game Pak Check page (host_gameui.c reads both from the ROM).
 * Everything is drawn on a 640 x 480 canvas, the game's own hi-res coordinates.
 *
 * Opening: F1 or Esc on the keyboard, a gamepad's menu button (Back on an Xbox-layout pad; assignable), or host_menu_request() (the Windows menu
 * bar asks for a screen). While the menu is open the game is PAUSED: host_menu_poll does not return (it is called by the video output once per
 * picture), the game's clock stands still (host_clock_pause) and the game sees no buttons.
 * Inside the menu: arrow keys or D-pad / left stick move, Enter or A chooses, Esc / Backspace or B goes back, Left/Right change a value,
 * Page Up / Page Down (shoulder buttons) jump in long lists. F1 closes it from anywhere. A gamepad moves through the menu with its own
 * bindings (its D-pad / stick / A / B / Start / L / R actions, see the controller screens).
 *
 * Changes are applied at once (buttons, cheat switch, volume, full screen) except: the Game ROM and save file are read when the program starts, and a
 * Game Boy cartridge is loaded at once but the GAME only looks for cartridges when it starts (GbSave_ScanAllPorts, from the n64_logo fragment).
 * After such a change the menu asks "Restart now / Later" when the player leaves the screen (or closes the menu) and offers "Restart game"
 * (host_restart). The settings are written to kyujo.cfg when the menu closes (or before a restart / quit).
 *
 * Development variables: KYUJO_MENU_DUMP=<dir> writes every menu picture as menu_NNNN.png; KYUJO_MENU_TRACE=1 prints what opened the
 * menu; KYUJO_MENU_OPEN=<n> opens it at screen n (the HM_* numbers of host_ui.h, as the Windows menu bar does); the keys can be scripted with
 * KYUJO_RAW (host_input.c).
 *
 * Nothing here needs the Windows API: the devices come in as a HostRawInput (host_raw_get), the picture goes out through host_window_present.
 * A Switch window layer would reuse this file unchanged (and only has this menu, no menu bar). HOST-side file (C library only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "host_ui.h"
#include "../render/gfx.h"

void host_raw_script_tick(void);                 /* host_input.c: the clock of the KYUJO_RAW test script */
unsigned host_raw_script_count(void);

/* ---- colours and layout (canvas pixels) -------------------------------------------------------------------------------------- */
#define COL_TEXT 0xFFFFFFu
#define COL_DIM 0xB4C0D8u
#define COL_SEL 0xFFE680u
#define COL_ERR 0xFF8C78u
#define COL_OK 0x9CF0A8u
#define PANEL_X 28
#define PANEL_Y 20
#define PANEL_W 584
#define PANEL_H 440
#define LIST_X 52
#define LIST_Y 84
#define VALUE_X 352
#define RIGHT_X 586
#define PITCH 22
#define MSG_Y 396
#define HELP_Y 424
#define SZ 2                                     /* the game's 16 pixel letters for the lists */
#define SZ_TITLE 4                               /* 24 pixel letters for the heading */

/* ---- the notices on the game picture ------------------------------------------------------------------------------------------ */
#define NOTICE_MAX 4
#define NOTICE_NS 4500000000ULL
static struct { char text[100]; unsigned long long until; } sNotice[NOTICE_MAX];

void host_menu_notice(const char *text)
{
    int i;
    unsigned long long now = host_wall_ns();
    for (i = 0; i < NOTICE_MAX; i++) {                        /* the same text again only renews it */
        if (sNotice[i].until > now && strcmp(sNotice[i].text, text) == 0) { sNotice[i].until = now + NOTICE_NS; return; }
    }
    for (i = 0; i < NOTICE_MAX; i++) if (sNotice[i].until <= now) break;
    if (i == NOTICE_MAX) { memmove(&sNotice[0], &sNotice[1], sizeof sNotice[0] * (NOTICE_MAX - 1)); i = NOTICE_MAX - 1; }
    snprintf(sNotice[i].text, sizeof sNotice[i].text, "%s", text);
    sNotice[i].until = now + NOTICE_NS;
}

int host_menu_overlay_active(void)
{
    unsigned long long now = host_wall_ns();
    int i;
    for (i = 0; i < NOTICE_MAX; i++) if (sNotice[i].until > now) return 1;
    return 0;
}

void host_menu_overlay(unsigned char *rgb, unsigned w, unsigned h)
{
    HostCanvas cv;
    unsigned long long now = host_wall_ns();
    int i, y = 8, any = 0;
    for (i = 0; i < NOTICE_MAX; i++) if (sNotice[i].until > now) any = 1;
    if (!any || !host_gameui_font_ok(SZ)) return;
    cv.rgb = rgb; cv.w = (int)w; cv.h = (int)h;
    cv.fx = (unsigned)(((unsigned long long)w << 16) / HUI_W);
    cv.fy = (unsigned)(((unsigned long long)h << 16) / HUI_H);
    for (i = 0; i < NOTICE_MAX; i++) {
        int tw;
        if (sNotice[i].until <= now) continue;
        tw = host_text_width(SZ, sNotice[i].text);
        host_canvas_fill(&cv, 8, y, tw + 16, 24, 0x000000u, 170);
        host_text_draw(&cv, SZ, 16, y + 4, sNotice[i].text, COL_TEXT, 1);
        y += 28;
    }
}

/* ---- state ---------------------------------------------------------------------------------------------------------------------- */
typedef enum {
    SC_MAIN, SC_PAKS, SC_PAK, SC_CTRL, SC_DEVICE, SC_PADTEST, SC_GAMEROM, SC_OPTIONS, SC_ABOUT, SC_BROWSER, SC_QUIT, SC_RESTART, SC_ASK
} Screen;
typedef enum { K_ACTION, K_CHOICE, K_LABEL } Kind;
typedef struct Item { char label[260]; char value[200]; int id; Kind kind; } Item;

enum { BP_GAME_ROM, BP_GAME_SAVE, BP_GB_ROM, BP_GB_SAVE };          /* what the file browser is for */
enum {                                                                      /* item ids */
    I_NONE = 0, I_BACK, I_RESUME, I_PAKS, I_CTRL, I_CHEAT, I_GAMEROM, I_QUIT, I_QUIT_YES, I_RESTART, I_RESTART_YES, I_ASK_LATER,
    I_OPTIONS, I_ABOUT, I_EXIT, I_FULL, I_VOLUME, I_QUIT_CLOSE, I_SCALE, I_WIDE, I_CHEAT_HOF, I_CHEAT_CPUVS,
    I_PAK_ROM, I_PAK_SAVE, I_PAK_REMOVE, I_PAK_DEFAULT_SAVE,
    I_DEVPORT, I_DEAD, I_DEVSETUP, I_DEVTEST, I_DEVRESET, I_DEVFORGET, I_DEVKB,
    I_ST_ROM, I_ST_SAVE, I_ST_NEW, I_ST_BACKUP, I_ST_DEFAULT,
    I_BR_PARENT, I_BR_NEW, I_BR_ALL,
    I_PORT0 = 100,                                                          /* + n: Game Pak port n */
    I_DEV0 = 120,                                                           /* + n: gamepad profile n (HS_PROFILES of them) */
    I_ACT0 = 200,                                                           /* + action: a binding row */
    I_ENTRY0 = 1000                                                         /* + n: entry n of the browser */
};

#define MAX_ITEMS 2300
static Item *sItems;
static int sN;
static Screen sStack[8];
static int sCursor[8], sScroll[8];
static int sDepth;
static int sOpen;
static int sBlockUntilRelease;                  /* after closing: the game sees nothing until all keys and buttons that were down are up */
static int sDirtySettings;
static int sPak;                                /* the controller port (0..3) the Game Paks screen works on */
#define DEV_KEYBOARD (-2)
static int sDev = -1;                           /* the device the controller screens work on: a gamepad profile (index into HostSettings.profile), DEV_KEYBOARD, or -1 */
static int sRestartNeeded;                      /* a change that only a restart brings into effect has been made */
static int sAsked;                              /* the player has been asked about it (not asked again until the next such change) */
static int sBoot;                               /* the Game Pak Setup page is shown at start-up, before the game runs (host_first_run_setup): a cartridge is seen at once, no restart is needed */
static int sSetup;                              /* the first-start Game Pak Setup page is on (HM_SETUP): the Game Boy cartridges screen is the root, and closing it marks the page as shown */
static int sAskThenClose;                       /* the question came from closing the menu: close it after "Later" */
static char sMessage[300];
static unsigned sMessageColor;
static unsigned long long sMessageUntil;
static HostRawInput sPrev;                      /* the snapshot of the previous menu iteration (for "newly pressed") */
static HostRawInput sRawNow;                    /* the snapshot of this iteration (the screens that list devices read it) */
static volatile int sRequest = -1;              /* a HM_* screen asked for by host_menu_request, or -1 */

/* file browser */
static struct {
    char dir[1100];
    HostDirEntry *e;
    int n;
    int purpose, port;
    const char *exts;                           /* "z64;n64;v64" (lower case) */
    int show_all;
    int can_create;
    char new_name[300];
} sBr;

#ifdef HOST_SHELL_PICKER
static int sPicking;                            /* the system's file picker is open for a Game Boy file (Android): its answer is taken in the menu loop */
#endif

static void message(const char *text, unsigned color)
{
    snprintf(sMessage, sizeof sMessage, "%s", text);
    sMessageColor = color;
    sMessageUntil = host_wall_ns() + 9000000000ULL;
}

static void changed(void) { sDirtySettings = 1; }
static void needs_restart(void) { if (sBoot) return; sRestartNeeded = 1; sAsked = 0; }

void host_menu_request(int screen) { __atomic_store_n(&sRequest, screen, __ATOMIC_SEQ_CST); }

static unsigned long long sBlockDeadline;       /* ... at the latest until then */
/* ---- helpers -------------------------------------------------------------------------------------------------------------------- */
static void join_path(char *out, unsigned size, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l == 0) snprintf(out, size, "%s", name);
    else snprintf(out, size, "%s%s%s", dir, (dir[l - 1] == '/' || dir[l - 1] == '\\') ? "" : host_path_separator(), name);
}

/* Text that fits `maxw` pixels: a too long one keeps its end (the file name) behind "...". */
static const char *fit(const char *text, int maxw, int size)
{
    static char buf[4][300];
    static int rot;
    char *b = buf[rot++ & 3];
    size_t len = strlen(text), cut;
    if (host_text_width(size, text) <= maxw) { snprintf(b, 300, "%s", text); return b; }
    for (cut = 1; cut < len; cut++) {
        snprintf(b, 300, "...%s", text + cut);
        if (host_text_width(size, b) <= maxw) return b;
    }
    snprintf(b, 300, "...");
    return b;
}

static int ext_match(const char *name, const char *exts)
{
    const char *dot = strrchr(name, '.'), *p = exts;
    char ext[16];
    size_t n;
    if (!dot) return 0;
    dot++;
    n = strlen(dot);
    if (n == 0 || n >= sizeof ext) return 0;
    for (n = 0; dot[n]; n++) ext[n] = (char)((dot[n] >= 'A' && dot[n] <= 'Z') ? dot[n] + 32 : dot[n]);
    ext[n] = 0;
    while (*p) {
        size_t l = strcspn(p, ";");
        if (l == n && strncmp(p, ext, l) == 0) return 1;
        p += l;
        if (*p == ';') p++;
    }
    return 0;
}

static void add_item(Kind kind, int id, const char *label, const char *value)
{
    Item *it;
    if (sN >= MAX_ITEMS) return;
    it = &sItems[sN++];
    it->kind = kind;
    it->id = id;
    snprintf(it->label, sizeof it->label, "%s", label);
    snprintf(it->value, sizeof it->value, "%s", value ? value : "");
}

static const char *binding_text(const int v[2], int is_key)
{
    static char buf[4][64];
    static int rot;
    char *b = buf[rot++ & 3];
    const char *n0 = is_key ? host_key_name(v[0]) : host_pad_button_label(v[0]);
    const char *n1 = is_key ? host_key_name(v[1]) : host_pad_button_label(v[1]);
    if (n0 && n1) snprintf(b, 64, "%s, %s", n0, n1);
    else if (n0 || n1) snprintf(b, 64, "%s", n0 ? n0 : n1);
    else snprintf(b, 64, "-");
    return b;
}

static const char *port_text(int port)
{
    static char b[4][16];
    static int rot;
    char *s = b[rot++ & 3];
    if (port <= 0) snprintf(s, 16, "none");
    else snprintf(s, 16, "port %d", port);
    return s;
}


/* Text cut to `maxw` pixels at the END with "..." (for lists that must keep their start). */
static const char *clip_end(const char *text, int maxw, int size)
{
    static char buf[2][300];
    static int rot;
    char *b = buf[rot++ & 1];
    size_t len = strlen(text);
    if (host_text_width(size, text) <= maxw) { snprintf(b, 300, "%s", text); return b; }
    while (len > 0) {
        len--;
        snprintf(b, 300, "%.*s...", (int)len, text);
        if (host_text_width(size, b) <= maxw) return b;
    }
    snprintf(b, 300, "...");
    return b;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Copies a file byte for byte. Returns 1 on success. */
static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[16384];
    size_t n;
    int ok = 1;
    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    return ok;
}

/* ---- gamepads and their profiles ---------------------------------------------------------------------------------------------- */
/* The profile of a connected pad; a pad that has none yet (the sync makes one at the next picture) gets the defaults of its kind in `tmp`. */
static const HostPadProfile *profile_of(const HostRawPad *p, HostPadProfile *tmp)
{
    int pi = host_profile_find(host_settings(), p);
    if (pi >= 0) return &host_settings()->profile[pi];
    host_profile_set_defaults(tmp, p->generic ? 1 : 0);
    tmp->port = 0;
    return tmp;
}

/* The connected pad that belongs to the profile (index into HostSettings.profile), or -1. */
static int pad_of_profile(const HostRawInput *raw, int profile)
{
    int i;
    if (profile < 0) return -1;
    for (i = 0; i < HOST_PADS; i++) {
        if (raw->pad[i].connected && host_profile_find(host_settings(), &raw->pad[i]) == profile) return i;
    }
    return -1;
}

/* 1 if one of the pad's inputs bound to `action` is down in the resolved state `all`. */
static int action_down(const HostPadProfile *pr, int action, const unsigned char all[PB_COUNT])
{
    int k;
    for (k = 0; k < 2; k++) if (pr->pad[action][k] >= 0 && pr->pad[action][k] < PB_COUNT && all[pr->pad[action][k]]) return 1;
    return 0;
}

static const char *kind_text(const HostRawPad *p)
{
    static char b[64];
    if (!p->generic) return "Xbox layout (XInput)";
    if (p->buttons_total) snprintf(b, sizeof b, "Generic gamepad, %d buttons", p->buttons_total);
    else snprintf(b, sizeof b, "Generic gamepad");
    return b;
}
static void browser_load(void)
{
    HostDirEntry *all = (HostDirEntry *)malloc(sizeof *all * MAX_ITEMS);
    int n, i, k = 0;
    if (!sBr.e) sBr.e = (HostDirEntry *)malloc(sizeof *sBr.e * MAX_ITEMS);
    sBr.n = 0;
    if (!all || !sBr.e) { free(all); return; }
    n = host_dir_list(sBr.dir, all, MAX_ITEMS - 8);
    if (n < 0) { message("That folder cannot be read.", COL_ERR); n = 0; }
    for (i = 0; i < n; i++) {
        if (all[i].is_dir || sBr.show_all || ext_match(all[i].name, sBr.exts)) sBr.e[k++] = all[i];
    }
    sBr.n = k;
    free(all);
}

/* "Label: path" in one line, the path shortened at its start so that the whole line fits. */
static void add_path_line(const char *label, const char *path)
{
    char line[300];
    int lw = host_text_width(SZ, label);
    snprintf(line, sizeof line, "%s%s", label, fit(path, RIGHT_X - LIST_X - 14 - lw, SZ));
    add_item(K_LABEL, I_NONE, line, NULL);
}

/* The held inputs of a pad as text ("Button 3, Hat up"). */
static void held_text(const HostRawPad *p, char *out, unsigned size)
{
    unsigned char all[PB_COUNT];
    int k;
    unsigned used = 0;
    host_pad_resolve(p, all);
    out[0] = 0;
    for (k = 0; k < PB_COUNT; k++) {
        const char *n;
        if (!all[k]) continue;
        n = host_pad_button_label(k);
        if (!n) continue;
        used += (unsigned)snprintf(out + used, used < size ? size - used : 0, "%s%s", used ? ", " : "", n);
        if (used >= size) { out[size - 1] = 0; return; }
    }
    if (!used) snprintf(out, size, "(nothing)");
}

static const char *percent_text(int v)
{
    static char b[4][16];
    static int rot;
    char *s = b[rot++ & 3];
    snprintf(s, 16, "%d %%", (v * 100) / 32767);
    return s;
}

/* The rows of the gamepad test screen: what the pad reports and what the N64 controller would get from it. */
static void build_padtest(const HostSettings *s)
{
    char tmp[300], tmp2[300];
    int pi = pad_of_profile(&sRawNow, sDev);
    const HostRawPad *p;
    HostRawInput iso;
    unsigned short bt = 0;
    signed char sx = 0, sy = 0;
    int conn = 0, a, used = 0;
    add_item(K_ACTION, I_BACK, "Back", NULL);
    if (pi < 0) { add_item(K_LABEL, I_NONE, "This gamepad is not connected.", NULL); return; }
    p = &sRawNow.pad[pi];
    add_item(K_LABEL, I_NONE, p->name, NULL);
    add_item(K_LABEL, I_NONE, kind_text(p), NULL);
    held_text(p, tmp2, sizeof tmp2);
    snprintf(tmp, sizeof tmp, "Held: %s", tmp2);
    add_item(K_LABEL, I_NONE, clip_end(tmp, RIGHT_X - LIST_X - 14, SZ), NULL);
    if (!p->generic) {
        snprintf(tmp, sizeof tmp, "%s, %s", percent_text(p->lx), percent_text(p->ly));
        add_item(K_LABEL, I_NONE, "Left stick (right, up)", tmp);
        snprintf(tmp, sizeof tmp, "%s, %s", percent_text(p->rx), percent_text(p->ry));
        add_item(K_LABEL, I_NONE, "Right stick (right, up)", tmp);
        snprintf(tmp, sizeof tmp, "%d, %d", p->lt, p->rt);
        add_item(K_LABEL, I_NONE, "Triggers left, right (0-255)", tmp);
    } else {
        snprintf(tmp, sizeof tmp, "%s, %s", percent_text(p->axis[0]), percent_text(p->axis[1]));
        add_item(K_LABEL, I_NONE, "Axes X, Y", tmp);
        snprintf(tmp, sizeof tmp, "%s, %s", percent_text(p->axis[2]), percent_text(p->axis[3]));
        add_item(K_LABEL, I_NONE, "Axes Z, R", tmp);
        snprintf(tmp, sizeof tmp, "%s, %s", percent_text(p->axis[4]), percent_text(p->axis[5]));
        add_item(K_LABEL, I_NONE, "Axes U, V", tmp);
    }
    /* what the game would receive from this gamepad alone */
    memset(&iso, 0, sizeof iso);
    iso.pad[0] = *p;
    if (s->profile[sDev].port > 0) host_input_map(s, &iso, s->profile[sDev].port, &bt, &sx, &sy, &conn);
    if (s->profile[sDev].port > 0) {
        snprintf(tmp, sizeof tmp, "port %d", s->profile[sDev].port);
        add_item(K_LABEL, I_NONE, "N64 controller", tmp);
        tmp2[0] = 0;
        for (a = 0; a < ACT_STICK_UP; a++) {
            if (!(bt & host_action_n64_bit(a))) continue;
            used += snprintf(tmp2 + used, sizeof tmp2 - (unsigned)used, "%s%s", used ? " " : "", host_action_label(a));
        }
        snprintf(tmp, sizeof tmp, "N64 buttons: %s", used ? tmp2 : "(none)");
        add_item(K_LABEL, I_NONE, clip_end(tmp, RIGHT_X - LIST_X - 14, SZ), NULL);
        snprintf(tmp, sizeof tmp, "%d, %d", sx, sy);
        add_item(K_LABEL, I_NONE, "N64 stick (right, up)", tmp);
    } else {
        add_item(K_LABEL, I_NONE, "This gamepad plays no N64 controller.", NULL);
    }
    add_item(K_LABEL, I_NONE, sRawNow.console ? "Minus: back" : sRawNow.touch ? "Menu button or Back: back" : "Menu button or Esc: back", NULL);
}

/* ---- the page of one device (a gamepad or the keyboard), laid out like the controller page of Project64's input plugin -------------------------
 * Digital pad and analog stick on the left, the C buttons, A and B on the right, the triggers, Start and the menu button in the middle, and the
 * buttons Setup (asks for every button in turn), Reset and Back below. Every input is a cell that is chosen with the arrow keys / D-pad / left stick
 * and set by pressing the new key or button. The cell of an input that is held down right now lights up, so the page doubles as a live test. */
#define LAY_SZ 1                                 /* the game's 12 pixel letters */
#define LAY_COL0 44
#define LAY_COL1 232
#define LAY_COL2 420
#define LAY_CW 176
#define LAY_CH 18
enum { LC_ACT = 0, LC_PORT = 100, LC_DEAD, LC_SETUP, LC_RESET, LC_TEST, LC_FORGET, LC_BACK };       /* cell ids; an action's cell has the action's number */
typedef struct LCell { int id, x, y, w; char label[28], value[64]; int pressed, choice, button; } LCell;
static LCell sCells[40];
static int sCellN;

static void lay_add(int id, int x, int y, int w, const char *label, const char *value, int pressed, int choice, int button)
{
    LCell *c;
    if (sCellN >= (int)(sizeof sCells / sizeof sCells[0])) return;
    c = &sCells[sCellN++];
    c->id = id; c->x = x; c->y = y; c->w = w; c->pressed = pressed; c->choice = choice; c->button = button;
    snprintf(c->label, sizeof c->label, "%s", label);
    snprintf(c->value, sizeof c->value, "%s", value ? value : "");
}

/* The first input bound to an action as text; *more is set when there is a second one. */
static const char *first_binding(const int v[2], int is_key, int *more)
{
    const char *n0 = is_key ? host_key_name(v[0]) : host_pad_button_label(v[0]);
    const char *n1 = is_key ? host_key_name(v[1]) : host_pad_button_label(v[1]);
    *more = n0 && n1;
    return n0 ? n0 : (n1 ? n1 : "-");
}

static void layout_build(void)
{
    static const struct { int action, col, y; const char *label; } kLay[] = {
        {ACT_DUP, 0, 150, "Up"}, {ACT_DDOWN, 0, 170, "Down"}, {ACT_DLEFT, 0, 190, "Left"}, {ACT_DRIGHT, 0, 210, "Right"},
        {ACT_STICK_UP, 0, 252, "Up"}, {ACT_STICK_DOWN, 0, 272, "Down"}, {ACT_STICK_LEFT, 0, 292, "Left"}, {ACT_STICK_RIGHT, 0, 312, "Right"},
        {ACT_L, 1, 150, "L trigger"}, {ACT_R, 1, 170, "R trigger"}, {ACT_Z, 1, 190, "Z trigger"}, {ACT_START, 1, 230, "Start"}, {ACT_MENU, 1, 250, "Menu button"},
        {ACT_CUP, 2, 150, "Up"}, {ACT_CDOWN, 2, 170, "Down"}, {ACT_CLEFT, 2, 190, "Left"}, {ACT_CRIGHT, 2, 210, "Right"},
        {ACT_A, 2, 252, "A"}, {ACT_B, 2, 272, "B"},
    };
    HostSettings *s = host_settings();
    int kb = sDev == DEV_KEYBOARD, pi = -1, k, more;
    const HostPadProfile *pr = NULL;
    unsigned char all[PB_COUNT];
    char tmp[80];
    sCellN = 0;
    memset(all, 0, sizeof all);
    if (!kb) {
        if (sDev < 0 || !s->profile[sDev].name[0]) { lay_add(LC_BACK, 496, 344, 100, "Back", NULL, 0, 0, 1); return; }
        pr = &s->profile[sDev];
        pi = pad_of_profile(&sRawNow, sDev);
        if (pi >= 0) host_pad_resolve(&sRawNow.pad[pi], all);
    }
    lay_add(LC_PORT, LAY_COL0, 84, 552, "Plays as N64 controller", port_text(kb ? s->keyboard_port : pr->port), 0, 1, 0);
    if (!kb) { snprintf(tmp, sizeof tmp, "%d %%", pr->deadzone); lay_add(LC_DEAD, LAY_COL0, 106, 552, "Left stick dead zone", tmp, 0, 1, 0); }
    for (k = 0; k < (int)(sizeof kLay / sizeof kLay[0]); k++) {
        int a = kLay[k].action, held = 0, j;
        char val[64];
        if (kb && a >= ACT_KEY_COUNT) continue;                              /* the keyboard has no menu button (F1 and Esc open the menu) */
        if (!kb && a >= ACT_STICK_UP && a <= ACT_STICK_RIGHT) continue;      /* a gamepad's left stick drives the analog stick itself: no cells, a note instead */
        if (kb) { for (j = 0; j < 2; j++) if (s->key[a][j] > 0 && s->key[a][j] < HK_COUNT && sRawNow.key[s->key[a][j]]) held = 1; }
        else held = action_down(pr, a, all);
        snprintf(val, sizeof val, "%s%s", first_binding(kb ? s->key[a] : pr->pad[a], kb, &more), more ? " +" : "");
        lay_add(LC_ACT + a, kLay[k].col == 0 ? LAY_COL0 : kLay[k].col == 1 ? LAY_COL1 : LAY_COL2, kLay[k].y, LAY_CW, kLay[k].label, val, held, 0, 0);
    }
    lay_add(LC_SETUP, 44, 344, 100, "Setup", NULL, 0, 0, 1);
    lay_add(LC_RESET, 150, 344, 100, "Reset", NULL, 0, 0, 1);
    if (!kb && pi >= 0) lay_add(LC_TEST, 256, 344, 100, "Test", NULL, 0, 0, 1);
    if (!kb && pi < 0) lay_add(LC_FORGET, 256, 344, 140, "Forget", NULL, 0, 0, 1);
    lay_add(LC_BACK, 496, 344, 100, "Back", NULL, 0, 0, 1);
}

/* The cell that is nearest in the direction (dx, dy), or -1. A cell straight ahead beats one that is far to the side. */
static int layout_neighbor(int cur, int dx, int dy)
{
    /* The two full-width cells at the top (port, dead zone) have no column of their own: moving down from one continues in the column
     * the cursor came from (sLayX), and moving up into one is never "to the side". */
    static long sLayX = LAY_COL0 + LAY_CW / 2;
    int j, best = -1;
    long best_score = 0x7fffffff, cx, cy = sCells[cur].y;
    if (sCells[cur].w < 300) sLayX = sCells[cur].x + sCells[cur].w / 2;
    cx = sLayX;
    for (j = 0; j < sCellN; j++) {
        long ex, ey, along, across, score;
        if (j == cur) continue;
        ex = (sCells[j].w >= 300 && dy) ? 0 : sCells[j].x + sCells[j].w / 2 - cx;
        ey = sCells[j].y - cy;
        along = dx ? ex * dx : ey * dy;
        across = dx ? ey : ex;
        if (across < 0) across = -across;
        if (along <= 0) continue;
        score = along + 3 * across;
        if (score < best_score) { best_score = score; best = j; }
    }
    return best;
}

static void build_items(void)
{
    HostSettings *s = host_settings();
    char tmp[300], tmp2[300];
    int i;
    sN = 0;
    switch (sStack[sDepth]) {
    case SC_MAIN: {
        int inserted = 0, pads = 0;
        for (i = 0; i < 4; i++) inserted += host_gbpak_present(i);
        for (i = 0; i < HOST_PADS; i++) pads += sRawNow.pad[i].connected;
        add_item(K_ACTION, I_RESUME, "Resume game", NULL);
        snprintf(tmp, sizeof tmp, "%d inserted", inserted);
        add_item(K_ACTION, I_PAKS, "Game Boy cartridges (Game Paks)", tmp);
        snprintf(tmp, sizeof tmp, "%d gamepad%s", pads, pads == 1 ? "" : "s");
        add_item(K_ACTION, I_CTRL, "Controllers", tmp);
        add_item(K_ACTION, I_GAMEROM, "Stadium ROM and save file", NULL);
        add_item(K_ACTION, I_OPTIONS, "Options", NULL);
        add_item(K_ACTION, I_ABOUT, "About and file locations", NULL);
        add_item(K_ACTION, I_RESTART, "Restart game", sRestartNeeded ? "needed" : NULL);
        add_item(K_ACTION, I_QUIT, "Quit game", NULL);
        add_item(K_ACTION, I_EXIT, "Exit menu", "back to the game");
        break; }
    case SC_PAKS:
        if (sSetup) {
            add_item(K_LABEL, I_NONE, "No Game Boy cartridge was found.", NULL);
            add_item(K_LABEL, I_NONE, "Choose a Game Boy ROM for the Transfer Pak of each", NULL);
            add_item(K_LABEL, I_NONE, "controller that should have one (GB Tower, Pokemon Lab).", NULL);
            add_item(K_LABEL, I_NONE, "", NULL);
        }
        for (i = 0; i < 4; i++) {
            snprintf(tmp, sizeof tmp, "Controller %d Transfer Pak", i + 1);
            if (host_gbpak_present(i)) host_gbpak_title(i, tmp2, sizeof tmp2); else snprintf(tmp2, sizeof tmp2, "(empty)");
            add_item(K_ACTION, I_PORT0 + i, tmp, tmp2);
        }
        add_item(K_ACTION, I_BACK, sSetup ? "Done (this page is shown only once)" : "Back", NULL);
        if (sRestartNeeded) {
            add_item(K_LABEL, I_NONE, "", NULL);
            add_item(K_LABEL, I_NONE, "Restart the game to use the change.", NULL);
        }
        break;
    case SC_PAK: {
        char def[1100];
#ifdef HOST_SHELL_PICKER
        host_settings_fix_gb_saves();
#endif
        snprintf(tmp, sizeof tmp, "Transfer Pak of controller %d", sPak + 1);
        add_item(K_LABEL, I_NONE, tmp, NULL);
        add_item(K_ACTION, I_PAK_ROM, "Game Boy ROM", s->gb_rom[sPak][0] ? host_path_basename(s->gb_rom[sPak]) : "(none)");
        if (s->gb_save[sPak][0]) snprintf(tmp2, sizeof tmp2, "%s", host_path_basename(s->gb_save[sPak]));
        else if (s->gb_rom[sPak][0]) { host_path_with_ext(s->gb_rom[sPak], ".sav", def, sizeof def); snprintf(tmp2, sizeof tmp2, "%s (automatic)", host_path_basename(def)); }
        else snprintf(tmp2, sizeof tmp2, "(automatic)");
        add_item(K_ACTION, I_PAK_SAVE, "Save file", tmp2);
        if (s->gb_save[sPak][0]) add_item(K_ACTION, I_PAK_DEFAULT_SAVE, "Use the automatic save file name", NULL);
        if (host_gbpak_present(sPak)) add_item(K_ACTION, I_PAK_REMOVE, "Take the cartridge out", NULL);
        add_item(K_ACTION, I_BACK, "Back", NULL);
        if (!sBoot) {
            add_item(K_LABEL, I_NONE, "", NULL);
            add_item(K_LABEL, I_NONE, "The game looks for cartridges only when it starts, so a", NULL);
            add_item(K_LABEL, I_NONE, "change is seen after a restart.", NULL);
            add_item(K_ACTION, I_RESTART, "Restart game", sRestartNeeded ? "needed" : NULL);
        }
        break; }
    case SC_CTRL: {
        int found = 0;
        if (sRawNow.keyboards) snprintf(tmp, sizeof tmp, "Keyboards found: %d", sRawNow.keyboards);
        else snprintf(tmp, sizeof tmp, "Keyboard");
        add_item(K_LABEL, I_NONE, tmp, NULL);
        add_item(K_ACTION, I_DEVKB, "Keyboard", s->keyboard_port ? port_text(s->keyboard_port) : "none");
        for (i = 0; i < HOST_PADS; i++) found += sRawNow.pad[i].connected;
        snprintf(tmp, sizeof tmp, "Gamepads found: %d", found);
        add_item(K_LABEL, I_NONE, tmp, NULL);
        for (i = 0; i < HOST_PADS; i++) {                                    /* the connected ones first ... */
            int pi;
            if (!sRawNow.pad[i].connected) continue;
            pi = host_profile_find(s, &sRawNow.pad[i]);
            if (pi < 0) continue;                                            /* no profile yet: made at the next picture */
            add_item(K_ACTION, I_DEV0 + pi, sRawNow.pad[i].name, port_text(s->profile[pi].port));
        }
        for (i = 0; i < HS_PROFILES; i++) {                                  /* ... then the ones that are remembered but not plugged in */
            if (!s->profile[i].name[0] || pad_of_profile(&sRawNow, i) >= 0) continue;
            snprintf(tmp, sizeof tmp, "%s, not connected", port_text(s->profile[i].port));
            add_item(K_ACTION, I_DEV0 + i, s->profile[i].name, tmp);
        }
        if (!found) add_item(K_LABEL, I_NONE, "None found. Plug one in or pair it (Bluetooth).", NULL);
        add_item(K_LABEL, I_NONE, "Choose a device to set its buttons.", NULL);
        add_item(K_ACTION, I_BACK, "Back", NULL);
        break; }
    case SC_DEVICE:
        layout_build();                                                      /* this screen is not a list: its cells are drawn by draw_layout */
        break;
    case SC_PADTEST:
        build_padtest(s);
        break;
    case SC_GAMEROM:
        add_path_line("Save file in use: ", host_save_path());
        add_item(K_ACTION, I_ST_ROM, "Stadium ROM", s->rom[0] ? host_path_basename(s->rom) : "(chosen at start)");
        add_item(K_ACTION, I_ST_SAVE, "Load a different save file...", s->save[0] ? host_path_basename(s->save) : "(default)");
        add_item(K_ACTION, I_ST_NEW, "Start a new empty save file", NULL);
        add_item(K_ACTION, I_ST_BACKUP, "Back up the current save file", NULL);
        if (s->save[0]) add_item(K_ACTION, I_ST_DEFAULT, "Use the default save file again", NULL);
        add_item(K_ACTION, I_BACK, "Back", NULL);
        add_item(K_LABEL, I_NONE, "", NULL);
        add_item(K_LABEL, I_NONE, "A different ROM or save file is used after a restart.", NULL);
        add_item(K_ACTION, I_RESTART, "Restart game", sRestartNeeded ? "needed" : NULL);
        break;
    case SC_OPTIONS:
        if (!getenv("KYUJO_FIXED_WINDOW")) add_item(K_CHOICE, I_FULL, "Full screen", s->fullscreen ? "On" : "Off");     /* a console's picture always fills the screen */
        snprintf(tmp, sizeof tmp, "%d %%", s->volume);
        add_item(K_CHOICE, I_VOLUME, "Sound volume", tmp);
        add_item(K_CHOICE, I_WIDE, "Widescreen (stretches the picture)", s->widescreen ? "On" : "Off");
        add_item(K_CHOICE, I_SCALE, "Picture scaling", s->scale_style == 1 ? "Sharp" : s->scale_style == 2 ? "Firm" : "Smooth");
        add_item(K_CHOICE, I_CHEAT, "Cheat: knock out opponent", s->cheats ? "On" : "Off");
        host_cheat_help(s, tmp2, sizeof tmp2);
        add_item(K_LABEL, I_NONE, clip_end(tmp2, RIGHT_X - LIST_X - 14, SZ), NULL);
        add_item(K_CHOICE, I_CHEAT_HOF, "Cheat: Hall of Fame complete", s->cheat_hof ? "On" : "Off");
        add_item(K_CHOICE, I_CHEAT_CPUVS, "Cheat: Kids Club CPU vs CPU", s->cheat_cpuvs ? "On" : "Off");
        add_item(K_LABEL, I_NONE, "Hall: made-up records, not saved. Kids Club: Ready! with 4 COM.", NULL);
        if (sRawNow.touch) add_item(K_LABEL, I_NONE, "Menu button or Back: menu", NULL);
        else if (!sRawNow.console) add_item(K_LABEL, I_NONE, "F11: full screen     F1 or Esc: menu", NULL);
        else add_item(K_LABEL, I_NONE, "Minus: menu", NULL);
        add_item(K_ACTION, I_BACK, "Back", NULL);
        break;
    case SC_ABOUT:
        add_item(K_LABEL, I_NONE, "Pokemon Stadium (USA) - native port", NULL);
        add_item(K_LABEL, I_NONE, "", NULL);
        add_path_line("Folder: ", host_app_dir());
        add_path_line("Settings: ", host_settings_path());
        add_path_line("Save file: ", host_save_path());
        add_path_line("ROM: ", s->rom[0] ? s->rom : "(chosen at start)");
        add_item(K_LABEL, I_NONE, "Log: kyujo-log.txt in the folder", NULL);
        add_item(K_LABEL, I_NONE, "", NULL);
        add_item(K_LABEL, I_NONE, sRawNow.console ? "Minus: menu" : sRawNow.touch ? "Menu button or Back: menu" : "F1 or Esc: menu     F11: full screen", NULL);
        add_item(K_ACTION, I_BACK, "Back", NULL);
        break;
    case SC_BROWSER: {
        int k;
        add_item(K_ACTION, I_BR_PARENT, ".. (parent folder)", NULL);
        if (sBr.can_create) { snprintf(tmp, sizeof tmp, "New file here: %s", sBr.new_name); add_item(K_ACTION, I_BR_NEW, tmp, NULL); }
        add_item(K_CHOICE, I_BR_ALL, "Show all files", sBr.show_all ? "On" : "Off");
        for (k = 0; k < sBr.n; k++) {
            if (sBr.e[k].is_dir) { snprintf(tmp, sizeof tmp, "[%s]", sBr.e[k].name); add_item(K_ACTION, I_ENTRY0 + k, tmp, NULL); }
            else add_item(K_ACTION, I_ENTRY0 + k, sBr.e[k].name, NULL);
        }
        break; }
    case SC_QUIT:
        add_item(K_ACTION, I_BACK, "No, go back", NULL);
        add_item(K_ACTION, I_QUIT_YES, sRawNow.console ? "Yes, quit to the launcher" : "Yes, quit the game", NULL);
        if (sRawNow.console) {
            add_item(K_ACTION, I_QUIT_CLOSE, "Yes, quit and close the software", NULL);
            add_item(K_LABEL, I_NONE, "", NULL);
            add_item(K_LABEL, I_NONE, "Closing the software ends the whole program, so the next", NULL);
            add_item(K_LABEL, I_NONE, "start is a clean one (a start right after quitting to the", NULL);
            add_item(K_LABEL, I_NONE, "launcher has crashed once).", NULL);
        }
        break;
    case SC_RESTART:
        add_item(K_ACTION, I_BACK, "No, go back", NULL);
        add_item(K_ACTION, I_RESTART_YES, "Yes, restart the game", NULL);
        add_item(K_LABEL, I_NONE, "", NULL);
        add_item(K_LABEL, I_NONE, "Anything in the game that is not saved is lost.", NULL);
        break;
    case SC_ASK:
        add_item(K_ACTION, I_RESTART_YES, "Restart now", NULL);
        add_item(K_ACTION, I_ASK_LATER, "Later", NULL);
        add_item(K_LABEL, I_NONE, "", NULL);
        add_item(K_LABEL, I_NONE, "The game reads the Game Boy cartridges, the Stadium ROM", NULL);
        add_item(K_LABEL, I_NONE, "and the save file only when it starts, so the change you", NULL);
        add_item(K_LABEL, I_NONE, "made is used after a restart. Anything in the game that", NULL);
        add_item(K_LABEL, I_NONE, "is not saved is lost when it restarts.", NULL);
        break;
    }
}

static const char *title_text(void)
{
    static char t[160];
    switch (sStack[sDepth]) {
    case SC_MAIN: return "Pokemon Stadium";
    case SC_PAKS: return sSetup ? "Game Pak Setup" : "Game Boy cartridges";
    case SC_PAK: snprintf(t, sizeof t, "Game Pak: controller %d", sPak + 1); return t;
    case SC_CTRL: return "Controllers";
    case SC_DEVICE:
        if (sDev == DEV_KEYBOARD) return "Keyboard";
        if (sDev >= 0 && host_settings()->profile[sDev].name[0]) { snprintf(t, sizeof t, "%s", fit(host_settings()->profile[sDev].name, RIGHT_X - LIST_X, SZ_TITLE)); return t; }
        return "Gamepad";
    case SC_PADTEST: return "Gamepad test";
    case SC_GAMEROM: return "Stadium ROM and save";
    case SC_OPTIONS: return "Options";
    case SC_ABOUT: return "About";
    case SC_BROWSER:
        switch (sBr.purpose) {
        case BP_GAME_ROM: return "Choose the Pokemon Stadium ROM";
        case BP_GAME_SAVE: return "Choose the save file";
        case BP_GB_ROM: return "Choose the Game Boy ROM";
        default: return "Choose the Game Boy save file";
        }
    case SC_QUIT: return "Quit the game?";
    case SC_RESTART: return "Restart the game?";
    case SC_ASK: return "Restart the game?";
    }
    return "";
}

static void close_menu(void)
{
    if (sSetup && !host_settings()->pak_setup_done) { host_settings()->pak_setup_done = 1; changed(); }       /* shown once: it does not come back at the next start */
    sSetup = 0;
    sOpen = 0;
}
static void push(Screen s);

/* Closes the menu, or asks about the restart first when a change is waiting for one that the player has not been asked about. */
static void request_close(void)
{
    if (sRestartNeeded && !sAsked && sStack[sDepth] != SC_ASK) {
        sAsked = 1;
        sAskThenClose = 1;
        sMessage[0] = 0;
        push(SC_ASK);
        return;
    }
    close_menu();
}

static void push(Screen s)
{
    if (sDepth >= 7) return;
    sDepth++;
    sStack[sDepth] = s;
    sCursor[sDepth] = 0;
    sScroll[sDepth] = 0;
    if (s == SC_BROWSER) browser_load();
}

static void pop(void)
{
    if (sStack[sDepth] == SC_ASK && sAskThenClose) { sAskThenClose = 0; close_menu(); return; }   /* "Later" (or Back) on the question that came from closing */
    if (sDepth > 0) sDepth--;
    else request_close();
}

/* The screens where a change that needs a restart is made: the question comes when the player leaves them. */
static int in_restart_area(Screen s)
{
    return s == SC_PAKS || s == SC_PAK || s == SC_GAMEROM || s == SC_BROWSER || s == SC_ASK || s == SC_RESTART;
}

static void maybe_ask(void)
{
    if (sRestartNeeded && !sAsked && !in_restart_area(sStack[sDepth])) {
        sAsked = 1;
        sAskThenClose = 0;
        sMessage[0] = 0;
        push(SC_ASK);
    }
}

/* Opens one of the screens from the root (the Windows menu bar asks for them). */
static void goto_screen(int target)
{
    sDepth = 0;
    sStack[0] = SC_MAIN;
    sCursor[0] = 0;
    sScroll[0] = 0;
    sAskThenClose = 0;
    sSetup = target == HM_SETUP;
    if (sSetup) { sStack[0] = SC_PAKS; return; }
    switch (target) {
    case HM_PAKS: push(SC_PAKS); break;
    case HM_CONTROLLERS: push(SC_CTRL); break;
    case HM_GAMEROM: push(SC_GAMEROM); break;
    case HM_OPTIONS: push(SC_OPTIONS); break;
    case HM_ABOUT: push(SC_ABOUT); break;
    case HM_RESTART: push(SC_RESTART); break;
    default:
        if (target >= HM_PAK1 && target < HM_PAK1 + 4) { sPak = target - HM_PAK1; push(SC_PAKS); push(SC_PAK); }
        break;
    }
}
/* ---- drawing ---------------------------------------------------------------------------------------------------------------------- */
static unsigned char *sCanvasRgb;
static unsigned char *sBase;                     /* the background at 640 x 480 */
static int sBaseTried;
static int sDumpCount;

/* The background: the Game Pak Check page's pattern (host_gameui.c), or a plain colour if it cannot be read from the ROM. */
static void make_base(void)
{
    const unsigned char *bg;
    int i;
    sBaseTried = 1;
    sBase = (unsigned char *)malloc((size_t)HUI_W * HUI_H * 3);
    if (!sBase) return;
    bg = host_gameui_background();
    if (bg) { memcpy(sBase, bg, (size_t)HUI_W * HUI_H * 3); return; }
    for (i = 0; i < HUI_W * HUI_H; i++) { sBase[i * 3] = 120; sBase[i * 3 + 1] = 100; sBase[i * 3 + 2] = 40; }
}

static void draw_triangle(HostCanvas *cv, int x, int y, unsigned rgb)
{
    int i, j;
    for (i = 0; i < 6; i++) for (j = i - 1; j <= 11 - i - 1 && j < 6; j++) if (j >= 0 && j <= 11 - 2 * i) host_canvas_blend(cv, x + i, y + j, rgb, 255);
}

static int visible_rows(void) { return (MSG_Y - 6 - LIST_Y) / PITCH; }

/* The device page: group headings, the cells (a lit cell = that input is held down now), and a line about the chosen cell. */
static void draw_layout(HostCanvas *cv)
{
    static const struct { int x, y; const char *text; } kHead[] = {
        {LAY_COL0, 134, "Digital pad"}, {LAY_COL0, 236, "Analog stick"}, {LAY_COL1, 134, "Triggers"}, {LAY_COL1, 214, "Start and menu"},
        {LAY_COL2, 134, "C buttons"}, {LAY_COL2, 236, "A and B"},
    };
    int i, cur = sCursor[sDepth], kb = sDev == DEV_KEYBOARD;
    char info[300];
    if (cur >= sCellN) cur = 0;
    for (i = 0; i < (int)(sizeof kHead / sizeof kHead[0]); i++) {
        host_text_draw(cv, LAY_SZ, kHead[i].x, kHead[i].y, (kb && kHead[i].y == 214) ? "Start" : kHead[i].text, COL_DIM, 1);
        host_canvas_fill(cv, kHead[i].x, kHead[i].y + 14, LAY_CW, 1, 0xFFFFFFu, 70);
    }
    for (i = 0; i < sCellN; i++) {
        const LCell *c = &sCells[i];
        int sel = i == cur;
        unsigned col = sel ? COL_SEL : COL_TEXT;
        host_canvas_fill(cv, c->x, c->y - 2, c->w, LAY_CH, 0xFFFFFFu, c->button ? (sel ? 80 : 40) : (sel ? 70 : 22));
        if (c->pressed) host_canvas_fill(cv, c->x, c->y - 2, c->w, LAY_CH, 0x40E060u, 150);
        if (sel) { host_canvas_fill(cv, c->x, c->y - 2, c->w, 1, COL_SEL, 255); host_canvas_fill(cv, c->x, c->y + LAY_CH - 3, c->w, 1, COL_SEL, 255); }
        if (c->button) {
            host_text_draw(cv, LAY_SZ, c->x + (c->w - host_text_width(LAY_SZ, c->label)) / 2, c->y + 1, c->label, col, 1);
        } else if (c->choice) {
            char v[100];
            host_text_draw(cv, LAY_SZ, c->x + 6, c->y + 1, c->label, col, 1);
            snprintf(v, sizeof v, sel ? "< %s >" : "%s", c->value);
            host_text_draw(cv, LAY_SZ, c->x + c->w - 6 - host_text_width(LAY_SZ, v), c->y + 1, v, col, 1);
        } else {
            host_text_draw(cv, LAY_SZ, c->x + 6, c->y + 1, c->label, sel ? COL_SEL : COL_DIM, 1);
            host_text_draw(cv, LAY_SZ, c->x + c->w - 6 - host_text_width(LAY_SZ, fit(c->value, c->w - 16 - host_text_width(LAY_SZ, c->label), LAY_SZ)),
                           c->y + 1, fit(c->value, c->w - 16 - host_text_width(LAY_SZ, c->label), LAY_SZ), col, 1);
        }
    }
    /* what the chosen cell is, in full */
    info[0] = 0;
    if (sCellN > 0) {
        HostSettings *s = host_settings();
        int id = sCells[cur].id;
        if (id >= LC_ACT && id < LC_ACT + ACT_COUNT) {
            const int *v = kb ? s->key[id - LC_ACT] : (sDev >= 0 ? s->profile[sDev].pad[id - LC_ACT] : NULL);
            if (v) snprintf(info, sizeof info, "%s: %s", host_action_label(id - LC_ACT), binding_text(v, kb));
        } else if (id == LC_SETUP) snprintf(info, sizeof info, "Setup: press the button for each N64 button in turn.");
        else if (id == LC_RESET) snprintf(info, sizeof info, "Reset: the default buttons of this device.");
        else if (id == LC_TEST) snprintf(info, sizeof info, "Test: shows every input of the gamepad and what the N64 controller receives.");
        else if (id == LC_FORGET) snprintf(info, sizeof info, "Forget: removes this gamepad's settings (it is not connected).");
    }
    host_text_draw(cv, LAY_SZ, LAY_COL0, 372, clip_end(info, RIGHT_X - LAY_COL0, LAY_SZ), COL_TEXT, 1);
    if (!kb) {
        host_text_draw(cv, LAY_SZ, LAY_COL0 + 6, 254, "The left stick moves the", COL_DIM, 1);
        host_text_draw(cv, LAY_SZ, LAY_COL0 + 6, 272, "N64 analog stick.", COL_DIM, 1);
    }
}

static void draw_menu(HostCanvas *cv, int capture_active, const char *capture_prompt, const char *capture_sub, const char *capture_help)
{
    int i, rows = visible_rows(), cur = sCursor[sDepth], *scroll = &sScroll[sDepth];
    const char *help;
    memcpy(cv->rgb, sBase, (size_t)HUI_W * HUI_H * 3);
    host_canvas_fill(cv, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 0x04081Cu, 150);
    host_canvas_fill(cv, PANEL_X, PANEL_Y, PANEL_W, 2, 0xFFFFFFu, 110);
    host_canvas_fill(cv, PANEL_X, PANEL_Y + PANEL_H - 2, PANEL_W, 2, 0xFFFFFFu, 110);
    host_canvas_fill(cv, PANEL_X, PANEL_Y, 2, PANEL_H, 0xFFFFFFu, 110);
    host_canvas_fill(cv, PANEL_X + PANEL_W - 2, PANEL_Y, 2, PANEL_H, 0xFFFFFFu, 110);
    if (sStack[sDepth] == SC_MAIN || sSetup)                                     /* the main screen's title (and the first-start page's) is centred in the panel, the others sit at the left */
        host_text_draw(cv, SZ_TITLE, LIST_X + (RIGHT_X - LIST_X - host_text_width(SZ_TITLE, title_text())) / 2, 36, title_text(), COL_TEXT, 1);
    else host_text_draw(cv, SZ_TITLE, LIST_X, 36, title_text(), COL_TEXT, 1);
    if (sStack[sDepth] == SC_BROWSER) host_text_draw(cv, SZ, LIST_X, 62, fit(sBr.dir[0] ? sBr.dir : "(drives)", RIGHT_X - LIST_X, SZ), COL_DIM, 1);

    if (cur < *scroll) *scroll = cur;
    if (cur >= *scroll + rows) *scroll = cur - rows + 1;
    if (*scroll < 0) *scroll = 0;
    if (sStack[sDepth] == SC_DEVICE) { draw_layout(cv); rows = 0; }
    for (i = 0; i < rows && *scroll + i < sN; i++) {
        const Item *it = &sItems[*scroll + i];
        int y = LIST_Y + i * PITCH, sel = (*scroll + i) == cur && it->kind != K_LABEL;
        unsigned col = it->kind == K_LABEL ? COL_DIM : (sel ? COL_SEL : COL_TEXT);
        int vw = it->value[0] ? host_text_width(SZ, it->value) : 0;
        int label_w = it->value[0] ? VALUE_X - LIST_X - 10 : RIGHT_X - LIST_X - 14;
        if (sel) { host_canvas_fill(cv, LIST_X - 8, y - 2, RIGHT_X - LIST_X + 16, PITCH, 0xFFFFFFu, 46); draw_triangle(cv, LIST_X - 4, y + 3, COL_SEL); }
        host_text_draw(cv, SZ, LIST_X + 10, y, fit(it->label, label_w, SZ), col, 1);
        if (it->value[0]) {
            const char *v = fit(it->value, RIGHT_X - VALUE_X - 8, SZ);
            (void)vw;
            if (it->kind == K_CHOICE && sel) {
                char b[200];
                snprintf(b, sizeof b, "< %s >", v);
                host_text_draw(cv, SZ, RIGHT_X - 8 - host_text_width(SZ, b), y, b, col, 1);
            } else {
                host_text_draw(cv, SZ, RIGHT_X - 8 - host_text_width(SZ, v), y, v, sel ? COL_SEL : COL_DIM, 1);
            }
        }
    }
    if (sN > rows && rows > 0) {                                         /* scroll bar */
        int th = rows * PITCH * rows / sN, ty = LIST_Y + (rows * PITCH - th) * *scroll / (sN - rows);
        host_canvas_fill(cv, PANEL_X + PANEL_W - 12, LIST_Y, 4, rows * PITCH, 0xFFFFFFu, 40);
        host_canvas_fill(cv, PANEL_X + PANEL_W - 12, ty, 4, th, 0xFFFFFFu, 190);
    }
    if (sMessage[0] && host_wall_ns() < sMessageUntil) host_text_draw(cv, SZ, LIST_X, MSG_Y, fit(sMessage, RIGHT_X - LIST_X, SZ), sMessageColor, 1);
    if (capture_active) {
        host_canvas_fill(cv, 80, 170, 480, 120, 0x000000u, 225);
        host_canvas_fill(cv, 80, 170, 480, 2, 0xFFFFFFu, 200);
        host_canvas_fill(cv, 80, 288, 480, 2, 0xFFFFFFu, 200);
        host_text_draw(cv, SZ, 320 - host_text_width(SZ, capture_prompt) / 2, 188, capture_prompt, COL_SEL, 1);
        if (capture_sub && capture_sub[0]) host_text_draw(cv, SZ, 320 - host_text_width(SZ, capture_sub) / 2, 216, capture_sub, COL_TEXT, 1);
        help = capture_help;
        host_text_draw(cv, SZ, 320 - host_text_width(SZ, help) / 2, 252, help, COL_DIM, 1);
    } else {
        if (sRawNow.console)
            help = sStack[sDepth] == SC_BROWSER ? "A: open   B: back   Minus: close menu"
                 : sStack[sDepth] == SC_PADTEST ? "Press buttons on the gamepad.   Minus: back"
                 : sStack[sDepth] == SC_DEVICE ? "D-pad / left stick: move   A: set   B: back"
                                                : "Up/Down: select   A: choose   Left/Right: change   B: back";
        else
        help = sStack[sDepth] == SC_BROWSER ? "Enter / A: open   Esc / B: back   F1: close menu"
             : sStack[sDepth] == SC_PADTEST ? "Press buttons on the gamepad.   Menu button or Esc: back"
             : sStack[sDepth] == SC_DEVICE ? "Arrows / D-pad / left stick: move   Enter / A: set   Esc / B: back"
                                            : "Up/Down: select   Enter / A: choose   Left/Right: change   Esc / B: back";
        host_text_draw(cv, SZ, LIST_X, HELP_Y, fit(help, RIGHT_X - LIST_X, SZ), COL_DIM, 1);
    }
}

/* The menu picture is 640 x 480. On a wider window it is shown on a canvas that has the window's shape: the game's wallpaper continues to the left and right of the
 * menu (the pattern keeps its phase), so the picture fills the window and the text is not stretched. */
static void present_wide(const unsigned char *rgb)
{
    static unsigned char *wide;
    static int wide_w;
    const unsigned char *bg = host_gameui_background();
    int w = HUI_W, ox, x, y;
    if (sRawNow.view_w > 0 && sRawNow.view_h > 0) w = (int)(((long)HUI_H * sRawNow.view_w + sRawNow.view_h / 2) / sRawNow.view_h);
    if (w > 1280) w = 1280;
    if (w <= HUI_W || !bg) { host_window_present(rgb, HUI_W, HUI_H); return; }
    if (w != wide_w || !wide) {
        free(wide);
        wide = (unsigned char *)malloc((size_t)w * HUI_H * 3);
        wide_w = wide ? w : 0;
        if (!wide) { host_window_present(rgb, HUI_W, HUI_H); return; }
    }
    ox = (w - HUI_W) / 2;
    for (y = 0; y < HUI_H; y++) {
        unsigned char *row = wide + (size_t)y * w * 3;
        for (x = 0; x < ox; x++) memcpy(row + (size_t)x * 3, bg + ((size_t)y * HUI_W + (size_t)(((x - ox) % 128 + 128) % 128)) * 3, 3);
        memcpy(row + (size_t)ox * 3, rgb + (size_t)y * HUI_W * 3, (size_t)HUI_W * 3);
        for (x = ox + HUI_W; x < w; x++) memcpy(row + (size_t)x * 3, bg + ((size_t)y * HUI_W + (size_t)(((x - ox) % 128 + 128) % 128)) * 3, 3);
    }
    host_window_present(wide, (unsigned)w, HUI_H);
}

static void present(const unsigned char *rgb)
{
    const char *dir = getenv("KYUJO_MENU_DUMP");
    present_wide(rgb);
    if (dir && dir[0] && sDumpCount < 400) {                             /* development: every distinct menu picture as a PNG */
        char path[1100];
        snprintf(path, sizeof path, "%s/menu_%04d.png", dir, sDumpCount++);
        if (png_write_rgb(path, rgb, HUI_W, HUI_H) != 0) fprintf(stderr, "kyujo: cannot write %s\n", path);
    }
}

/* ---- input -------------------------------------------------------------------------------------------------------------------------- */
enum { NV_UP = 1, NV_DOWN = 2, NV_LEFT = 4, NV_RIGHT = 8, NV_OK = 16, NV_BACK = 32, NV_PGUP = 64, NV_PGDN = 128, NV_CLOSE = 256 };
#define NAV_STICK 16000                          /* a generic pad's X / Y axis past this moves the cursor */

/* How one gamepad moves through the menu. An Xbox-layout pad: D-pad or left stick, A / Start choose, B goes back, bumpers page. A generic pad: hat
 * or X / Y axes, and the buttons it has bound to A / Start (choose), B (back), L / R (page). Every pad: its menu button closes the menu. */
static unsigned pad_nav(const HostRawPad *p)
{
    HostPadProfile tmp;
    const HostPadProfile *pr = profile_of(p, &tmp);
    unsigned char d[PB_COUNT];
    unsigned v = 0;
    host_pad_resolve(p, d);
    if (!p->generic) {
        if (d[PB_DUP] || d[PB_LS_UP]) v |= NV_UP;
        if (d[PB_DDOWN] || d[PB_LS_DOWN]) v |= NV_DOWN;
        if (d[PB_DLEFT] || d[PB_LS_LEFT]) v |= NV_LEFT;
        if (d[PB_DRIGHT] || d[PB_LS_RIGHT]) v |= NV_RIGHT;
        if (d[PB_A] || d[PB_START]) v |= NV_OK;
        if (d[PB_B]) v |= NV_BACK;
        if (d[PB_LB]) v |= NV_PGUP;
        if (d[PB_RB]) v |= NV_PGDN;
    } else {
        if (d[PB_G_HAT_UP] || p->ly > NAV_STICK) v |= NV_UP;
        if (d[PB_G_HAT_DOWN] || p->ly < -NAV_STICK) v |= NV_DOWN;
        if (d[PB_G_HAT_LEFT] || p->lx < -NAV_STICK) v |= NV_LEFT;
        if (d[PB_G_HAT_RIGHT] || p->lx > NAV_STICK) v |= NV_RIGHT;
        if (action_down(pr, ACT_A, d) || action_down(pr, ACT_START, d)) v |= NV_OK;
        if (action_down(pr, ACT_B, d)) v |= NV_BACK;
        if (action_down(pr, ACT_L, d)) v |= NV_PGUP;
        if (action_down(pr, ACT_R, d)) v |= NV_PGDN;
    }
    if (action_down(pr, ACT_MENU, d)) { v &= ~(unsigned)(NV_OK | NV_BACK); v |= NV_CLOSE; }
    return v;
}

static unsigned nav_bits(const HostRawInput *r)
{
    unsigned v = 0;
    int i;
    if (r->key[HK_UP]) v |= NV_UP;
    if (r->key[HK_DOWN]) v |= NV_DOWN;
    if (r->key[HK_LEFT]) v |= NV_LEFT;
    if (r->key[HK_RIGHT]) v |= NV_RIGHT;
    if (r->key[HK_ENTER] || r->key[HK_NUMENTER] || r->key[HK_SPACE]) v |= NV_OK;
    if (r->key[HK_ESC] || r->key[HK_BACKSPACE]) v |= NV_BACK;
    if (r->key[HK_PGUP]) v |= NV_PGUP;
    if (r->key[HK_PGDN]) v |= NV_PGDN;
    for (i = 0; i < HOST_PADS; i++) {
        unsigned n;
        if (!r->pad[i].connected) continue;
        n = pad_nav(&r->pad[i]);
        if (sOpen && sStack[sDepth] == SC_PADTEST && host_profile_find(host_settings(), &r->pad[i]) == sDev) {
            /* the gamepad under test: only its menu button does something (it leaves the test), so that pressing its buttons can be tried */
            if (n & NV_CLOSE) v |= NV_BACK;
            continue;
        }
        v |= n;
    }
    return v;
}

static int any_input(const HostRawInput *r)
{
    int i, k;
    for (i = 0; i < HK_COUNT; i++) if (r->key[i]) return 1;
    for (i = 0; i < HOST_PADS; i++) {
        unsigned char d[PB_COUNT];
        if (!r->pad[i].connected) continue;
        host_pad_resolve(&r->pad[i], d);
        for (k = 0; k < PB_COUNT; k++) if (d[k]) return 1;
    }
    return 0;
}

/* 1 when the menu button of some gamepad went down between `before` and `now`. */
static int menu_button_edge(const HostRawInput *now, const HostRawInput *before)
{
    int i;
    for (i = 0; i < HOST_PADS; i++) {
        HostPadProfile tmp;
        const HostPadProfile *pr;
        unsigned char a[PB_COUNT], b[PB_COUNT];
        int k;
        if (!now->pad[i].connected) continue;
        pr = profile_of(&now->pad[i], &tmp);
        host_pad_resolve(&now->pad[i], a);
        if (before->pad[i].connected && strcmp(before->pad[i].name, now->pad[i].name) == 0) host_pad_resolve(&before->pad[i], b);
        else memcpy(b, a, sizeof b);                                         /* a pad that has just appeared: what it holds is not a press */
        for (k = 0; k < 2; k++) {
            int id = pr->pad[ACT_MENU][k];
            if (id >= 0 && id < PB_COUNT && a[id] && !b[id]) return 1;
        }
    }
    return 0;
}

/* The menu opens with F1, Esc or a gamepad's menu button. */
static int open_pressed(const HostRawInput *now, const HostRawInput *before)
{
    if ((now->key[HK_F1] && !before->key[HK_F1]) || (now->key[HK_ESC] && !before->key[HK_ESC])) return 1;
    return menu_button_edge(now, before);
}

/* ---- what the items do ---------------------------------------------------------------------------------------------------------- */
static void open_browser(int purpose, int port, const char *current)
{
    HostSettings *s = host_settings();
    char dir[1100];
    memset(&sBr.dir, 0, sizeof sBr.dir);
    sBr.purpose = purpose;
    sBr.port = port;
    sBr.show_all = 0;
    sBr.can_create = 0;
    sBr.new_name[0] = 0;
    switch (purpose) {
    case BP_GAME_ROM: sBr.exts = "z64;n64;v64"; break;
    case BP_GB_ROM: sBr.exts = "gb;gbc"; break;
    case BP_GAME_SAVE:
        sBr.exts = "sav;srm;eep;fla";
        sBr.can_create = 1;
        snprintf(sBr.new_name, sizeof sBr.new_name, "kyujo.sav");
        break;
    default:
        sBr.exts = "sav;srm";
        if (s->gb_rom[port][0]) {
            char def[1100];
            sBr.can_create = 1;
            host_path_with_ext(s->gb_rom[port], ".sav", def, sizeof def);
            snprintf(sBr.new_name, sizeof sBr.new_name, "%s", host_path_basename(def));
        }
        break;
    }
    if (current && current[0]) host_path_dirname(current, dir, sizeof dir);
    else if (purpose == BP_GB_SAVE && s->gb_rom[port][0]) host_path_dirname(s->gb_rom[port], dir, sizeof dir);
    else snprintf(dir, sizeof dir, "%s", host_app_dir());
    if (!dir[0]) snprintf(dir, sizeof dir, "%s", host_app_dir());
    snprintf(sBr.dir, sizeof sBr.dir, "%s", dir);
    push(SC_BROWSER);
}

/* Loads (or reloads) the cartridge of controller port `port` from the settings. Returns 1 on success. */
static int apply_pak(int port)
{
    HostSettings *s = host_settings();
    char save[1100];
    const char *err;
#ifdef HOST_SHELL_PICKER
    host_settings_fix_gb_saves();
#endif
    if (!s->gb_rom[port][0]) { host_gbpak_unload(port); return 1; }
    if (s->gb_save[port][0]) snprintf(save, sizeof save, "%s", s->gb_save[port]);
    else host_path_with_ext(s->gb_rom[port], ".sav", save, sizeof save);
    err = host_gbpak_load(port, s->gb_rom[port], save);
    if (err) { message(err, COL_ERR); return 0; }
    return 1;
}

#ifdef HOST_SHELL_PICKER
/* ---- Android: the system's file picker takes the place of the file browser for the Game Boy ROM and the save file ---------------------------------------------------
 * The page looks and works as everywhere else (ROM, save file "(automatic)" = the ROM's name + ".sav", a different save can be chosen, "automatic" brings it back). Only the
 * choosing is done by the system's picker, because the files belong to the player's phone, outside the game's folder: the shell copies what was chosen into the game's folder. */
static int start_pick(int kind)
{
    HostSettings *s = host_settings();
    if (kind == 1 && !s->gb_rom[sPak][0]) { message("Choose the Game Boy ROM first.", COL_ERR); return 1; }
    if (!host_pick_gb_start(sPak, kind, s->gb_rom[sPak])) return 0;      /* no picker in this shell: the browser */
    sPicking = 1;
    message(kind ? "Choose the save file." : "Choose the Game Boy game.", COL_OK);
    return 1;
}

/* The picker is over: the ROM (kind 0) or the save (kind 1) it brought is in the game's folder. */
static void pick_gb_done(int port, int kind, const char *path, const char *msg)
{
    HostSettings *s = host_settings();
    char oldrom[HS_PATH], oldsave[HS_PATH], autosave[1100];
    if (port < 0 || port >= 4) return;
    if (!path[0]) { message(msg[0] ? msg : "Nothing was chosen.", COL_ERR); return; }
    snprintf(oldrom, sizeof oldrom, "%s", s->gb_rom[port]);
    snprintf(oldsave, sizeof oldsave, "%s", s->gb_save[port]);
    if (kind == 0) {
        snprintf(s->gb_rom[port], sizeof s->gb_rom[port], "%s", path);
        s->gb_save[port][0] = 0;                                        /* a new cartridge starts with its own automatic save file */
    } else {
        host_path_with_ext(s->gb_rom[port], ".sav", autosave, sizeof autosave);
        if (strcmp(autosave, path) == 0) s->gb_save[port][0] = 0;       /* it is the automatic one */
        else snprintf(s->gb_save[port], sizeof s->gb_save[port], "%s", path);
    }
    if (!apply_pak(port)) {                                             /* refused: what was in stays in */
        snprintf(s->gb_rom[port], sizeof s->gb_rom[port], "%s", oldrom);
        snprintf(s->gb_save[port], sizeof s->gb_save[port], "%s", oldsave);
        return;
    }
    message(msg[0] ? msg : (kind ? "Save file chosen." : "Cartridge inserted."), COL_OK);
    changed();
    needs_restart();
}
#endif

static void browser_chosen(const char *path)
{
    HostSettings *s = host_settings();
    int port = sBr.port;
    char oldrom[HS_PATH], oldsave[HS_PATH];
    const char *err;
    switch (sBr.purpose) {
    case BP_GAME_ROM:
        err = host_rom_check(path);
        if (err) { message(err, COL_ERR); return; }
        snprintf(s->rom, sizeof s->rom, "%s", path);
        message("Stadium ROM chosen. It is used after a restart.", COL_OK);
        break;
    case BP_GAME_SAVE:
        snprintf(s->save, sizeof s->save, "%s", path);
        message("Save file chosen. It is used after a restart.", COL_OK);
        break;
    case BP_GB_ROM:
        snprintf(oldrom, sizeof oldrom, "%s", s->gb_rom[port]);
        snprintf(oldsave, sizeof oldsave, "%s", s->gb_save[port]);
        snprintf(s->gb_rom[port], sizeof s->gb_rom[port], "%s", path);
        s->gb_save[port][0] = 0;                                        /* a new cartridge starts with its own automatic save file */
        if (!apply_pak(port)) {                                         /* refused: the cartridge that was in stays in */
            snprintf(s->gb_rom[port], sizeof s->gb_rom[port], "%s", oldrom);
            snprintf(s->gb_save[port], sizeof s->gb_save[port], "%s", oldsave);
            return;
        }
        message(sBoot ? "Cartridge inserted." : "Cartridge inserted. The game sees it after a restart.", COL_OK);
        break;
    default:
        snprintf(oldsave, sizeof oldsave, "%s", s->gb_save[port]);
        snprintf(s->gb_save[port], sizeof s->gb_save[port], "%s", path);
        if (s->gb_rom[port][0] && !apply_pak(port)) { snprintf(s->gb_save[port], sizeof s->gb_save[port], "%s", oldsave); return; }
        message(sBoot ? "Save file chosen." : "Save file chosen. The game uses it after a restart.", COL_OK);
        break;
    }
    changed();
    needs_restart();
    pop();                                                              /* back to the screen that opened the browser */
}

/* The folder of the save file in use (where a new or backup file goes). */
static void save_folder(char *out, unsigned size)
{
    host_path_dirname(host_save_path(), out, size);
    if (!out[0]) snprintf(out, size, "%s", host_app_dir());
}

static void new_save(void)
{
    HostSettings *s = host_settings();
    char dir[1100], name[300], path[1500];
    int n;
    save_folder(dir, sizeof dir);
    for (n = 2; n < 1000; n++) {
        snprintf(name, sizeof name, "kyujo-%d.sav", n);
        join_path(path, sizeof path, dir, name);
        if (!file_exists(path)) break;
    }
    if (n >= 1000) { message("Too many save files in the folder.", COL_ERR); return; }
    snprintf(s->save, sizeof s->save, "%s", path);
    changed();
    needs_restart();
    snprintf(path, sizeof path, "New empty save %s is used after a restart.", name);
    message(path, COL_OK);
}

static void backup_save(void)
{
    char dir[1100], base[300], name[340], path[1500], msg[1700];
    const char *src = host_save_path(), *file = host_path_basename(src), *dot = strrchr(file, '.');
    int n;
    host_save_flush();                                              /* the file is written a moment after the game's last change (host_save.c) */
    if (!file_exists(src)) { message("There is no save file yet (the game creates it when it saves).", COL_ERR); return; }
    snprintf(base, sizeof base, "%.*s", dot ? (int)(dot - file) : (int)strlen(file), file);
    save_folder(dir, sizeof dir);
    for (n = 1; n < 1000; n++) {
        snprintf(name, sizeof name, "%s-backup-%d.sav", base, n);
        join_path(path, sizeof path, dir, name);
        if (!file_exists(path)) break;
    }
    if (n >= 1000) { message("Too many backup files in the folder.", COL_ERR); return; }
    if (!copy_file(src, path)) { message("The backup could not be written.", COL_ERR); return; }
    snprintf(msg, sizeof msg, "Backup written: %s", name);
    message(msg, COL_OK);
}

/* Puts the default buttons back on the device the page works on (its port and, for a gamepad, nothing else changes except its dead zone). */
static void reset_device(void)
{
    HostSettings def, *s = host_settings();
    host_settings_defaults(&def);
    if (sDev == DEV_KEYBOARD) {
        memcpy(s->key, def.key, sizeof s->key);
    } else if (sDev >= 0 && s->profile[sDev].name[0]) {
        int port = s->profile[sDev].port;
        host_profile_set_defaults(&s->profile[sDev], s->profile[sDev].generic);
        s->profile[sDev].port = port;
    }
    changed();
}

/* Choice items change with Left/Right (dir = -1 / +1) and also with Enter (dir = +1; `wrap` lets the value go round). */
static void change_choice(int id, int dir, int wrap)
{
    HostSettings *s = host_settings();
    if (id == I_WIDE) { s->widescreen = !s->widescreen; changed(); return; }
    if (id == I_SCALE) { s->scale_style = (s->scale_style + (dir < 0 ? 2 : 1)) % 3; changed(); return; }
    if (id == I_CHEAT) { s->cheats = !s->cheats; changed(); return; }
    if (id == I_CHEAT_HOF) { s->cheat_hof = !s->cheat_hof; changed(); return; }
    if (id == I_CHEAT_CPUVS) { s->cheat_cpuvs = !s->cheat_cpuvs; changed(); return; }
    if (id == I_FULL) { s->fullscreen = !s->fullscreen; host_window_set_fullscreen(s->fullscreen); changed(); return; }
    if (id == I_VOLUME) {
        int v = s->volume + dir * 10;
        if (v > 100) v = wrap ? 0 : 100;
        if (v < 0) v = wrap ? 100 : 0;
        s->volume = v;
        changed();
        return;
    }
    if (id == I_DEVPORT && sDev == DEV_KEYBOARD) { s->keyboard_port = (s->keyboard_port + dir + 5) % 5; changed(); return; }
    if (id == I_DEVPORT && sDev >= 0) { s->profile[sDev].port = (s->profile[sDev].port + dir + 5) % 5; changed(); return; }
    if (id == I_DEAD && sDev >= 0) {
        int *dz = &s->profile[sDev].deadzone;
        *dz += dir * 2;
        if (*dz < 0) *dz = 0;
        if (*dz > 50) *dz = 50;
        changed();
        return;
    }
    if (id == I_BR_ALL) { sBr.show_all = !sBr.show_all; browser_load(); sCursor[sDepth] = 0; sScroll[sDepth] = 0; return; }
}

/* Builds the list of the current screen and puts the cursor on a line that can be chosen. */
static void rebuild(void)
{
    int *c = &sCursor[sDepth];
    build_items();
    if (sStack[sDepth] == SC_DEVICE) {                              /* the device page has cells, not list lines */
        if (*c >= sCellN) *c = 0;
        return;
    }
    if (*c >= sN) *c = sN ? sN - 1 : 0;
    while (*c < sN - 1 && sItems[*c].kind == K_LABEL) (*c)++;
}

static void do_restart(void)
{
    HostSettings *s = host_settings();
    host_menu_notice("Restarting...");
    if (sDirtySettings) host_settings_save(s, host_settings_path());
    host_gbpak_flush();
    if (!host_restart()) message("The game could not be started again.", COL_ERR);
}

static void activate(const Item *it)
{
    HostSettings *s = host_settings();
    int id = it->id;
    if (it->kind == K_LABEL) return;
    if (it->kind == K_CHOICE) { change_choice(id, 1, 1); return; }
    switch (id) {
    case I_BACK: pop(); return;
    case I_RESUME: request_close(); return;
    case I_PAKS: push(SC_PAKS); return;
    case I_CTRL: push(SC_CTRL); return;
    case I_GAMEROM: push(SC_GAMEROM); return;
    case I_OPTIONS: push(SC_OPTIONS); return;
    case I_ABOUT: push(SC_ABOUT); return;
    case I_QUIT: push(SC_QUIT); return;
    case I_RESTART: push(SC_RESTART); return;
    case I_RESTART_YES: do_restart(); return;
    case I_ASK_LATER: pop(); return;
    case I_QUIT_YES:
    case I_QUIT_CLOSE:
        host_menu_notice("Quitting...");
        if (sDirtySettings) host_settings_save(s, host_settings_path());
        host_gbpak_flush();
        host_exit(it->id == I_QUIT_CLOSE ? 100 : 0);                 /* 100: the console shell ends the whole process instead of returning to its launcher */
        return;
    case I_DEVTEST: push(SC_PADTEST); return;
    case I_DEVKB: sDev = DEV_KEYBOARD; push(SC_DEVICE); return;
    case I_EXIT: request_close(); return;
#ifdef HOST_SHELL_PICKER
    case I_PAK_ROM: if (start_pick(0)) return; open_browser(BP_GB_ROM, sPak, s->gb_rom[sPak]); return;
    case I_PAK_SAVE: if (start_pick(1)) return; open_browser(BP_GB_SAVE, sPak, s->gb_save[sPak][0] ? s->gb_save[sPak] : NULL); return;
#else
    case I_PAK_ROM: open_browser(BP_GB_ROM, sPak, s->gb_rom[sPak]); return;
    case I_PAK_SAVE: open_browser(BP_GB_SAVE, sPak, s->gb_save[sPak][0] ? s->gb_save[sPak] : NULL); return;
#endif
    case I_PAK_DEFAULT_SAVE:
        s->gb_save[sPak][0] = 0;
        if (s->gb_rom[sPak][0]) apply_pak(sPak);
        changed();
        needs_restart();
        message(sBoot ? "The automatic save file name is used." : "The automatic save file name is used.", COL_OK);
        return;
    case I_PAK_REMOVE:
        s->gb_rom[sPak][0] = 0;
        s->gb_save[sPak][0] = 0;
        host_gbpak_unload(sPak);
        changed();
        needs_restart();
        message(sBoot ? "Cartridge taken out." : "Cartridge taken out. The game notices after a restart.", COL_OK);
        return;
    case I_ST_ROM: open_browser(BP_GAME_ROM, 0, s->rom); return;
    case I_ST_SAVE: open_browser(BP_GAME_SAVE, 0, s->save[0] ? s->save : host_save_path()); return;
    case I_ST_NEW: new_save(); return;
    case I_ST_BACKUP: backup_save(); return;
    case I_ST_DEFAULT:
        s->save[0] = 0;
        changed();
        needs_restart();
        message("The default save file is used after a restart.", COL_OK);
        return;
    case I_BR_PARENT: {
        char parent[1100];
        host_path_parent(sBr.dir, parent, sizeof parent);
        snprintf(sBr.dir, sizeof sBr.dir, "%s", parent);
        browser_load();
        sCursor[sDepth] = 0;
        sScroll[sDepth] = 0;
        return; }
    case I_BR_NEW: {
        char path[1500];
        join_path(path, sizeof path, sBr.dir, sBr.new_name);
        browser_chosen(path);
        return; }
    default: break;
    }
    if (id >= I_PORT0 && id < I_PORT0 + 4) { sPak = id - I_PORT0; push(SC_PAK); return; }
    if (id >= I_DEV0 && id < I_DEV0 + HS_PROFILES) { sDev = id - I_DEV0; push(SC_DEVICE); return; }
    if (id >= I_ENTRY0 && id - I_ENTRY0 < sBr.n) {
        const HostDirEntry *e = &sBr.e[id - I_ENTRY0];
        char path[1500];
        join_path(path, sizeof path, sBr.dir, e->name);
        if (e->is_dir) {
            snprintf(sBr.dir, sizeof sBr.dir, "%s", path);
            browser_load();
            sCursor[sDepth] = 0;
            sScroll[sDepth] = 0;
        } else {
            browser_chosen(path);
        }
    }
}

/* ---- capturing a key or button for a binding -------------------------------------------------------------------------------------- */
typedef struct Capture {
    int active, action, is_key;
    int setup, setup_i, setup_n, order[ACT_COUNT];      /* "Setup": the actions that are asked for one after the other */
} Capture;

/* Puts `value` first among the action's two alternatives (the old first one becomes the second) and takes it from every other action. */
static void bind_value(int (*arr)[2], int none, int count, int action, int value)
{
    int a, k, old0, old1;
    for (a = 0; a < count; a++) {
        if (a == action) continue;
        for (k = 0; k < 2; k++) if (arr[a][k] == value) arr[a][k] = none;
    }
    old0 = arr[action][0];
    old1 = arr[action][1];
    if (old0 == value) return;
    arr[action][0] = value;
    arr[action][1] = old0 != none ? old0 : (old1 == value ? none : old1);
    changed();
}

/* How capture_step ended: still waiting, an input was bound (or cleared), the player stopped, or skipped this one (Setup only). */
enum { CAP_WAIT = 0, CAP_DONE, CAP_STOP, CAP_SKIP };

/* Looks at a new snapshot while capturing. In Setup, Esc skips the action and the pad's menu button (F1 on the keyboard) stops the whole Setup. */
static int capture_step(Capture *c, const HostRawInput *now)
{
    HostSettings *s = host_settings();
    int k;
    char buf[200];
    if (c->is_key) {
        if (now->key[HK_ESC] && !sPrev.key[HK_ESC]) return c->setup ? CAP_SKIP : CAP_STOP;
        if (now->key[HK_DELETE] && !sPrev.key[HK_DELETE]) { s->key[c->action][0] = s->key[c->action][1] = HK_NONE; changed(); return CAP_DONE; }
        if (now->key[HK_F1] && !sPrev.key[HK_F1]) {
            if (!c->setup) message("F1 opens the menu, so it cannot be used as a game button.", COL_ERR);
            return CAP_STOP;
        }
        for (k = 1; k < HK_COUNT; k++) {
            if (!now->key[k] || sPrev.key[k]) continue;
            if (!host_key_name(k)) continue;
            if (c->setup) s->key[c->action][0] = s->key[c->action][1] = HK_NONE;     /* Setup replaces the old buttons; a single capture adds to them */
            bind_value(s->key, HK_NONE, ACT_KEY_COUNT, c->action, k);
            snprintf(buf, sizeof buf, "%s is now %s.", host_action_label(c->action), host_key_name(k));
            message(buf, COL_OK);
            return CAP_DONE;
        }
        if (menu_button_edge(now, &sPrev)) return CAP_STOP;                            /* a gamepad's menu button cancels, so the capture can be left without the keyboard */
    } else {
        int pi = pad_of_profile(now, sDev), j;
        HostPadProfile *pr;
        unsigned char a[PB_COUNT], b[PB_COUNT];
        if (sDev < 0 || pi < 0) { message("Connect this gamepad to change its buttons.", COL_ERR); return CAP_STOP; }
        pr = &s->profile[sDev];
        if (now->key[HK_ESC] && !sPrev.key[HK_ESC]) return c->setup ? CAP_SKIP : CAP_STOP;
        if (now->key[HK_DELETE] && !sPrev.key[HK_DELETE]) { pr->pad[c->action][0] = pr->pad[c->action][1] = -1; changed(); return CAP_DONE; }
        host_pad_resolve(&now->pad[pi], a);
        if (sPrev.pad[pi].connected) host_pad_resolve(&sPrev.pad[pi], b); else memcpy(b, a, sizeof b);
        if (c->action != ACT_MENU) {                                                   /* the pad's menu button cancels (unless that is the button being set) */
            for (j = 0; j < 2; j++) if (pr->pad[ACT_MENU][j] >= 0 && a[pr->pad[ACT_MENU][j]] && !b[pr->pad[ACT_MENU][j]]) return CAP_STOP;
        }
        for (k = 0; k < PB_COUNT; k++) {                                               /* ids are in the order buttons, hat, axes: the plain button wins when a press also moves an axis */
            if (!a[k] || b[k]) continue;
            if (c->setup) pr->pad[c->action][0] = pr->pad[c->action][1] = -1;        /* Setup replaces the old buttons; a single capture adds to them */
            bind_value(pr->pad, -1, ACT_COUNT, c->action, k);
            snprintf(buf, sizeof buf, "%s is now %s.", host_action_label(c->action), host_pad_button_label(k));
            message(buf, COL_OK);
            return CAP_DONE;
        }
    }
    return CAP_WAIT;
}

/* The actions that Setup asks for, in the order of Project64's Setup: buttons, D-pad, triggers, C buttons; the keyboard also the stick directions
 * (a gamepad's left stick is the analog stick), a gamepad also its menu button. Returns how many. */
static int setup_order(int is_key, int out[ACT_COUNT])
{
    static const int kBase[] = {ACT_A, ACT_B, ACT_Z, ACT_START, ACT_DUP, ACT_DDOWN, ACT_DLEFT, ACT_DRIGHT, ACT_L, ACT_R, ACT_CUP, ACT_CDOWN, ACT_CLEFT, ACT_CRIGHT};
    int n = 0, i;
    for (i = 0; i < (int)(sizeof kBase / sizeof kBase[0]); i++) out[n++] = kBase[i];
    if (is_key) { out[n++] = ACT_STICK_UP; out[n++] = ACT_STICK_DOWN; out[n++] = ACT_STICK_LEFT; out[n++] = ACT_STICK_RIGHT; }
    else out[n++] = ACT_MENU;
    return n;
}

static void start_capture(Capture *cap, int action)
{
    int kb = sDev == DEV_KEYBOARD;
    if (!kb && pad_of_profile(&sRawNow, sDev) < 0) { message("Connect this gamepad to change its buttons.", COL_ERR); return; }
    memset(cap, 0, sizeof *cap);
    cap->active = 1;
    cap->action = action;
    cap->is_key = kb;
    sMessage[0] = 0;
}

static void start_setup(Capture *cap)
{
    int kb = sDev == DEV_KEYBOARD;
    if (!kb && pad_of_profile(&sRawNow, sDev) < 0) { message("Connect this gamepad to change its buttons.", COL_ERR); return; }
    memset(cap, 0, sizeof *cap);
    cap->active = 1;
    cap->setup = 1;
    cap->is_key = kb;
    cap->setup_n = setup_order(kb, cap->order);
    cap->action = cap->order[0];
    sMessage[0] = 0;
}

static void forget_device(void)
{
    HostSettings *s = host_settings();
    if (sDev < 0) return;
    memset(&s->profile[sDev], 0, sizeof s->profile[sDev]);
    sDev = -1;
    changed();
    message("Gamepad forgotten. It is set up again when it is plugged in.", COL_OK);
    pop();
}

/* Keys and buttons on the device page: move between the cells, change a choice, start a capture. */
static void layout_input(unsigned edge, Capture *cap)
{
    int cur = sCursor[sDepth], to = -1;
    const LCell *c;
    if (sCellN == 0) { if (edge & NV_BACK) pop(); return; }
    if (cur >= sCellN) cur = sCursor[sDepth] = 0;
    c = &sCells[cur];
    if ((edge & (NV_LEFT | NV_RIGHT)) && c->choice) { change_choice(c->id == LC_PORT ? I_DEVPORT : I_DEAD, (edge & NV_LEFT) ? -1 : 1, 0); return; }
    if (edge & NV_UP) to = layout_neighbor(cur, 0, -1);
    else if (edge & NV_DOWN) to = layout_neighbor(cur, 0, 1);
    else if (edge & NV_LEFT) to = layout_neighbor(cur, -1, 0);
    else if (edge & NV_RIGHT) to = layout_neighbor(cur, 1, 0);
    if (to >= 0) { sCursor[sDepth] = to; return; }
    if (edge & (NV_UP | NV_DOWN | NV_LEFT | NV_RIGHT)) return;
    if (edge & NV_OK) {
        if (c->id >= LC_ACT && c->id < LC_ACT + ACT_COUNT) { start_capture(cap, c->id - LC_ACT); return; }
        switch (c->id) {
        case LC_PORT: change_choice(I_DEVPORT, 1, 1); break;
        case LC_DEAD: change_choice(I_DEAD, 1, 1); break;
        case LC_SETUP: start_setup(cap); break;
        case LC_RESET: reset_device(); message("The default buttons are back.", COL_OK); break;
        case LC_TEST: push(SC_PADTEST); break;
        case LC_FORGET: forget_device(); break;
        case LC_BACK: sMessage[0] = 0; pop(); break;
        default: break;
        }
        return;
    }
    if (edge & NV_BACK) { sMessage[0] = 0; pop(); }
}

/* ---- gamepads coming and going ---------------------------------------------------------------------------------------------------- */
/* Makes sure every connected gamepad has a profile (saved at once, so that a new pad keeps its port) and shows a notice when one is plugged in or out. */
static void update_devices(const HostRawInput *raw)
{
    static unsigned char known[HOST_PADS];
    static char names[HOST_PADS][48];
    HostSettings *s = host_settings();
    int i;
    if (host_profiles_sync(s, raw) > 0) {
        if (host_settings_save(s, host_settings_path()) != 0) fprintf(stderr, "kyujo: the settings could not be saved\n");
    }
    for (i = 0; i < HOST_PADS; i++) {
        const HostRawPad *p = &raw->pad[i];
        char buf[120];
        if (p->connected == known[i] && (!p->connected || strcmp(names[i], p->name) == 0)) continue;
        if (p->connected) {
            int pi = host_profile_find(s, p);
            if (pi >= 0 && s->profile[pi].port) snprintf(buf, sizeof buf, "Gamepad connected: %s (N64 controller %d)", p->name, s->profile[pi].port);
            else snprintf(buf, sizeof buf, "Gamepad connected: %s", p->name);
            fprintf(stderr, "kyujo: %s\n", buf);
            snprintf(names[i], sizeof names[i], "%s", p->name);
        } else {
            snprintf(buf, sizeof buf, "Gamepad disconnected: %s", names[i]);
            fprintf(stderr, "kyujo: %s\n", buf);
        }
        known[i] = p->connected;
        host_menu_notice(buf);
    }
}

/* A number that changes whenever something the menu shows changes (so a picture is made only then). */
static unsigned view_hash(int cap_active, int cap_action)
{
    unsigned h = 2166136261u;
    int i;
    const char *p;
#define HASH_BYTE(b) (h = (h ^ (unsigned char)(b)) * 16777619u)
    HASH_BYTE(sStack[sDepth]); HASH_BYTE(sCursor[sDepth]); HASH_BYTE(sScroll[sDepth]); HASH_BYTE(cap_active); HASH_BYTE(cap_action);
    HASH_BYTE(sMessage[0] && host_wall_ns() < sMessageUntil);
    for (p = sMessage; *p; p++) HASH_BYTE(*p);
    for (i = 0; i < sN; i++) {
        for (p = sItems[i].label; *p; p++) HASH_BYTE(*p);
        HASH_BYTE(1);
        for (p = sItems[i].value; *p; p++) HASH_BYTE(*p);
        HASH_BYTE(sItems[i].kind);
    }
    if (sStack[sDepth] == SC_DEVICE) {                              /* the cells of the device page, and which of them are held down */
        for (i = 0; i < sCellN; i++) {
            for (p = sCells[i].value; *p; p++) HASH_BYTE(*p);
            HASH_BYTE(sCells[i].pressed); HASH_BYTE(sCells[i].id);
        }
    }
#undef HASH_BYTE
    return h;
}

/* ---- the open menu ------------------------------------------------------------------------------------------------------------------ */
static void menu_loop(void)
{
    HostRawInput raw;
    HostCanvas cv;
    Capture cap;
    HostSettings *s = host_settings();
    unsigned held_prev = 0, last_hash = 0;
    int hold_ticks[8] = {0}, redraw = 1;
    unsigned short view_w = 0, view_h = 0;
    int swallow = 0;                                                /* after a capture: the button that was pressed may now mean something in the menu (a gamepad's own A) */
    int i;

    if (!host_gameui_font_ok(SZ) || !host_gameui_font_ok(SZ_TITLE)) {
        host_menu_notice("The menu needs the game's font, which could not be read from the ROM.");
        __atomic_store_n(&sRequest, -1, __ATOMIC_SEQ_CST);
        return;
    }
    if (!sItems) sItems = (Item *)malloc(sizeof *sItems * MAX_ITEMS);
    if (!sCanvasRgb) sCanvasRgb = (unsigned char *)malloc((size_t)HUI_W * HUI_H * 3);
    if (!sBaseTried) make_base();
    if (!sItems || !sCanvasRgb || !sBase) return;
    cv.rgb = sCanvasRgb; cv.w = HUI_W; cv.h = HUI_H; cv.fx = cv.fy = 0;

    memset(&cap, 0, sizeof cap);
    fprintf(stderr, "kyujo: menu opened (the game is paused)\n");
    host_clock_pause();
    sOpen = 1;
    goto_screen(HM_MAIN);
    sMessage[0] = 0;
    sAskThenClose = 0;
    if (s->fullscreen != host_window_is_fullscreen()) { s->fullscreen = host_window_is_fullscreen(); changed(); }   /* F11 may have changed it since */
    host_raw_get(&sPrev);
    sRawNow = sPrev;
    held_prev = nav_bits(&sPrev);                                   /* the keys that opened the menu are not "presses" inside it */

    while (sOpen) {
        unsigned nav, edge;
        int rq;
        host_raw_script_tick();
        host_raw_get(&raw);
        sRawNow = raw;
        if (raw.view_w != view_w || raw.view_h != view_h) { view_w = raw.view_w; view_h = raw.view_h; redraw = 1; }      /* the window's size became known (or changed): draw the menu as wide as it is */
        update_devices(&raw);
#ifdef HOST_SHELL_PICKER
        if (sPicking) {                                             /* the system's file picker was opened for a Game Boy file: its answer comes by itself */
            int pp = -1, pk = 0;
            char pth[1100], pmsg[400];
            pth[0] = pmsg[0] = 0;
            if (host_pick_gb_poll(&pp, &pk, pth, sizeof pth, pmsg, sizeof pmsg)) {
                sPicking = 0;
                pick_gb_done(pp, pk, pth, pmsg);
                redraw = 1;
            }
        }
#endif
        rq = __atomic_exchange_n(&sRequest, -1, __ATOMIC_SEQ_CST);
        if (rq >= 0) { goto_screen(rq); cap.active = 0; redraw = 1; }
        nav = nav_bits(&raw);
        edge = nav & ~held_prev;
        if (swallow) { if (!nav) swallow = 0; edge = 0; }
        for (i = 0; i < 8; i++) {                                   /* auto repeat for the movement keys while held */
            unsigned bit = 1u << i;
            if (!(bit & (NV_UP | NV_DOWN | NV_LEFT | NV_RIGHT | NV_PGUP | NV_PGDN))) continue;
            if (nav & bit) {
                if (!(held_prev & bit)) hold_ticks[i] = 0;
                else if (++hold_ticks[i] > 26 && (hold_ticks[i] - 26) % 4 == 0) edge |= bit;
            }
        }
        if (cap.active) {
            int r = capture_step(&cap, &raw);
            if (r != CAP_WAIT) {
                redraw = 1;
                swallow = 1;
                if (cap.setup && r != CAP_STOP && cap.setup_i + 1 < cap.setup_n) cap.action = cap.order[++cap.setup_i];      /* Setup: the next button */
                else {
                    if (cap.setup && r != CAP_STOP) message("Setup finished.", COL_OK);
                    cap.active = cap.setup = 0;
                }
            }
        } else {
            rebuild();
            if ((raw.key[HK_F1] && !sPrev.key[HK_F1]) || (edge & NV_CLOSE)) { request_close(); }
            else if (sStack[sDepth] == SC_DEVICE) { layout_input(edge, &cap); }
            else if (edge & (NV_UP | NV_DOWN | NV_PGUP | NV_PGDN)) {
                int step = (edge & (NV_PGUP | NV_PGDN)) ? visible_rows() - 1 : 1, dir = (edge & (NV_UP | NV_PGUP)) ? -1 : 1, c = sCursor[sDepth], tries;
                for (tries = 0; tries < step; tries++) {
                    int n = c + dir;
                    while (n >= 0 && n < sN && sItems[n].kind == K_LABEL) n += dir;
                    if (n < 0 || n >= sN) break;
                    c = n;
                }
                if (c == sCursor[sDepth] && step == 1 && sN > 0) {                         /* wrap around at the ends of short lists */
                    c = dir < 0 ? sN - 1 : 0;
                    while (c >= 0 && c < sN && sItems[c].kind == K_LABEL) c += dir < 0 ? -1 : 1;
                }
                if (c >= 0 && c < sN) sCursor[sDepth] = c;
            } else if ((edge & (NV_LEFT | NV_RIGHT)) && sN > 0 && sItems[sCursor[sDepth]].kind == K_CHOICE) {
                change_choice(sItems[sCursor[sDepth]].id, (edge & NV_LEFT) ? -1 : 1, 0);
            } else if ((edge & NV_OK) && sN > 0) {
                const Item it = sItems[sCursor[sDepth]];
                activate(&it);
            } else if (edge & NV_BACK) {
                pop();
            }
            if (!cap.active) maybe_ask();
        }
        held_prev = nav;
        sPrev = raw;
        if (sOpen) {
            unsigned h;
            char prompt[160], sub[160], help[160];
            rebuild();
            h = view_hash(cap.active, cap.action);
            if (redraw || h != last_hash) {
                snprintf(prompt, sizeof prompt, "Press the %s%s for \"%s\"", cap.setup ? "" : "new ", cap.is_key ? "key" : "button", host_action_label(cap.action));
                sub[0] = 0;
                if (cap.setup) snprintf(sub, sizeof sub, "Setup %d of %d", cap.setup_i + 1, cap.setup_n);
                if (!cap.is_key && cap.action >= ACT_CUP && cap.action <= ACT_CRIGHT)
                    snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%sor push the right stick", sub[0] ? "  -  " : "");
                if (cap.setup) snprintf(help, sizeof help, cap.is_key ? "Esc: skip  Delete: clear  F1: stop" : "Esc: skip  Delete: clear  Menu button: stop");
                else snprintf(help, sizeof help, cap.is_key ? "Esc: cancel      Delete: clear" : "Esc / menu button: cancel      Delete: clear");
                draw_menu(&cv, cap.active, prompt, sub, help);
                present(cv.rgb);
                last_hash = h;
                redraw = 0;
            }
        }
        if (!sBoot) host_audio_idle_tick();                         /* the game is paused: keep the sound stream alive with silence (nothing plays yet at start-up) */
        host_sleep_ns(16000000ULL);
    }
    if (sDirtySettings) {
        if (host_settings_save(host_settings(), host_settings_path()) == 0) host_menu_notice("Settings saved.");
        else host_menu_notice("The settings could not be saved (see the log).");
        sDirtySettings = 0;
    }
    fprintf(stderr, "kyujo: menu closed\n");
    sBlockUntilRelease = 1;                                         /* the game sees no input until the keys that closed the menu are up (but not longer than a moment) */
    sBlockDeadline = host_wall_ns() + 1500000000ULL;
    host_clock_resume();
}

int host_menu_is_open(void) { return sOpen || sBlockUntilRelease; }

/* Every build (shell and native), once, before the game's Main(): gives the shell the menu wallpaper (its start-up picture uses it), shows the Game Pak Setup page when no Game Boy
 * cartridge was ever set up (a first start: the page comes once), and then lets the shell prepare its shaders. The game has not started: nothing is paused, no restart is needed. */
void host_first_run_setup(void)
{
    const unsigned char *bg = host_gameui_background();
    HostSettings *hs = host_settings();
    if (bg) {                                                      /* one tile of the wallpaper (128 x 64): the pattern repeats with that period */
        static unsigned char tile[128 * 64 * 3];
        unsigned y;
        for (y = 0; y < 64; y++) memcpy(tile + (size_t)y * 128 * 3, bg + (size_t)y * HUI_W * 3, 128 * 3);
        host_window_set_backdrop(tile, 128, 64);
    }
    if (!hs->pak_setup_done && !getenv("KYUJO_INPUT") && !getenv("KYUJO_RAW") && !getenv("KYUJO_MOVETEST") && !getenv("KYUJO_NO_SETUP_PAGE")) {
        int any = 0, k;
        for (k = 0; k < 4; k++) any |= host_gbpak_present(k) != 0;
        if (!any) {
            fprintf(stderr, "kyujo: no Game Boy cartridge is set up: the Game Pak Setup page comes first\n");
            sBoot = 1;
            host_menu_request(HM_SETUP);
            host_raw_get(&sPrev);
            menu_loop();
            sBoot = 0;
            if (!hs->pak_setup_done) { hs->pak_setup_done = 1; host_settings_save(hs, host_settings_path()); }     /* leaving by any way counts as seen */
        }
    }
    host_window_graphics_go();
}

/* Called once per picture by the video output. */
void host_menu_poll(void)
{
    static int first = 1, asked;
    HostRawInput raw;
    host_raw_script_tick();
    host_raw_get(&raw);
    if (first) {
        first = 0;
        host_menu_notice(raw.console ? "Minus: menu" : "F1 or Esc: menu");
        sPrev = raw;
    }
    if (!asked && getenv("KYUJO_MENU_OPEN") && host_raw_script_count() >= 300) {      /* development: as if the menu bar had asked */
        asked = 1;
        host_menu_request(atoi(getenv("KYUJO_MENU_OPEN")));
    }
    update_devices(&raw);
    {   /* F11, Alt+Enter or the menu bar changed full screen: remember it (the window layer owns the state) */
        static int last_full = -1;
        int full = host_window_is_fullscreen();
        if (last_full < 0) last_full = full;
        else if (full != last_full) {
            last_full = full;
            if (host_settings()->fullscreen != full) { host_settings()->fullscreen = full; host_settings_save(host_settings(), host_settings_path()); }
        }
    }
    if (__atomic_load_n(&sRequest, __ATOMIC_SEQ_CST) >= 0) {        /* the Windows menu bar asked for a screen */
        sBlockUntilRelease = 0;
        sPrev = raw;
        menu_loop();
        return;
    }
    if (sBlockUntilRelease) {
        if (!any_input(&raw) || host_wall_ns() > sBlockDeadline) sBlockUntilRelease = 0;
        sPrev = raw;
        return;
    }
    if (open_pressed(&raw, &sPrev)) {
        if (getenv("KYUJO_MENU_TRACE")) fprintf(stderr, "kyujo: menu: poll %u opened by F1=%d/%d Esc=%d/%d\n", host_raw_script_count(), raw.key[HK_F1], sPrev.key[HK_F1], raw.key[HK_ESC], sPrev.key[HK_ESC]);
        sPrev = raw;
        menu_loop();
        return;
    }
    sPrev = raw;
}
