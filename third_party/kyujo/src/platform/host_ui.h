/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_ui.h - settings, raw input devices and the in-window menu of the host layer (HOST-side header: not for game code).
 *
 * The pieces, and which of them is system specific:
 *   host_keys.c      names of keyboard keys and gamepad buttons (portable).
 *   host_settings.c  the settings and their file kyujo.cfg (portable).
 *   host_input.c     turns the raw devices into the four N64 controllers with the bindings of the settings (portable).
 *   host_menu.c      the menu drawn over the picture, and the pause while it is open (portable: only needs the raw devices and the picture).
 *   host_window.c    reads keyboard and gamepads into a HostRawInput and shows the picture (Windows; a Switch layer would do the same job).
 * Nothing here needs the Windows API, so the menus work the same on every system the window layer is written for.
 */
#ifndef HOST_UI_H
#define HOST_UI_H

/* ---- keyboard keys ------------------------------------------------------------------------------------------------------------
 * Our own key numbers, so that the settings file does not depend on the Windows virtual-key codes. Letters and digits are their capital ASCII
 * code. The window layer converts the system's keys to these numbers. */
enum {
    HK_NONE = 0,
    HK_BACKSPACE = 8, HK_TAB = 9, HK_ENTER = 13, HK_ESC = 27, HK_SPACE = 32,
    /* '0'..'9' = 48..57, 'A'..'Z' = 65..90 */
    HK_UP = 128, HK_DOWN, HK_LEFT, HK_RIGHT, HK_LSHIFT, HK_RSHIFT, HK_LCTRL, HK_RCTRL, HK_LALT, HK_RALT,
    HK_INSERT, HK_DELETE, HK_HOME, HK_END, HK_PGUP, HK_PGDN,
    HK_F1, HK_F2, HK_F3, HK_F4, HK_F5, HK_F6, HK_F7, HK_F8, HK_F9, HK_F10, HK_F11, HK_F12,
    HK_NUM0, HK_NUM1, HK_NUM2, HK_NUM3, HK_NUM4, HK_NUM5, HK_NUM6, HK_NUM7, HK_NUM8, HK_NUM9,
    HK_NUMADD, HK_NUMSUB, HK_NUMMUL, HK_NUMDIV, HK_NUMDOT, HK_NUMENTER,
    HK_SEMICOLON, HK_QUOTE, HK_COMMA, HK_PERIOD, HK_SLASH, HK_LBRACKET, HK_RBRACKET, HK_BACKSLASH, HK_MINUS, HK_EQUALS, HK_BACKQUOTE,
    HK_COUNT_ = 256
};
#define HK_COUNT 256

const char *host_key_name(int key);               /* "A", "SPACE", "F1", ... ; NULL for a number that is not a key */
int host_key_from_name(const char *name);         /* case-insensitive; HK_NONE if unknown */

/* ---- gamepad inputs ------------------------------------------------------------------------------------------------------------
 * Every input of a gamepad has a number (PB_*) so that it can be bound to an N64 button. Two kinds of devices exist:
 *   - "Xbox layout" devices (what XInput reports): PB_A ... PB_RS_RIGHT. The sticks and triggers also count as buttons (pushed past a threshold).
 *   - "generic" devices (what Windows reports for DualShock/DualSense, Switch Pro and most other pads): the buttons are only numbered
 *     (PB_G_BTN1 ...), the hat switch is PB_G_HAT_*, and the six axes X Y Z R U V count as buttons in both directions (PB_G_AX_*). Which
 *     number is which button depends on the pad, so the menu lets the player press the button to bind it. */
