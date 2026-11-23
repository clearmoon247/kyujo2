#include "shared_menu.hpp"
#include "desktop.hpp"
#include "region.hpp"
#include "windows_cheats.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#ifdef __SWITCH__
#include <switch.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif
extern "C" {
#include "host_ui.h"
#include "platform.h"
#include "recomp.h"
void menu_input_init(SDL_Window *);
void menu_input_event(const SDL_Event *);
void menu_publish_input();
int kyujo2_choose_stadium();
void __real_osRecvMesg_recomp(uint8_t*,recomp_context*);
}
bool stadium2_battle_loaded();
void update_save_file();
#ifdef _WIN32
void windows_menu_start(SDL_Window *, const std::filesystem::path &,bool);
void windows_menu_restart(const std::filesystem::path &);
#endif
#ifdef __ANDROID__
void android_restart_game();
#endif
namespace fs=std::filesystem;
namespace {
SDL_Window *window;
fs::path root;
std::string save, self;
std::atomic<bool> paused{};
std::mutex pause_mutex, pak_mutex;
std::condition_variable resume;
std::atomic<uint8_t*> guest{};
std::atomic<unsigned> cheats{};
std::atomic<int> gain{100};
std::array<std::atomic<uint64_t>,4> ports{};
bool closing, restarting;
int click_x=-1,click_y;
std::string utf8(const fs::path &p) {auto s=p.u8string(); return {s.begin(),s.end()};}
void host_env(const char *name,const std::string &value) {
#ifdef _WIN32
    _putenv_s(name,value.c_str());
#else
    setenv(name,value.c_str(),1);
#endif
}
void refresh() {
    HostSettings *s=host_settings();
    gain=s->volume;
    cheats=(s->cheats?1:0)|(s->cheat_hof?2:0);
    stadium2_cheats_enabled(cheats.load());
    HostRawInput raw;
    host_raw_get(&raw);
    for(int i=0;i<4;++i) {
        unsigned short b=0; signed char x=0,y=0; int connected=0;
        host_input_map(s,&raw,i+1,&b,&x,&y,&connected);
        if(host_menu_is_open()) {b=0; x=y=0;}
        ports[i]=uint64_t(b)|(uint64_t(uint8_t(x))<<16)|(uint64_t(uint8_t(y))<<24)|(uint64_t(connected!=0)<<32);
    }
}
}
extern "C" unsigned long long host_wall_ns() {return host_now_ns();}
extern "C" void host_sleep_ns(unsigned long long ns) {SDL_Delay((ns+999999)/1000000);}
extern "C" void host_clock_pause() {std::lock_guard lock(pause_mutex); paused=true;}
extern "C" void host_clock_resume() {{std::lock_guard lock(pause_mutex); paused=false;} resume.notify_all();}
extern "C" void __wrap_osRecvMesg_recomp(uint8_t *r,recomp_context *ctx) {
    if (paused.load()) { std::unique_lock lock(pause_mutex); resume.wait(lock,[]{return !paused.load();}); }
    __real_osRecvMesg_recomp(r,ctx);
}
extern "C" void host_window_present(const unsigned char *rgb,unsigned w,unsigned h) {
    if(desktop_present_rgb(rgb,w,h)) return;
    SDL_Surface *src=SDL_CreateRGBSurfaceWithFormatFrom(const_cast<unsigned char*>(rgb),w,h,24,w*3,SDL_PIXELFORMAT_RGB24);
    SDL_Surface *dst=SDL_GetWindowSurface(window);
    if(src && dst) {SDL_BlitScaled(src,nullptr,dst,nullptr); SDL_UpdateWindowSurface(window);}
    SDL_FreeSurface(src);
}
extern "C" void host_audio_idle_tick() {}
extern "C" void host_window_set_fullscreen(int on) {SDL_SetWindowFullscreen(window,on?SDL_WINDOW_FULLSCREEN_DESKTOP:0);}
extern "C" int host_window_is_fullscreen() {return (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=0;}
extern "C" int kyujo2_menu_click(int *x,int *y) {
    if(click_x<0) return 0;
    *x=click_x; *y=click_y; click_x=-1; return 1;
}
extern "C" void kyujo2_request_exit(int) {closing=true; host_clock_resume(); if(guest.load()) ultramodern::quit();}
extern "C" int kyujo2_menu_pump() {
    SDL_Event e;
    while(SDL_PollEvent(&e)) {
        menu_input_event(&e);
        if(e.type==SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.scancode==SDL_SCANCODE_F11 ||
           (e.key.keysym.scancode==SDL_SCANCODE_RETURN && (e.key.keysym.mod&KMOD_ALT)))) host_window_set_fullscreen(!host_window_is_fullscreen());
        if(e.type==SDL_QUIT) kyujo2_request_exit(0);
        if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT && paused) {
            int w,h; SDL_GetWindowSize(window,&w,&h);
            if(w>0 && h>0) { int canvas=std::max(640,480*w/h); click_x=e.button.x*canvas/w-(canvas-640)/2; click_y=e.button.y*480/h; }
        }
        if(e.type==SDL_DROPFILE) SDL_free(e.drop.file);
    }
    menu_publish_input();
    return !closing;
}
extern "C" const char *host_rom_check(const char *path) {
    std::error_code ec;
    if(fs::file_size(fs::u8path(path),ec)!=67108864 || ec) return "Choose the 64 MiB USA Pokemon Stadium 2 cartridge.";
    return recomp::select_rom(fs::u8path(path),cartridge_id)==recomp::RomValidationError::Good ? nullptr : "This cartridge does not match Pokemon Stadium 2 (USA).";
}
extern "C" int host_restart() {restarting=true;kyujo2_request_exit(0);return 1;}
extern "C" void host_run_exit_hook() {host_gbpak_flush();}
extern "C" void host_exit(int code) {host_gbpak_flush();std::exit(code);}
extern "C" const char *host_save_path() {return save.c_str();}
extern "C" void host_save_flush() {if(guest.load()) update_save_file();}
extern "C" unsigned int host_gfx_frame_count() {return SDL_GetTicks()/16;}
extern "C" void host_cheat_help(const HostSettings*,char *out,unsigned size) {std::snprintf(out,size,"L + R: you win the battle after your next move. Z: first-slot HP.");}
extern "C" void kyujo2_pak_unload(int port) {std::lock_guard lock(pak_mutex);host_gbpak_unload(port);}
// File steps for the private save copies. They avoid std::filesystem::copy_file and replacing renames: on the Switch
// every file reports the same device and inode numbers, so copy_file takes an existing destination for its source and
// fails, and rename() does not replace an existing file.
static bool pak_copy_if_missing(const fs::path &from,const fs::path &to) {
    std::error_code ec;
    if(fs::exists(to,ec)) return true;
    std::ifstream in(from,std::ios::binary);
    std::ofstream out(to,std::ios::binary);
    out<<in.rdbuf();
    return in && out;
}
static void pak_move_aside(const fs::path &from,const fs::path &to) {
    std::error_code ec;
    fs::remove(to,ec);
    fs::rename(from,to,ec);
}
static const char *pak_load(int port,const char *rom,const char *battery,std::string &detail) {
    auto src=fs::u8path(battery);
    uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:std::string(rom)+"|"+battery) hash=(hash^c)*1099511628211ULL;
    if(fs::is_regular_file(src)) {
        std::ifstream source(src,std::ios::binary);
        const auto length=fs::file_size(src);
        if(length!=32768 && length!=32812 && length!=32816) {
            detail="it is "+std::to_string(length)+" bytes; a Game Boy save is 32 KiB (32768 bytes), or 32816 with a clock";
            return "";
        }
        char c;
        while(source.get(c)) hash=(hash^static_cast<unsigned char>(c))*1099511628211ULL;
        if(!source.eof()) return "it could not be read";
    }
    const auto dir=root/"Game Paks"/("port-"+std::to_string(port+1)+"-"+std::to_string(hash));
    fs::create_directories(dir);
    auto work=dir/"battery.sav";
    std::error_code ec;
    if(fs::exists(work,ec)) {
        // Earlier builds could leave the whole Gold/Silver file here, clock trailer included. The cartridge takes
        // exactly 32 KiB, so keep that part (the full file is kept beside it) instead of refusing it.
        const auto n=fs::file_size(work);
        if(n==32812 || n==32816) {
            std::array<char,32768> bytes{};
            {std::ifstream f(work,std::ios::binary); if(!f.read(bytes.data(),bytes.size())) return "Kyujo2's copy of it could not be read";}
            pak_copy_if_missing(work,dir/"battery-with-clock.sav");
            {std::ofstream o(work,std::ios::binary|std::ios::trunc); o.write(bytes.data(),bytes.size()); if(!o) return "Kyujo2's copy of it could not be repaired";}
            std::fprintf(stderr,"Game Pak %d: trimmed the clock trailer from %s\n",port+1,utf8(work).c_str());
        } else if(n!=32768) {
            pak_move_aside(work,dir/("battery-unusable-"+std::to_string(n)+".sav"));   // made again below
            std::fprintf(stderr,"Game Pak %d: set aside a %llu-byte private save\n",port+1,(unsigned long long)n);
        }
    }
    if(!fs::exists(work,ec) && !fs::exists(src,ec)) {
        // Without a save beside the ROM the cartridge starts empty, which looks like a lost save in GB Tower.
        std::fprintf(stderr, "Game Pak %d: no save file %s; the cartridge starts with an empty save\n", port + 1, battery);
        static std::string notice;
        notice="Port "+std::to_string(port+1)+": no .sav found beside the Game Boy ROM (empty save)";
        host_menu_notice(notice.c_str());
    }
    if(!fs::exists(work,ec) && fs::exists(src,ec)) {
        const auto n=fs::file_size(src);
        std::array<char,32768> bytes{};
        std::ifstream f(src,std::ios::binary);
        if(!f.read(bytes.data(),bytes.size())) return "it could not be read";
        if(n>bytes.size()) pak_copy_if_missing(src,dir/"original-with-clock.sav");
        auto tmp=dir/"import.tmp";
        std::ofstream o(tmp,std::ios::binary|std::ios::trunc); o.write(bytes.data(),bytes.size());o.close();
        if(!o) return "Kyujo2 could not store its copy of it (is the SD card full or read-only?)";
        fs::rename(tmp,work,ec);
        if(ec) {detail="Kyujo2 could not store its copy of it ("+ec.message()+")"; return "";}
    }
    std::lock_guard lock(pak_mutex);
    if(const char *err=host_gbpak_load(port,rom,utf8(work).c_str())) {
        // The cartridge code names the file it opened, which is the private copy: say what it is.
        detail=err;
        const auto copy=utf8(work);
        if(auto at=detail.find(copy); at!=std::string::npos) detail.replace(at,copy.size(),"Kyujo2's copy of this save ("+utf8(work.parent_path().filename()/"battery.sav")+")");
        return "";
    }
    return nullptr;
}
// Loads a Game Boy cartridge. Its save is used through a private 32 KiB copy under "Game Paks", which the game writes
// to; the player's own .sav file is only read. Errors name the player's file, and are logged.
extern "C" const char *kyujo2_pak_load(int port,const char *rom,const char *battery) {
    static std::string error;
    std::string detail;
    const char *reason;
    try {
        reason=pak_load(port,rom,battery,detail);
        if(!reason) return nullptr;
        if(!*reason) reason=detail.c_str();
    } catch(const std::exception &e) {detail=e.what(); reason=detail.c_str();}
    error="Game Pak "+std::to_string(port+1)+" save "+utf8(fs::u8path(battery).filename())+": "+reason;
    std::fprintf(stderr,"%s\n",error.c_str());
    return error.c_str();
}

