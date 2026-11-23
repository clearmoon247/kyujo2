/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * platform.h - the narrow interface between the game's C code and the host (libc, window system, files).
 *
 * Two kinds of source file exist and must not be mixed:
 *   GAME-side  files include the decomp's / libultra's headers (types such as OSThread). They are compiled without the C
 *              library's headers.
 *   HOST-side  files include the C library (stdio.h ...). They must not include the decomp's headers.
 * The two sides meet only through functions declared in this file, which use plain C types, so neither side's headers can clash
 * with the other's (both define NULL, size_t, bool-like types and more).
 */
#ifndef PLATFORM_H
#define PLATFORM_H

/* Exit status used when the game calls an unimplemented function; see missing.c. */
#define HOST_EXIT_MISSING 70

/* Reports `symbol` as not implemented and ends the program. Never returns. */
void host_missing(const char *symbol) __attribute__((noreturn));

/* Same, for a hardware register the emulation does not implement (name and physical address are printed). */
void host_missing_register(const char *access, unsigned int phys_addr) __attribute__((noreturn));

/* Prints a message to the diagnostic stream (stderr) and returns. printf-style formats are NOT supported here (the game side has no
 * <stdarg.h>); pass a finished string. */
void host_log(const char *message);

/* Development trace: when the environment variable KYUJO_TRACE is set, prints one line "[+<ms>] <what> 0x<a> 0x<b>" to stderr; otherwise
 * does nothing (cheap enough to call from hot paths). Used to see what the emulated hardware and the scheduler do when the game seems stuck. */
void host_trace(const char *what, unsigned int a, unsigned int b);

/* Ends the program with `status` after flushing diagnostics. Used by the self-tests and by the game's own exit paths. */
void host_exit(int status) __attribute__((noreturn));

/* Prints `message` and ends the program with a failure status. Used for "this must never happen" conditions. */
void host_fatal(const char *message) __attribute__((noreturn));

/* ---- byte order of game memory ---------------------------------------------------------------------------------------------
 * Game-side code is compiled so that its memory is BIG-ENDIAN like the console's (tools/native/be_memory.py explains how and why).
 * Consequences for this interface:
 *   - host code must treat any game buffer as plain bytes; to read a multi-byte value out of game memory it must assemble it from
 *     bytes in big-endian order (the renderer and audio code do this when they read display lists and command lists);
 *   - initialised game data is written by the compiler in the host's order, so it is reversed once at start-up:
 */
/* Reverses the multi-byte values of all initialised game data (once; later calls do nothing). Must run before any game code,
 * including self-tests. Returns the number of object files that had data to fix. */
unsigned int host_swap_game_data(void);
void host_fragments_snapshot(void);   /* after host_swap_game_data(): saves the code overlays' initial variables (host_fragment.c) */

/* ---- threads -------------------------------------------------------------------------------------------------------------
 * One N64 thread is run by one host thread, but exactly ONE of them executes game code at any moment: the "baton" is passed
 * explicitly. This makes the program behave like the single-CPU console (no data races inside game code) while still allowing
 * ordinary blocking calls inside the host. See src/ultra/os_sched.c for how libultra's scheduler drives it.
 */
typedef struct HostThread HostThread;

/* The host thread that called main() (the boot context). Valid from the first call on. */
const void *HostCurrentThreadId(void);     /* identity of the running game thread (os_sched.c); only for comparing */
HostThread *host_thread_boot(void);

/* Creates a host thread that starts running entry(arg) only when it is first handed the baton. `stack_bytes` is the host stack size
 * (the stacks the game provides for its N64 threads are not used: native code needs more room than MIPS code). */
HostThread *host_thread_new(void (*entry)(void *), void *arg, unsigned int stack_bytes);

/* Gives the baton to `to` (which must be waiting in host_baton_wait or not yet started). The caller keeps running until it waits. */
void host_baton_pass(HostThread *to);

/* Blocks the caller until another thread passes it the baton. */
void host_baton_wait(HostThread *self);