enum {
    PB_A = 0, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_BACK, PB_START, PB_LSTICK, PB_RSTICK,
    PB_DUP, PB_DDOWN, PB_DLEFT, PB_DRIGHT, PB_LT, PB_RT,
    PB_LS_UP, PB_LS_DOWN, PB_LS_LEFT, PB_LS_RIGHT, PB_RS_UP, PB_RS_DOWN, PB_RS_LEFT, PB_RS_RIGHT,
    PB_G_BTN1,                                     /* ... PB_G_BTN1 + 31 = button 32 */
    PB_G_HAT_UP = PB_G_BTN1 + 32, PB_G_HAT_DOWN, PB_G_HAT_LEFT, PB_G_HAT_RIGHT,
    PB_G_AX_X_NEG,                                 /* then X+, Y-, Y+, Z-, Z+, R-, R+, U-, U+, V-, V+ */
    PB_COUNT = PB_G_AX_X_NEG + 12
};
#define PB_XBOX_PLAIN 14                           /* PB_A ... PB_DRIGHT are plain buttons of an Xbox-layout device */
const char *host_pad_button_name(int button);     /* the name in the settings file: "A", "LB", "DUP", "RS_LEFT", "BTN3", "HAT_UP", "AXZ+" */
int host_pad_button_from_name(const char *name);  /* -1 if unknown */
const char *host_pad_button_label(int button);    /* for the menu: "A", "Left bumper", "L stick up", "Button 3", "Hat up", "Axis Z +" */

/* ---- raw input snapshot ------------------------------------------------------------------------------------------------------- */
#define HOST_PADS 8                                /* gamepads that can be connected at once (4 Xbox-layout + 4 generic) */
typedef struct HostRawPad {
    unsigned char connected;
    unsigned char generic;                         /* 1: a generic device (numbered buttons, hat, axes); 0: Xbox layout */
    char name[48];                                 /* what the menu shows; also the key of the device's profile in the settings (unique among the connected) */
    unsigned char down[PB_COUNT];                  /* the plain digital inputs that are down (PB_A..PB_DRIGHT; PB_G_BTN*, PB_G_HAT_*) */
    short lx, ly, rx, ry;                          /* sticks, -32768..32767, up and right positive (a generic device: its X and Y axes give lx, ly) */
    unsigned char lt, rt;                          /* triggers 0..255 (Xbox layout) */
    short axis[6];                                 /* generic: the axes X Y Z R U V, relative to their rest position, -32768..32767 */
    unsigned char buttons_total;                   /* generic: how many buttons the device has (information for the menu) */
} HostRawPad;

typedef struct HostRawInput {
    unsigned char key[HK_COUNT];                   /* 1 while the key is down (only while the window has the focus) */
    HostRawPad pad[HOST_PADS];
    unsigned char keyboards;                       /* how many keyboards the system reports (information for the menu; 0 = unknown) */
    unsigned short view_w, view_h;                 /* the window's size in pixels (0 = unknown): the menu is drawn as wide as the window, so it has no black side bars */
    unsigned char console;                         /* 1 on a game console (Switch): the menu names buttons the console's way (L, R, ZL, Minus) and does not talk about keys */
    unsigned char touch;                           /* 1 on a touch-screen device (Android): the menu talks about the Menu button and Back, not about keys */
} HostRawInput;

/* The window layer publishes its latest snapshot here (any thread); the menu and the controllers read it (game thread). Without a window layer
 * the snapshot stays empty, or comes from the test script KYUJO_RAW (host_input.c). */
void host_raw_publish(const HostRawInput *in);
void host_raw_get(HostRawInput *out);

/* All inputs of one pad that are down, thresholds applied (sticks and triggers as buttons): out[PB_*] = 0 or 1. */
void host_pad_resolve(const HostRawPad *p, unsigned char out[PB_COUNT]);

