#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "desktop.hpp"
#include "region.hpp"
#include "kyujo2_icon.h"
#ifdef KYUJO2_SHARED_MENU
#include "shared_menu.hpp"
extern "C" {
#include "platform.h"
#include "host_ui.h"
int kyujo2_pak_present(int);
}
#endif
#ifdef __ANDROID__
#include "android_touch.hpp"
#endif
#ifdef KYUJO2_WINDOWS_UI
#include <windows.h>
#endif
#ifdef __SWITCH__
#include "ultramodern/ultramodern.hpp"
#include "librecomp/mods.hpp"
#include <switch.h>
extern std::unique_ptr<recomp::mods::ModContext> mod_context;
extern std::mutex mod_context_mutex;
#endif
extern "C" {
#include "gfx.h"
#include "rdp.h"
uint8_t *gRdpMem;
uint32_t gRdpMemSize = 8u << 20;
#ifdef KYUJO2_GPU
#include "gpu_gles.h"
void gles_set_overlay(const unsigned char *rgba, unsigned w, unsigned h);   // patches/gpu_gles-kyujo2.patch
#endif
void host_game_range_bounds(unsigned *lo, unsigned *hi) { *lo = 0; *hi = 8u << 20; }
int host_game_range_ok(unsigned addr, unsigned len) {
    return len <= (8u << 20) && addr <= (8u << 20) - len;
}
unsigned long long host_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

namespace {
SDL_Window *window;
SDL_AudioDeviceID audio_device;
std::mutex audio_mutex, frame_mutex;
std::vector<uint8_t> published_frame;
unsigned frame_width, frame_height, published_count;
std::atomic<uint16_t> buttons;
std::atomic<float> stick_x, stick_y;
SDL_GameController *controller;
namespace graphics = ultramodern::renderer;

// GPU mode: the renderer's OpenGL ES 3 back end draws and shows the pictures on its own thread, which owns the GL
// context. Without it (or if no context can be made) the software renderer draws and SDL's window surface shows.
bool gpu_mode;
#ifdef KYUJO2_GPU
SDL_GLContext gl_context;
int gl_make_current(void *) { return SDL_GL_MakeCurrent(window, gl_context); }
void gl_release(void *) { SDL_GL_MakeCurrent(window, nullptr); }
void gl_swap(void *) { SDL_GL_SwapWindow(window); }
void gl_size(void *, int *w, int *h) { SDL_GL_GetDrawableSize(window, w, h); }
bool gpu_wanted() {
    if (const char *v = std::getenv("KYUJO2_RENDERER")) return std::strcmp(v, "software") != 0;
#ifdef __SWITCH__
    // A file named "software-renderer" in the Kyujo2 folder selects the software renderer.
    std::error_code ec;
    if (std::filesystem::exists("sdmc:/switch/Kyujo2/software-renderer", ec)) return false;
#endif
    return true;
}
// The menu's latest picture. While the menu is open the graphics thread shows it on every frame in place of the game's
// picture: presenting it only when it changes let a game picture that was already on its way replace it for good.
std::mutex menu_picture_mutex;
std::vector<uint8_t> menu_picture;
unsigned menu_picture_w, menu_picture_h;
// Notices ("Cheats on", "Gamepad connected") over a picture shown straight from the GPU: drawn by the menu code into
// an RGB picture as usual, once over black and once over white, which gives each pixel's coverage, then handed to the
// back end as a premultiplied RGBA layer. Rebuilt a few times a second while a notice shows.
bool notice_layer_on;
void notice_layer_off() {
    if (!notice_layer_on) return;
    gles_set_overlay(nullptr, 0, 0);
    notice_layer_on = false;
}
void notice_layer_update(const ::ViRegs &r) {
#ifdef KYUJO2_SHARED_MENU
    static unsigned frames;
    static unsigned last_w, last_h;
    if (!host_menu_overlay_active()) { notice_layer_off(); return; }
    // The picture size, as vi_present works it out.
    const int h_start = (r.h_video >> 16) & 0x3FF, h_end = r.h_video & 0x3FF;
    const int v_start = (r.v_video >> 16) & 0x3FF, v_end = r.v_video & 0x3FF;
    const unsigned xs = r.x_scale & 0xFFF, ys = r.y_scale & 0xFFF;
    if (xs == 0 || ys == 0 || h_end <= h_start || v_end <= v_start) { notice_layer_off(); return; }
    const unsigned w = (unsigned)((h_end - h_start) * (int)xs / 1024), h = (unsigned)(((v_end - v_start) >> 1) * (int)ys / 1024);
    if (w == 0 || h == 0 || w > 1024 || h > 768) { notice_layer_off(); return; }
    if (notice_layer_on && w == last_w && h == last_h && ++frames % 8) return;
    frames = 0;
    last_w = w;
    last_h = h;
    static std::vector<uint8_t> black, white, rgba;
    const size_t n = (size_t)w * h;
    black.assign(n * 3, 0);
    white.assign(n * 3, 255);
    rgba.resize(n * 4);
    host_menu_overlay(black.data(), w, h);
    host_menu_overlay(white.data(), w, h);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *b = &black[i * 3], *wt = &white[i * 3];
        const int clear = (wt[0] - b[0] + wt[1] - b[1] + wt[2] - b[2]) / 3;   // 255: untouched, 0: opaque
        rgba[i * 4 + 0] = b[0];
        rgba[i * 4 + 1] = b[1];
        rgba[i * 4 + 2] = b[2];
        rgba[i * 4 + 3] = (uint8_t)std::clamp(255 - clear, 0, 255);
    }
    gles_set_overlay(rgba.data(), w, h);
    notice_layer_on = true;
#else
    (void)r;
#endif
}
// The picture settings of the menu, applied to the GPU's presentation.
void gpu_picture_settings() {
#ifdef KYUJO2_SHARED_MENU
    gles_set_stretch(shared_menu::wide() ? 1 : 0);
    gles_set_filter(shared_menu::scaling());
#endif
}
#endif

class SoftwareRenderer : public graphics::RendererContext {
    std::vector<uint8_t> rgb = std::vector<uint8_t>(1024 * 768 * 3);
public:
    explicit SoftwareRenderer(uint8_t *memory) {
        gRdpMem = memory;
        rdp_reset();
    }
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dl(const OSTask *task) override {
        const auto t0 = host_now_ns();
        gfx_reset_rsp();
        rdp_cimg_log_reset();
        gfx_run_display_list(task->t.data_ptr & 0x00ffffff);
        dl_ns += host_now_ns() - t0;
        ++dl_count;
    }
    unsigned long long dl_ns = 0, dl_count = 0;
    void send_dummy_workload(uint32_t) override {}
#ifdef KYUJO2_GPU
    // GPU mode, every 2 s (always on the Switch, KYUJO2_PERF=1 elsewhere): the game's picture rate and where the time
    // went, so a slow scene in a player's log shows whether the game, the graphics thread or the GPU is behind.
    uint32_t last_origin = 0;
    unsigned long long perf_t0 = 0, new_pictures = 0;
    GlesStats perf_last{};
    void perf_line() {
#ifdef __SWITCH__
        static const bool on = gpu_mode;
#else
        static const bool on = gpu_mode && std::getenv("KYUJO2_PERF");
#endif
        if (!on) return;
        const uint32_t origin = graphics::get_vi_regs()->VI_ORIGIN_REG & 0xffffffu;
        if (origin != last_origin) { ++new_pictures; last_origin = origin; }
        const auto now = host_now_ns();
        if (!perf_t0) { perf_t0 = now; gles_stats(&perf_last); return; }
        if (now - perf_t0 < 2000000000ull) return;
        const double secs = (now - perf_t0) / 1e9;
        GlesStats st;
        gles_stats(&st);
        char render[256];
        gles_period_stats(render, sizeof render);
        std::fprintf(stderr, "perf: %.1f game pictures/s, graphics thread %.0f%% (%llu lists), GPU: %llu batches %llu triangles %llu textures %llu shaders\n",
                     new_pictures / secs, dl_ns / 1e7 / secs, dl_count, (unsigned long long)(st.batches - perf_last.batches),
                     (unsigned long long)(st.triangles - perf_last.triangles), (unsigned long long)(st.textures - perf_last.textures),
                     (unsigned long long)(st.shaders - perf_last.shaders));
        std::fprintf(stderr, "perf: %s\n", render);
        perf_last = st;
        perf_t0 = now;
        new_pictures = dl_ns = dl_count = 0;
    }
#endif
    void update_screen() override {
#ifdef KYUJO2_GPU
        perf_line();
#endif
        auto *vi = graphics::get_vi_regs();
        ::ViRegs regs{vi->VI_STATUS_REG, vi->VI_ORIGIN_REG & 0xffffffu, vi->VI_WIDTH_REG,
                      vi->VI_H_START_REG, vi->VI_V_START_REG, vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG};
        unsigned w, h;
        // The game turns on osViRepeatLine (Y scale zero, origin at the frame buffer) on retraces where its frame
        // queue is empty, holding the picture on a console. GB Tower draws its screen without that queue, so the
        // VI stays in this mode for the whole scene. Show the frame buffer at the mode's normal scale instead.
        if ((regs.y_scale & 0xFFF) == 0 && (regs.control & 3) >= 2)
            regs.y_scale |= (regs.width & 0xFFF) > 320 ? 0x800 : 0x400;
#ifdef KYUJO2_GPU
        if (gpu_mode) {
            // The menu presents its own pictures through the back end while the game is paused; the game's picture
            // must not replace them (in software mode the main thread, which runs the menu, presents the game).
            if (host_menu_is_open()) {
                notice_layer_off();
                std::lock_guard lock(menu_picture_mutex);
                if (!menu_picture.empty()) {
                    gpu_picture_settings();
                    gles_present(menu_picture.data(), menu_picture_w, menu_picture_h);
                }
                return;
            }
            gpu_picture_settings();
            // The GPU shows its colour surface itself (no read-back) unless a notice must be drawn over the picture
            // or test captures are being taken.
            static const bool capture = [] { const char *d = std::getenv("KYUJO2_CAPTURE_DIR"); return d && *d; }();
            static const bool trace = std::getenv("KYUJO2_TRACE_PRESENT") != nullptr;
            if (!capture) {
                notice_layer_update(regs);
                const bool direct = vi_present(&regs);
                if (trace) {
                    static uint32_t last_origin; static bool last_direct;
                    if (regs.origin != last_origin || direct != last_direct)
                        std::fprintf(stderr, "present: origin %06x %s (control %08x width %u h %08x v %08x xs %08x ys %08x)\n", regs.origin,
                                     direct ? "from a GPU surface" : "NOT on the GPU: read from memory", regs.control, regs.width, regs.h_video,
                                     regs.v_video, regs.x_scale, regs.y_scale);
                    last_origin = regs.origin; last_direct = direct;
                }
                if (direct) return;
                notice_layer_off();   // the picture goes through game memory: the notices are drawn into it below
            }
        }
#endif
        bool shown = vi_scanout(&regs, rgb.data(), 1024, 768, &w, &h);
#ifdef KYUJO2_SHARED_MENU
        if (shown) host_menu_overlay(rgb.data(), w, h);   // notices such as "F1 or Esc: menu" and the cheat messages
#endif
#ifdef KYUJO2_GPU
        if (shown && gpu_mode) gles_present(rgb.data(), w, h);
#endif
        if (shown) {
            std::lock_guard lock(frame_mutex);
            frame_width = w;
            frame_height = h;
            published_frame.assign(rgb.begin(), rgb.begin() + w*h*3);
            ++published_count;
        } else if (std::getenv("KYUJO2_TRACE_VI")) {
            static uint32_t last[7];
            const uint32_t now[7]{regs.control, regs.origin, regs.width, regs.h_video, regs.v_video, regs.x_scale, regs.y_scale};
            if (std::memcmp(last, now, sizeof now)) {
                std::memcpy(last, now, sizeof now);
                std::fprintf(stderr, "VI picture not shown: control %08x origin %08x width %08x h %08x v %08x xscale %08x yscale %08x\n",
                             now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
            }
        }
    }
    // Called on the graphics thread when the game stops, before the runtime frees the game's memory: the GPU back end
    // may still copy surfaces back into it, so it is stopped here.
    void shutdown() override { desktop_stop_renderer(); }
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1; }
};

uint32_t audio_frequency;
bool audio_waiting;             // paused until a cushion of samples is queued
uint32_t audio_cushion_bytes;
Uint32 audio_retry_ticks;
#ifdef __ANDROID__
constexpr Uint16 audio_period = 2048;   // phones: a longer period rides out busy frames instead of popping
#else
constexpr Uint16 audio_period = 1024;
#endif

// Called with audio_mutex held.
bool audio_closed;
void open_audio(uint32_t frequency) {
    if (audio_closed) return;
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    SDL_AudioSpec spec{}, have{};
    spec.freq = static_cast<int>(frequency);
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = audio_period;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &spec, &have, 0);
    audio_frequency = frequency;
    audio_retry_ticks = SDL_GetTicks();
    if (!audio_device) {
        std::fprintf(stderr, "Audio device: %s\n", SDL_GetError());
        return;
    }
    // Start (and restart after an underrun) only once two periods are queued, so a late audio frame becomes a
    // short gap instead of a burst of clicks.
    audio_cushion_bytes = 2u * have.samples * 4u;
    audio_waiting = true;
}