/* Passes the baton to `to`, then ends the calling host thread for good (used when an N64 thread destroys itself). Never returns.
 * `self` is released. */
void host_thread_end(HostThread *self, HostThread *to) __attribute__((noreturn));

/* Sleeps the calling host thread for about `ns` nanoseconds (may be longer; Windows rounds up to whole milliseconds). */
void host_sleep_ns(unsigned long long ns);

/* ---- time ----------------------------------------------------------------------------------------------------------------- */

/* Monotonic nanoseconds since an arbitrary start. */
/* Counters the stall report (shell/sdl/main_sdl.c, through mips_native_describe) reads straight from core memory; the game side only writes them.
 * [0] SP_STATUS  [1] bit0 audio run on its worker, 1 microcode suspended, 2 graphics busy, 3 virtual yield  [2] SP task starts (audio type) [3] audio runs ended
 * [4] preemption polls  [5] polls that delivered events  [6] sIE | mask << 8  [7] running thread id << 16 | priority  [8] run queue head priority  [9] SP starts of other types
 * [10] number of threads in the table  [16 + n] events of kind n delivered (OS_EVENT_*)  [32 + 3 i ..] thread slot i: id << 16 | priority, state << 16 | low 16 bits of its queue pointer, saved pc */
extern volatile unsigned int host_dbg[112];
unsigned long long host_now_ns(void);                  /* the game's clock: stands still while the menu has the game paused (host_ui.h) */
unsigned long long host_wall_ns(void);                 /* the plain monotonic clock (goes on during a pause) */

/* ---- speed monitor (src/platform/host_perf.c) -------------------------------------------------------------------------------
 * host_perf_add(kind, ns) reports that `ns` nanoseconds of wall-clock time went into one piece of work of that kind; every 5 seconds a
 * summary line is logged. PERF_FIELDS is counted (one call per video retrace the game handles), the others are timed. */
enum { PERF_FIELDS = 0, PERF_GFX, PERF_PICTURE, PERF_AUDIO, PERF_MIPS, PERF_JPEG, PERF_AIGAP, PERF_GFXWAIT, PERF_MIPSYIELD, PERF_MIPSNATIVE, PERF_MIPSKINSTR, PERF_KINDS };
/* PERF_GFXWAIT: one call per time the game had to stand still for the running graphics task (a picture shown from the buffer it draws into, or
 * pool memory freed while the task runs - patch 0025); `ns` = how long. */
/* PERF_MIPSYIELD / PERF_MIPSNATIVE / PERF_MIPSKINSTR: per outermost interpreter call: the time the interpreted code spent switched out at a preemption poll, the time inside native
 * functions it called, and the number of instructions run (thousands, printed as "ms"). Together with PERF_MIPS they show whether a slow Game Boy Tower is CPU, waiting or yielding. */
/* PERF_AIGAP: one call per silent gap of the audio interface (the playing buffer ended before the game supplied the next); `ns` = the gap's length. */
void host_perf_add(int kind, unsigned long long ns);
void host_rdp_trace(int on);               /* development: texture-rectangle trace on/off at run time (src/render/rdp.c, host_control.c) */
/* Development call trace of the game code (host_trace.c; records only in a build made with PORT_EXTRA_CFLAGS=-finstrument-functions). */
int host_trace_start(const char *path, const char *syms_file);   /* 0 = recording; syms_file NULL = the default symbol file */
void host_trace_stop(void);
void host_trace_mark(const char *text);
void host_trace_init(void);                /* KYUJO_TRACE=<file> */
/* Move animation test (host_movetest.c), a developer tool started with KYUJO_MOVETEST (no menu page): any move on any Pokemon, with the call trace written to a log. */
void host_movetest_init(void);             /* KYUJO_MOVETEST=<species>,<move>[,<opponent species>] */
void host_movetest_tick(void);             /* once per picture: carries out the scripted route */
int host_movetest_launch(int species, int move, int foe);       /* restarts the program with the test (developer use; there is no menu page for it) */
const char *host_movetest_species_name(int id);                 /* 1..151 */
const char *host_movetest_move_name(int id);                    /* 1..165 */
void host_perf_start_profiler(void);       /* development: KYUJO_PROFILE=<file> (Linux), see host_perf.c */