/* ---- N64 buttons and the settings ------------------------------------------------------------------------------------------- */
enum {                                             /* the things a key or pad button can be bound to */
    ACT_A = 0, ACT_B, ACT_Z, ACT_START, ACT_DUP, ACT_DDOWN, ACT_DLEFT, ACT_DRIGHT, ACT_L, ACT_R, ACT_CUP, ACT_CDOWN, ACT_CLEFT, ACT_CRIGHT,
    ACT_STICK_UP, ACT_STICK_DOWN, ACT_STICK_LEFT, ACT_STICK_RIGHT,
    ACT_KEY_COUNT,                                 /* the keyboard has bindings up to here (F1 and Esc open the menu and cannot be changed) */
    ACT_MENU = ACT_KEY_COUNT,                      /* gamepad only: the button that opens the menu */
    ACT_COUNT
};
const char *host_action_name(int action);          /* for the settings file: "a", "b", "z", "start", "dup", ..., "stick_up", "menu" */
const char *host_action_label(int action);         /* for the menu: "A", "B", "Z", "Start", "D-pad up", ..., "Stick up", "Open menu" */
unsigned short host_action_n64_bit(int action);    /* the console's bit for a button action, 0 for the stick directions and the menu button */

#define HS_PATH 512
#define HS_PROFILES 16
/* What the settings remember about one gamepad (found again by its name): which N64 controller it drives and its button assignment. */
typedef struct HostPadProfile {
    char name[48];                                 /* HostRawPad.name; "" = unused entry */
    int generic;                                   /* the device kind it was made for (the bindings use that kind's PB_* numbers) */
    int port;                                      /* 1..4: the N64 controller it drives (0 = none) */
    int deadzone;                                  /* left stick dead zone of this gamepad, percent 0..50 */
    int pad[ACT_COUNT][2];                         /* gamepad inputs (PB_*) for each action, two alternatives; -1 = unused */
} HostPadProfile;

typedef struct HostSettings {
    int cheats;                                    /* 1: the L+R knock-out cheat is on (port patch 0026) */
    int cheat_hof;                                 /* 1: the Hall of Fame is shown complete, all 151 Pokemon (port patch 0039; default 0) */
    int cheat_cpuvs;                               /* 1: in the Kids Club, Ready! can be chosen with nobody playing: computer against computer (port patch 0040; default 0) */
    int widescreen;                                /* 1: the picture is stretched to fill the window (no black bars at the sides); 0: the console's 4:3 shape */
    int pak_real_speed;                            /* 1: Transfer Pak transfers take the console's Joybus time (Game Boy cartridge loading about 27 s); 0 (default): 1/5 of it */
    int scale_style;                               /* picture scaling in the shell's GPU presenter: 0 smooth (default), 1 sharp, 2 firm (sent to the shell with the widescreen flag: shell_set_widescreen(w | style << 1)) */
    int pak_setup_done;                            /* 1: the first-start "Game Pak Setup" page has been shown (it comes once, when no Game Boy cartridge is set up) */
    int gb_fast_cpu;                               /* 1: the Game Boy's processor runs on the shell's native interpreter (shell/core/mips_native.c; faster, experimental on the Switch); 0: the module's own interpreter (default 1; gb_cpu_standard=1 in the settings file) */
    int stick_dpad;                                /* 1: the control stick (left stick / stick keys), tilted past 40 %, also presses the D-pad: the game reads the D-pad only in its menus */
    int fullscreen;                                /* 1: start in full screen (F11 / Alt+Enter / the menu change it at any time) */
    int volume;                                    /* sound volume in percent, 0..100 */
    char rom[HS_PATH];                             /* the game ROM ("" = ask) */
    char save[HS_PATH];                            /* the N64 save file ("" = kyujo.sav in the program's folder) */
    char gb_rom[4][HS_PATH];                       /* the Game Boy cartridge in the Transfer Pak of controller port 1..4 ("" = none) */
    char gb_save[4][HS_PATH];                      /* its save file ("" = the ROM's name with .sav) */
    int key[ACT_COUNT][2];                         /* keyboard keys (HK_*) for each action, two alternatives; HK_NONE = unused */
    int keyboard_port;                             /* 1..4: the N64 controller the keyboard drives (0 = none) */
    HostPadProfile profile[HS_PROFILES];           /* the gamepads seen so far */
    int deadzone;                                  /* the dead zone a newly found gamepad starts with (each gamepad has its own, see HostPadProfile), percent 0..50 */
} HostSettings;

