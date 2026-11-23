/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * host_paths.c - where the program's own files live (config file, log, saves), see platform.h (host_app_dir).
 *
 * The directory that holds the program itself (the folder of kyujo.exe on Windows; the folder of the executable on Linux). Settings,
 * the log and the default save file are kept there, so that the program can be copied to another place as one folder. The environment
 * variable KYUJO_HOME replaces it (development, tests, and systems where the program folder is not writable).
 * HOST-side file: may use the C library and the Windows API.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

const char *host_app_dir(void)
{
    static char dir[1024];
    const char *env;
    if (dir[0]) return dir;
    env = getenv("KYUJO_HOME");
    if (env && env[0]) {
        snprintf(dir, sizeof dir, "%s", env);
    } else {
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)sizeof dir);
        if (n == 0 || n >= sizeof dir) dir[0] = 0;
#elif defined(__wasm__)
        dir[0] = 0;       /* the shell sets KYUJO_HOME to the folder it opened for the game (the SD card folder / the app's folder) */
#else
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        if (n > 0) dir[n] = 0; else dir[0] = 0;
#endif
        if (dir[0]) {                                           /* cut the file name off */
            char *slash = strrchr(dir, '/');
            char *bslash = strrchr(dir, '\\');
            if (bslash && (!slash || bslash > slash)) slash = bslash;
            if (slash) *slash = 0; else dir[0] = 0;
        }
    }
    if (!dir[0]) snprintf(dir, sizeof dir, ".");               /* unknown: the current folder */
    return dir;
}

/* dir + "/" + name into `out`; returns out. A name that is already absolute (a drive letter, or a leading slash) is copied as it is. */
const char *host_app_path(char *out, unsigned size, const char *name)
{
    int absolute = name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':');
    if (absolute) snprintf(out, size, "%s", name);
    else snprintf(out, size, "%s/%s", host_app_dir(), name);
    return out;
}

/* ---- directories, for the file browser of the menu ---- */
#include <ctype.h>
#include "host_ui.h"
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

const char *host_path_separator(void)
{
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

void host_path_dirname(const char *path, char *out, unsigned size)
{
    char tmp[1100];
    char *cut = NULL, *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp; *p; p++) if (is_sep(*p)) cut = p;
    if (!cut) tmp[0] = 0;
    else if (cut == tmp) tmp[1] = 0;                                 /* "/file": the folder is the root */
    else *cut = 0;
    snprintf(out, size, "%s", tmp);
}

void host_path_parent(const char *dir, char *out, unsigned size)
{
    char tmp[1100];
    size_t n;
    snprintf(tmp, sizeof tmp, "%s", dir);
    n = strlen(tmp);
    while (n > 1 && is_sep(tmp[n - 1]) && !(n == 3 && tmp[1] == ':')) tmp[--n] = 0;      /* no trailing separator, except "C:\" and "/" */
#ifdef _WIN32
    if (n <= 3 && tmp[1] == ':') { out[0] = 0; return; }              /* a drive root: the parent is the list of drives */
#else
    if (n <= 1) { snprintf(out, size, "/"); return; }
#endif
    {
        char *cut = NULL, *p;
        for (p = tmp; *p; p++) if (is_sep(*p)) cut = p;
        if (!cut) { snprintf(out, size, "%s", ""); return; }
        if (cut == tmp) tmp[1] = 0; else *cut = 0;
#ifdef _WIN32
        if (strlen(tmp) == 2 && tmp[1] == ':') { tmp[2] = '\\'; tmp[3] = 0; }       /* "C:" -> "C:\" */
#endif
    }
    snprintf(out, size, "%s", tmp);
}

static int entry_cmp(const void *a, const void *b)
{
    const HostDirEntry *x = (const HostDirEntry *)a, *y = (const HostDirEntry *)b;
    const char *p = x->name, *q = y->name;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;      /* directories first */
    while (*p && *q) {
        int c = tolower((unsigned char)*p) - tolower((unsigned char)*q);
        if (c) return c;
        p++; q++;
    }
    return tolower((unsigned char)*p) - tolower((unsigned char)*q);
}

int host_dir_list(const char *dir, HostDirEntry *out, int max)
{
    int n = 0;
#ifdef _WIN32
    if (dir[0] == 0) {                                                /* the drives */
        DWORD mask = GetLogicalDrives();
        int d;
        for (d = 0; d < 26 && n < max; d++) {
            if (mask & (1u << d)) { snprintf(out[n].name, sizeof out[n].name, "%c:\\", 'A' + d); out[n].is_dir = 1; n++; }
        }
        return n;
    }
    {
        char pattern[1100];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        size_t l = strlen(dir);
        snprintf(pattern, sizeof pattern, "%s%s*", dir, (l && is_sep(dir[l - 1])) ? "" : "\\");
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
            if (n < max) {
                snprintf(out[n].name, sizeof out[n].name, "%s", fd.cFileName);
                out[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                n++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir[0] ? dir : "/");
        struct dirent *e;
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char full[2200];
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || e->d_name[0] == '.') continue;   /* hidden files stay hidden */
            if (n >= max) break;
            snprintf(full, sizeof full, "%s/%s", dir[0] ? dir : "", e->d_name);
            snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
            out[n].is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            n++;
        }
        closedir(d);
    }
#endif
    qsort(out, (size_t)n, sizeof out[0], entry_cmp);
    return n;
}

/* `path` with its extension (if any) replaced by `ext` (".sav"); out may equal path. Used for the default save of a Game Boy cartridge. */
void host_path_with_ext(const char *path, const char *ext, char *out, unsigned size)
{
    char tmp[1100], *dot, *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    dot = strrchr(tmp, '.');
    for (p = tmp; *p; p++) if (is_sep(*p) && dot && p > dot) dot = NULL;      /* a dot in a folder name is not an extension */
    if (dot) *dot = 0;
    snprintf(out, size, "%s%s", tmp, ext);
}