/* ---- hardware events -------------------------------------------------------------------------------------------------------
 * Devices (timer, video, audio...) run on their own host threads. They never touch game state; they only raise an event, which the
 * running N64 thread picks up at the next point where interrupts are enabled (os_sched.c). Each event number has ONE pending flag,
 * like a hardware interrupt line: raising it twice before it is handled is handled once.
 */
void host_event_post(int event);
/* Returns the next pending event number (lowest first) and clears it, or -1 if none. */
int host_event_take(void);
/* Percent of the console's Joybus time that a Transfer Pak read or write takes: 100 = as on the console, less = loading a Game Boy cartridge is quicker
 * (menu: Options; the default is 20). Only the waiting time changes, not what is transferred. */
int host_pak_time_percent(void);
/* Blocks until an event is pending or `timeout_ns` has passed (0 = wait forever). Returns 1 if an event is pending. */
int host_event_wait(unsigned long long timeout_ns);
/* One background job at a time on a worker thread (host_sched.c; the graphics worker uses it). host_job_start returns at once; host_job_running
 * says whether the job is still queued or running; host_job_wait blocks until it ended. When it ends the worker also wakes host_event_wait
 * (which then returns once) and raises host_preempt_flag; host_preempt_ack lowers the flag again after the result was taken. */
void host_job_start(void (*fn)(void *), void *arg);
int host_job_running(void);
void host_job_wait(void);
/* A second slot with its own worker, for the RSP audio microcode (same rules as the first). */
void host_job2_start(void (*fn)(void *), void *arg);
int host_job2_running(void);
void host_job2_wait(void);
void host_preempt_ack(void);
void host_job_taken(int slot);             /* the result of the job in slot 0 (graphics) or 1 (audio) was taken; the flag stays up while the other job's result waits */
/* Nonzero if any event is pending (does not clear it). */
int host_event_pending(void);

/* One-shot timer standing in for the CPU's Count/Compare interrupt: after `delay_ns` the event `event` is posted. Re-arming replaces
 * the previous setting; host_timer_disarm cancels it. */
void host_timer_arm(unsigned long long delay_ns, int event);
void host_timer_disarm(void);

/* Posts `event` after `delay_ns` (a device whose work takes time on the console, e.g. an SI transfer). One delayed post per event number
 * can be outstanding; posting again before it is due replaces it. Independent of host_timer_arm. */
void host_event_post_after(unsigned long long delay_ns, int event);

/* ---- video field clock ----------------------------------------------------------------------------------------------------
 * The console's video interface (VI) draws one field every 1/59.94 s (NTSC) and raises an interrupt when its beam reaches a chosen
 * line. The whole game is paced by it. The host runs the same clock: fields of `field_period_ns`, the event posted
 * `intr_offset_ns` after each field starts. The first call starts the clock (field 0 begins at that moment); later calls change the
 * timing from the next field on, without moving the phase. `event` < 0 means "run the clock but raise nothing".
 */
void host_vi_set_timing(unsigned long long field_period_ns, unsigned long long intr_offset_ns, int event);

/* Where the video beam is now: returns (half_line << 1) | (field number & 1), where half_line (0 .. v_sync-1) is the position inside the
 * current field, scaled to `v_sync` half-lines per field (the VI_V_SYNC register). */
unsigned int host_vi_position(unsigned int v_sync);

/* BYTE-ORDER RULE FOR THIS INTERFACE: never pass a pointer to a multi-byte variable that the HOST fills in. Game-side code is compiled to
 * keep ALL its memory big-endian (including its local variables), so the host's native-order store would be read back byte-swapped.
 * Results of this interface are therefore returned by value (or written byte by byte into buffers whose layout the game defines). */