HostSettings *host_settings(void);                                  /* the live settings (created with the defaults on first use) */
void host_settings_defaults(HostSettings *s);
/* Reads/writes the file; the path is kyujo.cfg in the program's folder unless KYUJO_CFG names another. Load returns the number of
 * problems found (unknown keys, bad values: each is logged and skipped), or -1 if there is no file. Save returns 0 on success. */
int host_settings_load(HostSettings *s, const char *path);
int host_settings_save(const HostSettings *s, const char *path);
const char *host_settings_path(void);                               /* where the file is */
/* Text form, for tests: the file's contents. host_settings_parse applies `text` to s and returns the number of problems. */
int host_settings_parse(HostSettings *s, const char *text);
int host_settings_format(const HostSettings *s, char *out, unsigned size);   /* returns the length, or -1 if `out` is too small */

/* Gamepad profiles. The default assignment for a kind of device: */
void host_profile_set_defaults(HostPadProfile *p, int generic);
/* The profile of the connected pad `raw` (same name and kind), or -1. */
int host_profile_find(const HostSettings *s, const HostRawPad *raw);
/* Makes sure every connected pad has a profile (a new one gets the default assignment and the lowest N64 controller that no other connected pad
 * drives). Returns how many profiles were created; the caller saves the settings when it is not 0. */
int host_profiles_sync(HostSettings *s, const HostRawInput *raw);

/* ---- controllers ------------------------------------------------------------------------------------------------------------- */
/* The N64 state of one port from a raw snapshot and the bindings (what host_input_read returns for a connected port). */
void host_input_map(const HostSettings *s, const HostRawInput *raw, int port, unsigned short *buttons, signed char *sx, signed char *sy, int *connected);
/* The N64 button names of the cheat chord for the menu, for example "L+R (keys Q+E)": made from the current bindings. */
void host_cheat_help(const HostSettings *s, char *keys_line, unsigned size);

/* ---- directories (host_paths.c), for the file browser ------------------------------------------------------------------------ */
typedef struct HostDirEntry { char name[256]; int is_dir; } HostDirEntry;
/* Lists `dir` (names only, not "." / ".."), directories first and each group sorted, at most `max` entries. Returns the count, or -1 if the
 * directory cannot be read. An empty `dir` on Windows lists the drives ("C:\"). */
int host_dir_list(const char *dir, HostDirEntry *out, int max);
const char *host_path_separator(void);                              /* "/" or "\" */
/* The folder part of a path (without the file name); out may equal path. */
void host_path_dirname(const char *path, char *out, unsigned size);
/* The parent folder of a folder (out may equal dir); "" means "the list of drives" on Windows. */
void host_path_parent(const char *dir, char *out, unsigned size);
void host_path_with_ext(const char *path, const char *ext, char *out, unsigned size);   /* "a/b.gb" + ".sav" -> "a/b.sav" */
const char *host_path_basename(const char *path);                   /* the file name part (points into `path`) */

#ifdef HOST_SHELL_PICKER
/* Android only: a save file named in the settings must end in .sav or .srm; one that does not (the ROM's name alone, say) is dropped: the automatic name (<rom>.sav) is used instead. */
void host_settings_fix_gb_saves(void);
/* ---- the system's own file picker for Game Boy files (the Android build only: built with -DHOST_SHELL_PICKER; every other build is without this) ------------------ */
/* kind 0: the ROM (the shell stores it in the game's folder), together with its save if the player chose one: the save is stored as <rom>.sav, the name the game looks for
 * by itself. kind 1: a save for the ROM `rom` (the game's path of it), stored under its own name. The picker covers the game: this only starts it. Returns 1 if started,
 * 0 if the shell has no picker (the in-window file browser is the way then). port -1 only asks whether there is a picker. */
int host_pick_gb_start(int port, int kind, const char *rom);
/* 0 while the picker is still open (or none was started); 1 when it is over: `path` is the game's path of the file now in use ("" if nothing was chosen or it failed),
 * `msg` is a short line in words for the player. */
int host_pick_gb_poll(int *port, int *kind, char *path, unsigned path_size, char *msg, unsigned msg_size);
#endif

