#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <cstdio>
#include "windows_settings.hpp"
#include "windows_input.hpp"
extern "C" {
#include "host_ui.h"
int menu_key_code(int);
}
namespace {
constexpr UINT base=400;
LRESULT CALLBACK command(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if(msg==WM_COMMAND && HIWORD(wp)==0 && LOWORD(wp)>=base && LOWORD(wp)<base+HM_SETUP) {
        host_menu_request(LOWORD(wp)-base); return 0;
    }
    return DefSubclassProc(h,msg,wp,lp);
}
void copy_value(char *dst,size_t size,const std::wstring &v) {
    auto s=std::filesystem::path(v).u8string();
    std::snprintf(dst,size,"%s",reinterpret_cast<const char*>(s.c_str()));
}
}
void windows_menu_start(SDL_Window *window,const std::filesystem::path &root,bool fresh) {
    auto legacy=root/L"Kyujo2.json";
    if(fresh && std::filesystem::exists(legacy)) {
        prefs::open(legacy);
        auto s=host_settings();
        s->pak_setup_done=prefs::number(L"Setup",L"GamePaksConfigured",0);
        s->volume=prefs::number(L"Options",L"Volume",100);
        s->fullscreen=prefs::number(L"Options",L"Fullscreen",0);
        auto flags=prefs::number(L"Options",L"BattleCheats",0);
        s->cheats=flags&1; s->cheat_hof=(flags>>1)&1;
        for(int i=0;i<4;++i) {
            auto g=L"Game Pak "+std::to_wstring(i+1);
            copy_value(s->gb_rom[i],HS_PATH,prefs::get(g.c_str(),L"ROM"));
            copy_value(s->gb_save[i],HS_PATH,prefs::get(g.c_str(),L"Battery"));
        }
        const int actions[]={ACT_CRIGHT,ACT_CLEFT,ACT_CDOWN,ACT_CUP,ACT_R,ACT_L,ACT_DRIGHT,ACT_DLEFT,ACT_DDOWN,ACT_DUP,ACT_START,ACT_Z,ACT_B,ACT_A,ACT_STICK_LEFT,ACT_STICK_RIGHT,ACT_STICK_UP,ACT_STICK_DOWN};
        for(int i=0;i<18;++i) {
            int keyboard=prefs::number(L"Keyboard",pads::actions[i],-1);
            if(keyboard>=0) s->key[actions[i]][0]=menu_key_code(keyboard);
        }
        host_settings_save(s,host_settings_path());
    }
    SDL_SysWMinfo info{};SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(window,&info)) return;
    auto h=info.info.win.window;
    auto bar=CreateMenu();
    struct Entry { const wchar_t *name; int screen; };
    const Entry items[]={{L"&File",HM_GAMEROM},{L"&Controllers",HM_CONTROLLERS},{L"&Game Paks",HM_PAKS},{L"&Options",HM_OPTIONS},{L"C&heats",HM_OPTIONS},{L"&Help",HM_ABOUT}};
    const wchar_t *labels[]={L"Stadium ROM and save file…",L"Devices and button assignments…",L"Configure Transfer Paks…",L"Options…",L"Battle cheats…",L"About and file locations…"};
    for(unsigned i=0;i<6;++i) {
        auto sub=CreatePopupMenu();AppendMenuW(sub,MF_STRING,base+items[i].screen,labels[i]);
        if(!i){AppendMenuW(sub,MF_STRING,base+HM_MAIN,L"Open game menu");AppendMenuW(sub,MF_STRING,base+HM_RESTART,L"Restart game…");}
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(sub),items[i].name);
    }
    SetMenu(h,bar);SetWindowSubclass(h,command,14,0);DrawMenuBar(h);
}
// One argument as CommandLineToArgvW parses it: backslashes are literal except before a quote. SDL's
// preference folder ends in a backslash, so an unescaped "...\Kyujo2\" swallowed the closing quote and the
// restarted game looked for its ROM and settings in a folder whose name ended in a quote character.
static std::wstring quote_argument(const std::wstring &value) {
    std::wstring out=L"\"";
    size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\') {++slashes;continue;}
        out.append(c==L'"'?slashes*2+1:slashes,L'\\');
        slashes=0;
        out.push_back(c);
    }
    out.append(slashes*2,L'\\');
    out.push_back(L'"');
    return out;
}
void windows_menu_restart(const std::filesystem::path &root) {
    std::wstring exe(32768,L'\0');
    DWORD n=GetModuleFileNameW(nullptr,exe.data(),exe.size());
    if(!n||n>=exe.size())return;exe.resize(n);
    std::wstring args=quote_argument(exe)+L" "+quote_argument((root/L"stadium2-us.z64").wstring())+L" "+quote_argument(root.wstring());
    STARTUPINFOW si{sizeof si};PROCESS_INFORMATION pi{};
    if(CreateProcessW(exe.c_str(),args.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    }
}