/* ---- Game Boy cartridge in a Transfer Pak (src/platform/host_gbpak.c) ------------------------------------------------------
 * Used by GB Tower / Game Pak Check. The emulated PIF (hw_pif.c) does the joybus framing and calls these for the pak's contents.
 * Bytes are plain bytes (see the byte-order notes above). `channel` is the controller port, 0..3.
 */
/* Puts the Game Boy ROM `rom_path` (and its save RAM file `save_path`, may be NULL or not exist yet) into a Transfer Pak on `channel`.
 * Returns NULL, or a message saying why not (the ROM is checked: header checksum, chip type, save size). */
const char *host_gbpak_load(int channel, const char *rom_path, const char *save_path);
int host_gbpak_present(int channel);              /* 1 if a Transfer Pak with a cartridge is on `channel` */
void host_gbpak_unload(int channel);              /* takes the cartridge out (its save file is written first); nothing happens if there is none */
void host_gbpak_title(int channel, char *out, unsigned size);   /* the cartridge's title from its header, "" if none */
/* The status byte the controller's info command reports: 0x03 the first time (pak newly inserted), then 0x01; 0 without a pak. */
int host_gbpak_info_status(int channel);
int host_gbpak_latched(int channel);              /* 1 while the "newly inserted" report is still to be made */
/* 32-byte read / write at a pak address (already checked and 32-byte aligned by the PIF). */
void host_gbpak_read(int channel, unsigned address, unsigned char *out, int len);
void host_gbpak_write(int channel, unsigned address, const unsigned char *in, int len);
void host_gbpak_trace_cmd(int channel, int command, int arg1, int arg2, int rx);   /* development trace only */
void host_gbpak_trace_reject(int channel, int is_write, unsigned address, int latched, int checksum_ok);   /* development trace only */
void host_error_box(const char *text);                /* Windows: shows the text in a message box (a console window would vanish at exit); elsewhere nothing */

/* The program's own folder (src/platform/host_paths.c): the config file, the log and the default save live there. KYUJO_HOME replaces it.
 * host_app_path joins a name to it (a name that is already absolute is copied as it is) and returns `out`. */
const char *host_app_dir(void);
const char *host_app_path(char *out, unsigned size, const char *name);
/* Starts a second copy of the program with the same arguments (but not the ones that name a Game Boy cartridge) and ends this one. Returns 0
 * only when the new copy could not be started (then this one keeps running). host_set_program_args is called once by main. */
void host_set_program_args(int argc, char **argv);
int host_restart(void);
void host_game_range_bounds(unsigned int *lo, unsigned int *hi);   /* the image's address range [lo, hi), for hot paths that compare inline (host_range.c) */
int host_game_range_ok(unsigned int addr, unsigned int len);   /* 1 when [addr, addr+len) lies inside the program image, i.e. is memory the game can really have;
                                                       * 0 for a garbage address. Host code that follows a pointer taken from game data (renderer, video,
                                                       * RSP DMA) asks this first, so a bad pointer in the game's data is reported instead of crashing. */
void host_run_exit_hook(void);                      /* runs that function now (host_exit() does it itself) */
void host_set_exit_hook(void (*fn)(void));          /* a function that host_exit() calls before ending the program (the Transfer Pak registers its save flush) */
void host_gbpak_flush(void);                      /* writes changed save RAM to its file (also done at exit and when the game powers the pak off) */

/* ---- save data -------------------------------------------------------------------------------------------------------------
 * The cartridge's save memory (flash chip) is kept in one file. It is read when the game starts and rewritten after every change.
 */
/* Sets the file used for save data (default "kyujo.sav" in the current directory). */
void host_save_set_path(const char *path);
/* The file in use (the default, or what host_save_set_path set). */
const char *host_save_path(void);
/* 1 if a save file can be created at `path` (its folder exists and is writable). */
int host_save_can_write(const char *path);
/* Fills `dst` (size bytes) from the save file. Returns the number of bytes found (0 when there is no file yet; the caller keeps its own
 * "blank" contents). A file of the wrong size is ignored with a message on stderr, never half-used. */