bool shared_menu::setup(SDL_Window *win,const fs::path &data,const fs::path &rom,int argc,char **argv) {
    window=win;root=data;self=argc>0?argv[0]:"";
    host_env("KYUJO_HOME",utf8(root));
    host_env("KYUJO_CFG",utf8(root/"Kyujo2.cfg"));
#if defined(__SWITCH__) || defined(__ANDROID__)
    host_env("KYUJO_FIXED_WINDOW","1");
#endif
    menu_input_init(window);
    HostSettings *s=host_settings();
    bool fresh=host_settings_load(s,host_settings_path())<0;
    if(fresh) {
        const int keys[]={'Z','X','C',HK_ENTER,'W','S','A','D','Q','E','I','K','J','L',HK_UP,HK_DOWN,HK_LEFT,HK_RIGHT};
        for(int i=0;i<ACT_KEY_COUNT;++i){s->key[i][0]=keys[i];s->key[i][1]=HK_NONE;}
    }
#ifdef _WIN32
    windows_menu_start(window,root,fresh);
#endif
    auto input=rom;
    if(argc<2 && s->rom[0] && fs::exists(fs::u8path(s->rom))) input=fs::u8path(s->rom);
    if(host_rom_check(utf8(input).c_str())) {
        s->rom[0]=0;
        if(!kyujo2_choose_stadium() || closing) return false;
    }
    std::snprintf(s->rom,sizeof s->rom,"%s",utf8(root/cartridge_file).c_str());
    save=s->save[0]?s->save:utf8(root/"saves"/"stadium2-us.bin");
    host_env("KYUJO2_STADIUM_SAVE",save);
    for(int i=0;i<4;++i) if(s->gb_rom[i][0]) {
        char path[1100];host_path_with_ext(s->gb_rom[i],".sav",path,sizeof path);
        if(auto err=kyujo2_pak_load(i,s->gb_rom[i],s->gb_save[i][0]?s->gb_save[i]:path)) host_menu_notice(err);
    }
#ifdef __ANDROID__
    for(int i=0;i<4;++i) {
        const auto pending=root/("incoming-pak-"+std::to_string(i+1));
        std::ifstream file(pending);
        std::string selected;
        if(!std::getline(file,selected) || selected.empty() || selected.size()>=HS_PATH) continue;
        auto path=fs::u8path(selected);
        if(path.parent_path()!=root/"GB") continue;
        auto battery=path;battery.replace_extension(".sav");
        if(auto err=kyujo2_pak_load(i,selected.c_str(),utf8(battery).c_str())) { host_menu_notice(err); continue; }
        std::snprintf(s->gb_rom[i],HS_PATH,"%s",selected.c_str());
        s->gb_save[i][0]=0;
        if(host_settings_save(s,host_settings_path())==0) {
            std::error_code ec;
            fs::remove(pending,ec);
        }
    }
#endif
    if(s->fullscreen) host_window_set_fullscreen(1);
    kyujo2_menu_pump();host_first_run_setup();refresh();
    return !closing;
}
void shared_menu::poll() {if(!kyujo2_menu_pump())return;host_menu_poll();refresh();}
void shared_menu::start_guest(uint8_t *r) {guest=r;}
bool shared_menu::input(int port,uint16_t *b,float *x,float *y) {
    if(port<0 || port>=4)return false;
    auto v=ports[port].load();
    *b=uint16_t(v);*x=int8_t(v>>16)/80.f;*y=int8_t(v>>24)/80.f;
    if(!port && stadium2_battle_loaded()) stadium2_battle_cheats(guest.load(),cheats.load(),*b);
    return (v>>32)!=0;
}
bool shared_menu::connected(int port) {return port>=0 && port<4 && (ports[port].load()>>32)!=0;}
int shared_menu::volume() {return gain.load();}
int shared_menu::scaling() {return host_settings()->scale_style;}
bool shared_menu::wide() {return host_settings()->widescreen!=0;}
void shared_menu::finish() {
    host_gbpak_flush();
#ifdef __SWITCH__
    desktop_shutdown();
    if(restarting && !self.empty()) envSetNextLoad(self.c_str(),self.c_str());
    return;
#endif
    desktop_stop_renderer();
    if(!restarting)return;
#if defined(__ANDROID__)
    android_restart_game();
#elif defined(_WIN32)
    windows_menu_restart(root);
#else
    auto r=utf8(root/cartridge_file), d=utf8(root);
    const char *app=std::getenv("APPIMAGE");
    const char *exe=app&&*app?app:"/proc/self/exe";
    execl(exe,exe,r.c_str(),d.c_str(),(char*)nullptr);
#endif
}