#ifdef __SWITCH__
} // namespace
// On the Switch the Homebrew loader unloads the program's code as soon as it exits (to load the next program on a
// restart), while threads that are still running keep executing that code. SDL's audio thread is the one that keeps
// running, so the audio device, and SDL with it, are closed first, and the timer thread is ended (the game threads are ended by the runtime). Every
// thread must also be joined: libnx maps a thread's stack out of the heap until then, and the launcher, which reuses
// that heap, crashed on the unmapped pages.
void desktop_shutdown() {
    ultramodern::stop_timer_thread();
    {
        // Its destructor ends the mod settings thread; static destructors are not run on this exit path.
        std::lock_guard lock(mod_context_mutex);
        mod_context.reset();
    }
    {
        std::lock_guard lock(audio_mutex);
        if (audio_device) SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        audio_closed = true;   // no reopening by a late audio frame
    }
    desktop_stop_renderer();
    SDL_Quit();
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}
namespace {
#endif

void set_frequency(uint32_t frequency) {
    std::lock_guard lock(audio_mutex);
    // The game sets the rate again when scenes change. Reopening the device each time dropped the queued
    // sound; only a different rate needs a new device.
    if (audio_device && frequency == audio_frequency) return;
    open_audio(frequency);
}
void queue_samples(int16_t *input, size_t count) {
    std::lock_guard lock(audio_mutex);
    if (!audio_device && audio_frequency && SDL_GetTicks() - audio_retry_ticks > 1000) open_audio(audio_frequency);
    if (!audio_device || count % 2) return;
    thread_local std::vector<int16_t> samples;
    samples.resize(count);
    for (size_t i = 0; i < count; i += 2) {
        samples[i] = input[i+1];
        samples[i+1] = input[i];
    }
#ifdef KYUJO2_SHARED_MENU
    int gain = shared_menu::volume();
    if (gain != 100) for (auto &sample : samples) sample = int(sample) * gain / 100;
#endif
    Uint32 queued = SDL_GetQueuedAudioSize(audio_device);
    // The device stops pulling samples while the app is in the background (Android pauses it) or the window is
    // stalled, but the game keeps producing them. Played later, that backlog leaves the sound seconds behind the
    // picture, so a queue far beyond the start-up cushion is dropped and the cushion rebuilt.
    if (queued > 4u * audio_cushion_bytes) {
        std::fprintf(stderr, "Audio: dropped %u ms of queued sound to stay in sync\n",
                     static_cast<unsigned>(queued / 4u * 1000u / (audio_frequency ? audio_frequency : 1)));
        SDL_ClearQueuedAudio(audio_device);
        queued = 0;
        audio_waiting = true;
        SDL_PauseAudioDevice(audio_device, 1);
    }
    if (!audio_waiting && queued == 0) {
        audio_waiting = true;           // underrun: build the cushion again
        SDL_PauseAudioDevice(audio_device, 1);
    }
    if (SDL_QueueAudio(audio_device, samples.data(), static_cast<uint32_t>(count * 2)) != 0) {
        std::fprintf(stderr, "Audio queue: %s\n", SDL_GetError());
        return;
    }
    if (audio_waiting && queued + count * 2 >= audio_cushion_bytes) {
        audio_waiting = false;
        SDL_PauseAudioDevice(audio_device, 0);
    }
}
void update_window(void *) {
#ifdef KYUJO2_SHARED_MENU
    shared_menu::poll();
#else
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) ultramodern::quit();
        if (!controller && event.type == SDL_CONTROLLERDEVICEADDED)
            controller = SDL_GameControllerOpen(event.cdevice.which);
        if (controller && event.type == SDL_CONTROLLERDEVICEREMOVED &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }

    }
    const auto *key = SDL_GetKeyboardState(nullptr);
    const std::array<SDL_Scancode,16> mapping{SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_I,
        SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
        SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_W,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_C, SDL_SCANCODE_X, SDL_SCANCODE_Z};
    uint16_t mask = 0;
    for (unsigned i = 0; i < mapping.size(); ++i) if (key[mapping[i]]) mask |= 1u << i;
    float x = key[SDL_SCANCODE_RIGHT] - key[SDL_SCANCODE_LEFT];
    float y = key[SDL_SCANCODE_UP] - key[SDL_SCANCODE_DOWN];
    if (controller) {
        const auto down = [](SDL_GameControllerButton button) {
            return SDL_GameControllerGetButton(controller, button) != 0;
        };
        if (down(SDL_CONTROLLER_BUTTON_A)) mask |= 0x8000;
        if (down(SDL_CONTROLLER_BUTTON_B)) mask |= 0x4000;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192) mask |= 0x2000;
        if (down(SDL_CONTROLLER_BUTTON_START)) mask |= 0x1000;
        if (down(SDL_CONTROLLER_BUTTON_DPAD_UP)) mask |= 0x0800;
        if (down(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= 0x0400;
        if (down(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= 0x0200;
        if (down(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mask |= 0x0100;
        if (down(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) mask |= 0x0020;
        if (down(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= 0x0010;
        auto cx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        auto cy = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (cy < -16384) mask |= 8;
        if (cy > 16384) mask |= 4;
        if (cx < -16384) mask |= 2;
        if (cx > 16384) mask |= 1;
        if (x == 0) x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
        if (y == 0) y = -SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
    }
#ifdef __ANDROID__
    touch::merge(mask, x, y);
#endif
    buttons = mask;
    stick_x = x;
    stick_y = y;
#endif
    static unsigned displayed;
    std::lock_guard lock(frame_mutex);
    if (!window || displayed == published_count) return;
    SDL_Surface *frame = SDL_CreateRGBSurfaceWithFormatFrom(published_frame.data(), frame_width, frame_height,
        24, frame_width * 3, SDL_PIXELFORMAT_RGB24);
    if (!frame) return;
    SDL_Surface *surface = gpu_mode ? nullptr : SDL_GetWindowSurface(window);   // GPU mode: shown by the back end
    if (surface) {
#ifdef KYUJO2_SHARED_MENU
        SDL_Rect dst{0,0,surface->w,surface->h};
        if(!shared_menu::wide()) {
            dst.w=std::min(surface->w,surface->h*4/3);
            dst.h=std::min(surface->h,surface->w*3/4);
            dst.x=(surface->w-dst.w)/2;dst.y=(surface->h-dst.h)/2;
        }
        SDL_FillRect(surface,nullptr,0);
        SDL_Surface *converted=SDL_ConvertSurface(frame,surface->format,0);
        if(converted && surface->format->BytesPerPixel==4 && shared_menu::scaling()!=1) {
            SDL_Surface *source=converted, *firm=nullptr;
            if(shared_menu::scaling()==2 && dst.w>converted->w && dst.h>converted->h) {
                firm=SDL_CreateRGBSurfaceWithFormat(0,converted->w*2,converted->h*2,32,surface->format->format);
                if(firm && SDL_BlitScaled(converted,nullptr,firm,nullptr)==0) source=firm;
            }
            if(SDL_SoftStretchLinear(source,nullptr,surface,&dst)!=0) SDL_BlitScaled(frame,nullptr,surface,&dst);
            SDL_FreeSurface(firm);
        } else SDL_BlitScaled(frame,nullptr,surface,&dst);
        SDL_FreeSurface(converted);
#else
        SDL_BlitScaled(frame, nullptr, surface, nullptr);
#endif
        SDL_UpdateWindowSurface(window);
    }
    if (const char *dir = std::getenv("KYUJO2_CAPTURE_DIR"); dir && *dir) {   // empty: off (not the current folder)
        static const unsigned interval = [] {
            const char *value = std::getenv("KYUJO2_CAPTURE_STEP");
            char *end = nullptr;
            long step = value ? std::strtol(value, &end, 10) : 0;
            return static_cast<unsigned>(step > 0 && step <= 3600 && end && !*end ? step : 60);
        }();
        if (!displayed || displayed/interval != published_count/interval) {
            auto path = std::filesystem::path(dir) / ("frame-" + std::to_string(published_count) + ".bmp");
            SDL_SaveBMP(frame, path.string().c_str());
        }
    }
    SDL_FreeSurface(frame);
    displayed = published_count;
}
}

static void open_window() {
    if (window) return;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#ifdef KYUJO2_WINDOWS_UI
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
        std::fprintf(stderr, "SDL startup failed: %s\n", SDL_GetError());
        std::exit(2);
    }
#ifdef KYUJO2_GPU
    if (gpu_wanted()) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow("Kyujo2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (window) gl_context = SDL_GL_CreateContext(window);
        if (gl_context) {
            SDL_GL_SetSwapInterval(1);
            SDL_GL_MakeCurrent(window, nullptr);                  // the back end's render thread takes the context
            GlesHost host{};
            // The GPU programs the game needed are listed in shader-keys.bin there and built at the next start, not in
            // the middle of a battle (each takes up to ~170 ms on the Switch).
            static const std::string cache_dir =
#ifdef __SWITCH__
                "sdmc:/switch/Kyujo2";
#else
                default_data_directory().string();
#endif
            host.cache_dir = cache_dir.c_str();
            host.make_current = gl_make_current;
            host.release = gl_release;
            host.swap = gl_swap;
            host.window_size = gl_size;
            gpu_mode = gles_start(&host) == 0;
        }
        if (gpu_mode) {
            std::fprintf(stderr, "Renderer: GPU (OpenGL ES 3)\n");
        } else {
            std::fprintf(stderr, "Renderer: no OpenGL ES 3 context (%s); using the software renderer\n", SDL_GetError());
            if (gl_context) SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
            if (window) SDL_DestroyWindow(window);
            window = nullptr;
        }
    }
#endif
    if (!window) {
        window = SDL_CreateWindow("Kyujo2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  640, 480, SDL_WINDOW_RESIZABLE);
        if (window && !gpu_mode) std::fprintf(stderr, "Renderer: software\n");
    }
    if (!window) { std::fprintf(stderr, "%s\n", SDL_GetError()); std::exit(2); }
#ifndef __ANDROID__
    if (SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<unsigned char *>(kyujo2_icon_rgba), 64, 64, 32, 64 * 4,
                                                               SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
#endif
}
void desktop_stop_renderer() {
#ifdef KYUJO2_GPU
    if (gpu_mode) gles_stop();   // joins the GPU back end's threads
    gpu_mode = false;
#endif
}
#ifdef KYUJO2_SHARED_MENU
SDL_Window *desktop_window() { open_window(); return window; }
bool desktop_present_rgb(const unsigned char *rgb, unsigned w, unsigned h) {
#ifdef KYUJO2_GPU
    if (gpu_mode) {
        notice_layer_off();
        {
            std::lock_guard lock(menu_picture_mutex);
            menu_picture.assign(rgb, rgb + static_cast<size_t>(w) * h * 3);
            menu_picture_w = w;
            menu_picture_h = h;
        }
        gpu_picture_settings();
        gles_present(rgb, w, h);
        return true;
    }
#endif
    (void)rgb; (void)w; (void)h;
    return false;
}
#endif
std::filesystem::path default_data_directory() {
#ifdef KYUJO2_WINDOWS_UI
    if (const wchar_t *override_dir = _wgetenv(L"KYUJO2_DATA_DIR"); override_dir && *override_dir)
        return std::filesystem::path(override_dir);
#endif
    char *path = SDL_GetPrefPath("Kyujo2", "Kyujo2");
    if (!path) throw std::runtime_error(SDL_GetError());
    auto dir = std::filesystem::u8path(path);
    SDL_free(path);
    return dir;
}
std::filesystem::path choose_rom() {
    open_window();
    SDL_SetWindowTitle(window, cartridge_prompt);
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        if (event.type == SDL_QUIT) break;
        if (event.type == SDL_DROPFILE) {
            auto path = std::filesystem::u8path(event.drop.file);
            SDL_free(event.drop.file);
            SDL_SetWindowTitle(window, "Kyujo2");
            return path;
        }
    }
    return {};
}

void configure_desktop(recomp::Configuration& cfg) {
    open_window();
    cfg.audio_callbacks = {queue_samples, []() -> size_t {
        std::lock_guard lock(audio_mutex);
        return audio_device ? SDL_GetQueuedAudioSize(audio_device)/4 : 0;
    }, set_frequency};
    cfg.input_callbacks.get_input = [](int port, uint16_t *b, float *x, float *y) {
#ifdef KYUJO2_SHARED_MENU
        return shared_menu::input(port, b, x, y);
#else
        if (port) return false;
        *b = buttons.load();
        *x = stick_x.load();
        *y = stick_y.load();
        static thread_local uint16_t last_buttons;
        if (*b != last_buttons && std::getenv("KYUJO2_TRACE_INPUT"))
            std::fprintf(stderr, "N64 controller 1 sampled buttons %04x\n", static_cast<unsigned>(*b));
        last_buttons = *b;
        return true;
#endif
    };
    cfg.input_callbacks.get_connected_device_info = [](int port) {
        using namespace ultramodern::input;
#ifdef KYUJO2_SHARED_MENU
        return connected_device_info_t{(shared_menu::connected(port) || kyujo2_pak_present(port)) ? Device::Controller : Device::None,
            kyujo2_pak_present(port) ? Pak::TransferPak : Pak::None};
#else
        return connected_device_info_t{port ? Device::None : Device::Controller, Pak::None};
#endif
    };
    cfg.renderer_callbacks.create_render_context = [](uint8_t *ram, graphics::WindowHandle, bool)
        -> std::unique_ptr<graphics::RendererContext> { return std::make_unique<SoftwareRenderer>(ram); };
    cfg.gfx_callbacks.create_window = [](void*) -> graphics::WindowHandle { return {}; };
    cfg.gfx_callbacks.update_gfx = update_window;
}