unsigned int host_save_load(void *dst, unsigned int size);
/* Replaces the save file with `size` bytes from `src`: written to a temporary file first, then moved over the old one, so a crash
 * cannot leave a half-written save. Failure is reported on stderr (the game keeps running). */
void host_save_store(const void *src, unsigned int size);
/* The store above is write-behind: the file is written when the chip has been quiet for a moment (host_save_poll, once per field), when the file in use changes, at exit
 * and restart (the exit hook calls host_save_flush) and whenever a caller needs the file to be current. */
void host_save_flush(void);
void host_save_poll(void);

/* ---- code overlays ("fragments") -------------------------------------------------------------------------------------------
 * Every indirect call in game code is compiled to call this first (tools/native/be_memory.py, CALL_HOOK). It returns `target` unchanged
 * unless `target` points into a code overlay that the game has loaded into its memory pool; then it returns the native function that
 * the overlay's code was compiled into. Stops the program if a call into the pool cannot be explained. See host_fragment.c. */
void *HostResolveCallTarget(void *target);

/* Returns a native function that, when called with up to four integer arguments, runs the MIPS machine code found at the pool address
 * `pool_addr` with a minimal interpreter and returns its v0. Used for fragments that are raw binary files with no C source; see
 * host_mips_stub.c. The interpreter stops the program on any instruction it does not support. */
void *host_mips_thunk(unsigned int pool_addr);

/* ---- the MIPS interpreter for hand-written assembly (src/platform/host_mips.c) ---------------------------------------------
 * Runs the machine code at `entry` (a native address inside the memory pool, in a fragment the game has loaded) with the MIPS o32 calling
 * convention for up to four integer arguments, and returns v0. Calls from that code into a fragment's C functions run the native
 * functions (with the same four arguments); calls into other machine code are interpreted.
 */
unsigned int host_mips_call(unsigned int entry, unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3);
void host_mips_trace_next(int n);        /* development: print the next n interpreted instructions */
void host_mips_flush(void);              /* the code in the pool may have changed (the GB Tower emulator is being set up): forget the decoded blocks */
/* Pool address of code the console linked at `link_addr` (like 0x81209870) in the newest loaded fragment that covers it, or 0. */
unsigned int host_fragment_pool_addr(unsigned int link_addr);
/* The native function standing for the C code at `pool_addr`, or NULL (not a fragment function, e.g. hand-written assembly). */
void *host_fragment_native(unsigned int pool_addr);
unsigned int host_fragment_link_addr(unsigned int pool_addr);     /* the console's (0x81xxxxxx) address of a loaded fragment byte, or 0; for messages */

/* ---- controllers -----------------------------------------------------------------------------------------------------------
 * The emulated PIF (src/ultra/hw_pif.c) asks the host for the state of each of the four controller ports every time the game polls.
 * Returns -1 if no controller is connected on `channel` (0..3); otherwise the state packed in one value:
 *   bits 31..16 = buttons, bits 15..8 = stick_x, bits 7..0 = stick_y (the two sticks as unsigned bytes of the signed values).
 *   buttons : the console's bit layout, one bit per button (lib/ultralib/include/PR/os_cont.h: CONT_A 0x8000, CONT_B 0x4000,
 *             CONT_G (Z) 0x2000, CONT_START 0x1000, CONT_UP 0x0800, DOWN 0x0400, LEFT 0x0200, RIGHT 0x0100, L 0x0020, R 0x0010,
 *             C-up 0x0008, C-down 0x0004, C-left 0x0002, C-right 0x0001)
 *   stick_x, stick_y : signed -128..127, right and up positive
 */