extern "C" int kyujo2_pak_present(int port) {
    std::lock_guard guard(pak_mutex);
    return host_gbpak_present(port);
}
extern "C" int kyujo2_pak_read(int port, unsigned address, unsigned char *bytes) {
    std::lock_guard guard(pak_mutex);
    if (!host_gbpak_present(port)) return 0;
    const auto started = host_now_ns();
    host_gbpak_read(port,address,bytes,32);
    static unsigned long long inside;
    inside += host_now_ns() - started;
    static unsigned blocks;
    if (std::getenv("KYUJO2_TRACE_PAK") && ++blocks % 256 == 0)
        std::fprintf(stderr, "Transfer Pak %d: %u KiB read (%llu ms, %llu ms inside reads)\n", port + 1, blocks / 32,
                     static_cast<unsigned long long>(host_now_ns() / 1000000), inside / 1000000);
    return 1;
}
extern "C" int kyujo2_pak_write(int port, unsigned address, const unsigned char *bytes) {
    std::lock_guard guard(pak_mutex);
    if (!host_gbpak_present(port)) return 0;
    static unsigned writes;
    if (std::getenv("KYUJO2_TRACE_PAK") && writes++ < 400)
        std::fprintf(stderr, "Transfer Pak %d write %04x: %02x %02x %02x %02x\n", port + 1, address, bytes[0], bytes[1], bytes[2], bytes[3]);
    host_gbpak_write(port,address,bytes,32);
    return 1;
}