/* The file name part of a path (points into `path`). */
const char *host_path_basename(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (is_sep(*p)) b = p + 1;
    return b;
}

/* ---- starting the program again, for "Restart the game" in the menu ---------------------------------------------------------------- */

/* The game looks for the Game Boy cartridges only once, when it starts (GbSave_ScanAllPorts, called from the n64_logo fragment), so a cartridge
 * that is put in through the menu is seen after a restart. The new copy gets the arguments of this one, except the two that name a Game Boy
 * cartridge (--gb-rom, --gb-save): a command line choice wins over the settings file for one run only, and the restart exists to
 * use what the menu has just stored there. The same goes for the environment variables of the same meaning and for the development key
 * script KYUJO_RAW (it describes one run). */
static int sArgc;
static char **sArgv;

void host_set_program_args(int argc, char **argv) { sArgc = argc; sArgv = argv; }

static int skipped_arg(int i)                              /* 1 for --gb-rom / --gb-save and for the value that follows them */
{
    if (i > 1 && (strcmp(sArgv[i - 1], "--gb-rom") == 0 || strcmp(sArgv[i - 1], "--gb-save") == 0)) return 1;
    return strcmp(sArgv[i], "--gb-rom") == 0 || strcmp(sArgv[i], "--gb-save") == 0;
}

#ifdef _WIN32
/* Appends one argument to a command line, quoted the way CommandLineToArgvW / the C runtime read it back. */
static void append_quoted(char *cmd, size_t size, const char *arg)
{
    size_t n = strlen(cmd);
    int backslashes = 0;
    if (n + 3 >= size) return;
    cmd[n++] = ' ';
    cmd[n++] = '"';
    for (; *arg && n + 4 < size; arg++) {
        if (*arg == '\\') { backslashes++; cmd[n++] = '\\'; continue; }
        if (*arg == '"') { while (backslashes-- > 0 && n + 4 < size) cmd[n++] = '\\'; cmd[n++] = '\\'; }
        backslashes = 0;
        cmd[n++] = *arg;
    }
    while (backslashes-- > 0 && n + 3 < size) cmd[n++] = '\\';       /* backslashes before the closing quote are doubled */
    cmd[n++] = '"';
    cmd[n] = 0;
}
#endif

int host_restart(void)
{
    int i;
    if (sArgc < 1 || !sArgv) return 0;
#ifdef _WIN32
    {
        char exe[1024], cmd[8192];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (n == 0 || n >= sizeof exe) return 0;
        _putenv("KYUJO_GB_ROM=");                    /* an empty value removes the variable */
        _putenv("KYUJO_GB_SAVE=");
        _putenv("KYUJO_RAW=");
        cmd[0] = 0;
        append_quoted(cmd, sizeof cmd, exe);
        for (i = 1; i < sArgc; i++) if (!skipped_arg(i)) append_quoted(cmd, sizeof cmd, sArgv[i]);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        host_run_exit_hook();                              /* what the old copy has written (cartridge saves) is on disk before the new one reads it */
        if (!CreateProcessA(exe, cmd + 1, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
        /* Windows lets only the program that has the keyboard focus bring a window to the front. This copy has it (the player just used its menu),
         * so it hands the right to the new copy; the new copy's window then asks for the front when it opens (host_window.c). Without this the new
         * window opened behind the other windows. */
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#elif defined(__wasm__)
    {
        /* A program cannot start another copy of itself here; the shell does it (it runs the whole core again). */
        __attribute__((import_module("shell"), import_name("restart"))) extern int shell_restart(void);
        host_run_exit_hook();
        return shell_restart();
    }
#else
    {
        char exe[1024];
        char **args = (char **)calloc((size_t)sArgc + 1, sizeof *args);
        int k = 0;
        long fd, maxfd;
        pid_t pid;
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (!args || n <= 0) { free(args); return 0; }
        exe[n] = 0;
        args[k++] = exe;
        for (i = 1; i < sArgc; i++) if (!skipped_arg(i)) args[k++] = sArgv[i];
        args[k] = NULL;
        unsetenv("KYUJO_GB_ROM");
        unsetenv("KYUJO_GB_SAVE");
        unsetenv("KYUJO_RAW");
        maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        host_run_exit_hook();
        fflush(stderr);
        pid = fork();
        if (pid < 0) { free(args); return 0; }
        if (pid == 0) {                                    /* the new copy: only the three standard descriptors, so that this copy's window and sound devices close */
            for (fd = 3; fd < maxfd; fd++) close((int)fd);
            execv(exe, args);
            _exit(127);
        }
    }
#endif
    host_exit(0);                                          /* ends this copy at once; the window goes away with it */
}

#ifdef HOST_SHELL_PICKER
void host_settings_fix_gb_saves(void)
{
    HostSettings *s = host_settings();
    int i;
    for (i = 0; i < 4; i++) {
        const char *b, *dot;
        if (!s->gb_save[i][0]) continue;
        b = host_path_basename(s->gb_save[i]);
        dot = strrchr(b, '.');
        if (dot && strlen(dot) == 4 && (dot[1] | 32) == 's' && ((dot[2] | 32) == 'a' ? (dot[3] | 32) == 'v' : ((dot[2] | 32) == 'r' && (dot[3] | 32) == 'm'))) continue;
        fprintf(stderr, "kyujo: Game Boy save '%s' does not end in .sav or .srm: the automatic name is used\n", s->gb_save[i]);
        s->gb_save[i][0] = 0;
    }
}
#endif