int host_input_read(int channel);
/* The development command reader (host_control.c) sets the state of port 1 here (same bit layout and ranges as above); it is added to what the
 * keyboard and gamepads give. Not used by the window code, which publishes the raw devices (host_ui.h: host_raw_publish). */
void host_input_set_live(unsigned short buttons, signed char stick_x, signed char stick_y);

/* Development aid (host_control.c): starts the command reader when KYUJO_CONTROL is set. A pending screenshot request is taken by the
 * video output (host_vi_commit), which reports the outcome with host_control_shot_done. */
void host_control_start(void);
const char *host_control_take_shot(char *buf, unsigned int size);
void host_control_shot_done(int ok);

/* ---- graphics ---------------------------------------------------------------------------------------------------------------
 * The RSP's graphics microcode and the RDP are done by the host (src/render/): the game-side hardware model hands over the address of a
 * display list and, when the video interface is reprogrammed, its registers. Display lists and frame buffers are read and written as
 * plain big-endian bytes at the addresses the game uses (native pointer == physical address, see the memory model notes above). */
/* Runs the display list `dl_addr` (segmented or physical address) as one graphics task. Returns the number of RDP full syncs it held. */
unsigned int host_gfx_task(unsigned int dl_addr);
/* The same task, run in pieces (src/render/gfx_host.c). host_gfx_task_begin prepares it without running it. host_gfx_task_pump runs at most
 * `budget` commands (0 = no limit) and stops before the next command when `*yield_req` is nonzero. It adds the RDP full syncs it ran to *syncs
 * and returns HOST_GFX_DONE (the list ended), HOST_GFX_PAUSED (the budget ran out) or HOST_GFX_YIELDED (the yield request was taken). */
#define HOST_GFX_DONE    0
#define HOST_GFX_PAUSED  1
#define HOST_GFX_YIELDED 2
void host_gfx_task_begin(unsigned int dl_addr);
/* The same task on a worker thread (src/render/gfx_host.c), so that the game carries on meanwhile as it does on the console. After
 * host_gfx_task_begin (or to continue a yielded task) host_gfx_task_run_async starts it; host_gfx_task_collect then returns HOST_GFX_PAUSED while
 * it still runs (wait == 0), or waits (wait != 0), and otherwise HOST_GFX_DONE / HOST_GFX_YIELDED with the full syncs added to *syncs.
 * host_gfx_async_enabled is false when KYUJO_GFX_THREAD=0 (then hw_sp.c uses the sliced, single-threaded path). */
int host_gfx_async_enabled(void);
void host_gfx_task_run_async(const volatile int *yield_req);
int host_gfx_task_collect(int wait, unsigned int *syncs);
int host_gfx_task_pump(unsigned int budget, const volatile int *yield_req, unsigned int *syncs);
/* The graphics task the RSP is running (src/ultra/hw_sp.c) is advanced in slices: HostHwGfxPump runs up to `budget` commands of it,
 * HostHwGfxBusy says whether one is running, HostHwGfxComplete finishes it (used before a field is shown). */
void HostHwGfxPump(unsigned int budget);
/* The audio microcode runs on its own worker thread (hw_sp.c): HostHwRspPump reports its end (SP_STATUS, interrupt) if it has ended, without waiting; HostHwRspBusy says whether
 * one is running or waits to be reported. */
void HostHwRspPump(void);
int HostHwRspBusy(void);
void HostHwRspSettle(void);
void HostTraceEv(const char *tag, unsigned v);                      /* waits for the audio run on the worker to end and reports its end (SP interrupt) now */
unsigned HostHwRspJobSeq(void);                  /* trace (hw_ai.c): audio microcode runs started, collected, and how long ago the last one started / ended */
unsigned HostHwRspJobsDone(void);
unsigned long long HostHwRspSinceStartNs(void);
unsigned long long HostHwRspSinceEndNs(void);
int host_audio_trace_enabled(void);              /* KYUJO_AUDIO_TRACE=1 */
int HostHwGfxBusy(void);
void HostHwGfxComplete(void);
void HostHwGfxCompleteFor(unsigned int origin);     /* before the picture in the frame buffer at `origin` is shown: finishes the task only if it draws there */
int host_gfx_task_may_draw_to(unsigned int origin); /* (gfx_host.c) the running graphics task has selected a colour image that starts at `origin` (or the answer is unknown) */
/* Nonzero when graphics may run in idle time. KYUJO_GFX_NO_IDLE=1 turns that off (development: the graphics task then stays
 * running until the retrace, which exercises the game's yield to audio). */