/* ---- the game's own look (host_gameui.c) ------------------------------------------------------------------------------------ */
/* The menus use the game's font and the background of its Game Pak Check page; both are read from the ROM when first needed. All menu drawing
 * is done on a canvas of HUI_W x HUI_H (the game's hi-res coordinates), which is then scaled onto the picture. */
#define HUI_W 640
#define HUI_H 480
enum { HUI_SIZES = 6 };                            /* the game's letter sizes, by height: 0:10 1:12 2:16 3:20 4:24 5:32 pixels */
/* A canvas is the picture to draw on (rgb: w*h*3 bytes, top row first) and the scale from the 640 x 480 drawing coordinates to its pixels
 * (16.16 fixed point; 0 means 1.0). A drawing coordinate that covers several pixels fills all of them. */
typedef struct HostCanvas { unsigned char *rgb; int w, h; unsigned fx, fy; } HostCanvas;
void host_canvas_blend(HostCanvas *cv, int x, int y, unsigned rgb, unsigned alpha);               /* one pixel; alpha 0..255; outside is ignored */
void host_canvas_fill(HostCanvas *cv, int x, int y, int w, int h, unsigned rgb, unsigned alpha);
int host_gameui_font_ok(int size);                 /* 1 if the font of that size was read from the ROM */
int host_text_width(int size, const char *s);      /* in canvas pixels; the widest line of a text with '\n' */
int host_text_height(int size);
void host_text_draw(HostCanvas *cv, int size, int x, int y, const char *s, unsigned rgb, int shadow);
const unsigned char *host_gameui_background(void); /* the Game Pak Check page background, HUI_W x HUI_H x 3 (RGB), or NULL if it cannot be read */

/* ---- the menu (host_menu.c) -------------------------------------------------------------------------------------------------- */
/* Called by the video output before every picture is handled (also when the picture did not change): looks at the keyboard and pads and opens
 * the menu. While the menu is open this function does not return: it shows the menu (the game's font on the Game Pak Check background) and the
 * game is paused (the game clock stands still, host_clock_pause). */
void host_menu_poll(void);
void host_first_run_setup(void);                      /* every build, called once before the game starts: gives the shell the menu wallpaper and, when no Game Boy cartridge was ever set up, shows the Game Pak Setup page; then lets the graphics set-up begin */
void host_raw_script_tick(void);                                    /* host_input.c: the clock of the test script KYUJO_RAW; called once per poll */
/* A message in the top left corner of the picture for a few seconds (cheat notices, controller connected ...). */
void host_menu_notice(const char *text);
/* Draws the notices (not the menu) onto a picture: called for every picture that is shown. */
void host_menu_overlay(unsigned char *rgb, unsigned w, unsigned h);
int host_menu_overlay_active(void);                    /* are short messages showing (they are drawn into the picture: the direct GPU path cannot show them) */
/* Asks for the menu to open at one of its screens (the Windows menu bar uses it). Any thread; the menu opens at the game's next picture. */
enum { HM_MAIN = 0, HM_PAKS, HM_CONTROLLERS, HM_GAMEROM, HM_OPTIONS, HM_ABOUT, HM_RESTART, HM_PAK1 /* ... HM_PAK1 + 3 = controller 4 */, HM_SETUP = HM_PAK1 + 4 /* the first-start Game Pak Setup page */ };
void host_menu_request(int screen);
int host_menu_is_open(void);                                        /* 1 while the menu is open (and until the keys that closed it are up): the game then sees no button presses */

/* ---- game clock pause (host_sched.c) --------------------------------------------------------------------------------------------- */
/* While the menu is open the game's clock (host_now_ns) stands still, so the game does not see a long gap afterwards. Calls pair up. */
void host_clock_pause(void);
void host_clock_resume(void);
/* Called by the menu loop about every 16 ms while the game is paused: keeps the sound device fed with silence (host_audio.c explains why). */
void host_audio_idle_tick(void);

#endif