int host_gfx_idle_slices(void);
/* Runs the game's JPEG microcode task whose 0x20-byte parameter block is at `data_addr`: converts coefficient blocks to pixels in place. */
unsigned int host_jpeg_task(unsigned int data_addr);

/* ---- the RSP interpreter (src/rsp/rsp_cpu.c) -------------------------------------------------------------------------------
 * Runs microcode on the RSP's memories (the byte arrays of src/ultra/hw_sp.c). Values are passed and returned BY VALUE (see the byte-order
 * note above): host_rsp_start gives the interpreter the memories and the SP registers it needs (PC as an IMEM offset, SP_MEM_ADDR,
 * SP_DRAM_ADDR, SP_STATUS, the semaphore, DPC_STATUS (read-only for the microcode)); host_rsp_run executes until BREAK and returns the number of instructions; host_rsp_get reads
 * back: 0 SP_MEM_ADDR, 1 SP_DRAM_ADDR, 2 SP_STATUS, 3 semaphore, 4 PC, 5 interrupt requests (bit 0 acknowledge, bit 1 raise). */
void host_rsp_start(unsigned char *dmem, unsigned char *imem, unsigned int pc, unsigned int mem_addr, unsigned int dram_addr,
                    unsigned int status, unsigned int semaphore, unsigned int dpc_status);
unsigned int host_rsp_run(void);
/* The same run on the second worker thread: host_rsp_run_async starts it and returns; host_rsp_async_running says whether it is still going, host_rsp_async_wait waits for its end;
 * host_rsp_async_result returns the instruction count, host_rsp_async_ns the run's time. The interpreter must not be used from anywhere else in between. host_audio_async_enabled is
 * false when KYUJO_AUDIO_THREAD=0 (the audio microcode then runs inside the SP_STATUS write that starts it, as before). */
void host_rsp_run_async(void);
int host_rsp_async_running(void);
void host_rsp_async_wait(void);
unsigned int host_rsp_async_result(void);
unsigned long long host_rsp_async_ns(void);
int host_audio_async_enabled(void);
void host_rsp_resume(unsigned int status, unsigned int semaphore, unsigned int dpc_status);   /* continue a suspended task (host_rsp_get(6) != 0), then host_rsp_run */
unsigned int host_rsp_get(unsigned int which);          /* ... 6: 1 = the task was suspended while waiting for the CPU */
/* ---- window (src/platform/host_window.c on Windows, host_window_linux.c on Linux; does nothing elsewhere) ----------------------
 * Shows one finished picture (8-bit RGB, `w` x `h`, top row first). Called from the game thread for every new frame; returns quickly. */
void host_window_present(const unsigned char *rgb, unsigned int w, unsigned int h);
/* 1 if there is a window to show pictures in (Windows always; Linux when an X display could be opened; the first call opens it), else 0:
 * the video output skips the conversion work for the window when it is 0. */
int host_window_wanted(void);
/* Early in start-up: Windows creates the window (and its message loop and input thread) now, not at the first picture, because creating it takes tens of milliseconds
 * and the first picture is made while the game's first sound is already playing (the "Pika" voice stutter, Windows log of 2026-11-14). Other systems: nothing. */
void host_window_prepare(void);
/* Windows: opens the standard file dialog and returns `buf` (filled with the chosen path), or NULL if cancelled; elsewhere NULL. */
const char *host_window_pick_rom(char *buf, unsigned int size);
/* Full screen (a borderless window over the whole monitor) on or off, and whether it is on. Windows; elsewhere nothing happens / 0. Any thread. */
void host_window_set_fullscreen(int on);
int host_window_is_fullscreen(void);
/* The shell build (Switch, Android): tells the shell, when it changes, whether the picture fills the window (the Widescreen option) or keeps its 4:3 shape. Called
 * once per picture by the game; elsewhere (Windows, Linux window layers) the window layer reads the setting itself and this does nothing. */
#if defined(__wasm__)
void host_window_sync_view(void);
/* The shell build: the game's first-start page (host_first_run_setup) comes before the shaders are prepared and the game starts. host_window_set_backdrop gives the shell
 * the game's menu wallpaper tile (RGB, w x h; copied at once) for its start-up picture; host_window_graphics_go says the page is over: the shell may prepare its shaders
 * (behind the start-up picture) and the game starts. */
void host_window_set_backdrop(const unsigned char *rgb, unsigned int w, unsigned int h);
void host_window_graphics_go(void);
#else
#define host_window_sync_view() ((void)0)
#define host_window_set_backdrop(rgb, w, h) ((void)0)
#define host_window_graphics_go() ((void)0)
#endif
/* The audio interface starts playing a buffer: `addr`/`len` locate big-endian 16-bit stereo samples in the game's memory, `rate` is the
 * sample rate in Hz (src/platform/host_audio.c). */
void host_audio_buffer(unsigned int addr, unsigned int len, unsigned int rate);
void host_audio_prepare(void);   /* early in start-up: Windows opens the sound device on a short thread, so the game's first sound does not wait for it (other systems: nothing) */
/* Number of distinct frames the video output has shown so far (the numbering the development frame dump uses). */
unsigned int host_gfx_frame_count(void);
/* The game has finished programming the video interface for the next field (libultra writes VI_CONTROL last). `h_video` / `v_video` are
 * the VI_H_START / VI_V_START registers (named H_VIDEO / V_VIDEO in n64brew). */
void host_vi_commit(unsigned int control, unsigned int origin, unsigned int width, unsigned int h_video, unsigned int v_video,
                    unsigned int x_scale, unsigned int y_scale);

/* ---- cartridge ROM ---------------------------------------------------------------------------------------------------------
 * The game's data (textures, models, audio, text, code overlays) is read from the player's own ROM file at run time. The ROM is never
 * part of this repository or of any build output. The image is kept in memory in the byte order of the .z64 format (big-endian),
 * exactly as the cartridge presents it, so a device register read of ROM space returns the same 32-bit value as on hardware.
 */

/* Loads and checks the ROM at `path` (.z64, .v64 or .n64 byte order; the file is converted to .z64 order in memory). The SHA-1 of
 * the converted image must equal the US release recorded by the decomp (yamls/us/header.yaml). Returns NULL on success, otherwise a
 * message saying what is wrong (never a partial result). */
const char *host_rom_load(const char *path);
/* The same checks without loading it (the menu uses this on a ROM the player picked): NULL if it is the supported ROM, else the reason. */
const char *host_rom_check(const char *path);

/* The loaded image, and its size in bytes. Valid after a successful host_rom_load. */
const unsigned char *host_rom_data(void);
unsigned int host_rom_size(void);

/* Fills the program's variables that stand for ROM contents (the main program's initialised data and the cartridge-window words the
 * game reads). Defined in the GENERATED stubs.c, because which variables exist depends on what the decomp still lacks. Call it once
 * after host_rom_load succeeded and before the game starts. */
void host_init_rom_data(void);

/* Copies `len` bytes starting at ROM offset `offset` to `dst`. Bytes are copied as they are in the ROM file (big-endian data stays
 * big-endian: converting data to host byte order is the job of whoever knows what the data is). Stops the program if the range
 * lies outside the ROM. */
void host_copy_from_rom(unsigned int offset, void *dst, unsigned int len);

#endif /* PLATFORM_H */
