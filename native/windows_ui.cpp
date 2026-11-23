#include "windows_ui.h"
#include "windows_input.hpp"
#include "windows_settings.hpp"
#include "menu_canvas.h"
#include "windows_cheats.h"
#include "region.hpp"
#include "librecomp/game.hpp"
#include <SDL_syswm.h>
#include <SDL_system.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <array>
#include <atomic>
#include <fstream>
#include <vector>
extern "C" {
#include "platform.h"
}
bool stadium2_battle_loaded();

namespace winui {
namespace fs = std::filesystem;
namespace {
enum class Page { play, rom, paks, controllers, options, home, cheats, pakSlot, bindings };
enum Command {
    choose = 400,
    backup,
    folder,
    restart,
    leave,
    configurePads,
    configurePaks,
    settings,
    fullscreen,
    mute,
    ko,
    hp,
    about,
    home,
    battleSettings
};
SDL_Window *sdl;
HWND window, panel;
HMENU bar;
fs::path data, ini;
Page page = Page::play;
bool begun, ready, cancelled, romValid, relaunch;
bool keyboardBinding;
int port;
std::atomic<int> gain{100};
std::atomic<unsigned> cheatFlags{};
std::array<std::atomic<uint64_t>, 4> samples{};
std::array<std::atomic<bool>, 4> online{};
uint8_t *guestRam;
bool menuHeld, confirmHeld, upHeld, downHeld, leftHeld, rightHeld, backHeld;
std::array<bool, 4> blockHeldInput{};
std::wstring note;
std::wstring get(const wchar_t *section, const wchar_t *key) {
    return prefs::get(section, key);
}
void store(const wchar_t *section, const wchar_t *key, const std::wstring &value) {
    prefs::set(section, key, value);
}
std::wstring pakSection() {
    return L"Game Pak " + std::to_wstring(port + 1);
}
std::wstring widen(const char *s) {
    return fs::u8path(s).wstring();
}
std::string utf8(const fs::path &p) {
    auto s = p.u8string();
    return {s.begin(), s.end()};
}
fs::path picker(const wchar_t *title, const wchar_t *filter, bool output = false) {
    std::vector<wchar_t> path(32768);
    OPENFILENAMEW f{sizeof(f)};
    f.hwndOwner = window;
    f.lpstrTitle = title;
    f.lpstrFilter = filter;
    f.lpstrFile = path.data();
    f.nMaxFile = path.size();
    f.Flags =
        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (output ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (output)
        f.lpstrDefExt = L"bin";
    BOOL ok = output ? GetSaveFileNameW(&f) : GetOpenFileNameW(&f);
    if (!ok && CommDlgExtendedError())
        throw std::runtime_error("The Windows file picker could not open.");
    return ok ? fs::path(path.data()) : fs::path{};
}
constexpr auto n64filter = L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0";
constexpr auto gbfilter = L"Game Boy cartridges (*.gb;*.gbc)\0*.gb;*.gbc\0All files\0*.*\0";
constexpr auto savefilter = L"Game Boy saves (*.sav;*.srm)\0*.sav;*.srm\0All files\0*.*\0";
void message(const std::wstring &s) {
    MessageBoxW(window, s.c_str(), L"Kyujo2", MB_OK | MB_ICONINFORMATION);
}
void add(HMENU m, UINT id, const wchar_t *name) {
    AppendMenuW(m, MF_STRING, id, name);
}
HMENU submenu(const wchar_t *name) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(m), name);
    return m;
}
void menu() {
    bar = CreateMenu();
    HMENU file = submenu(L"&File");
    add(file, choose, L"Choose Stadium 2 &ROM…");
    add(file, backup, L"Back up Stadium &save…");
    add(file, folder, L"Open data &folder");
    add(file, restart, L"&Restart game…");
    add(file, leave, L"E&xit");
    add(submenu(L"&Controllers"), configurePads, L"Devices and button assignments…");
    add(submenu(L"&Game Paks"), configurePaks, L"Configure Transfer Paks…");
    HMENU opt = submenu(L"&Options");
    add(opt, settings, L"Sound and display…");
    add(opt, fullscreen, L"Full screen\tF11");
    HMENU cheats = submenu(L"C&heats");
    add(cheats, battleSettings, L"Battle cheats…");
    add(cheats, ko, L"L + R to knock out opponent");
    add(cheats, hp, L"Z for maximum HP (first Pokémon)");
    add(submenu(L"&Help"), about, L"About Kyujo2 and file locations…");
    SetMenu(window, bar);
    DrawMenuBar(window);
}
void checkMenus() {
    CheckMenuItem(bar, ko, MF_BYCOMMAND | ((cheatFlags.load() & 1) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, hp, MF_BYCOMMAND | ((cheatFlags.load() & 2) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, fullscreen,
                  MF_BYCOMMAND |
                      ((SDL_GetWindowFlags(sdl) & SDL_WINDOW_FULLSCREEN) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(bar, restart, MF_BYCOMMAND | (begun ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(bar, backup, MF_BYCOMMAND | (begun ? MF_ENABLED : MF_GRAYED));
}
enum RowId {
    openRom = 1000,
    useRom,
    beginGame,
    backRow,
    chooseGb,
    chooseSave,
    removeGb,
    assignPad,
    padBindings,
    keyBindings,
    resetBindings,
    volumeRow,
    firstSlot = 1100,
    firstPad = 1200,
    firstBinding = 1300
};
struct MenuRow {
    int id;
    std::wstring label, value;
    bool choice;
};
std::vector<MenuRow> rows;
int cursor, scroll;
std::wstring heading, context;
void command(int);
void activate(int direction = 0);

void center() {
    if (!panel)
        return;
    RECT rc{};
    GetClientRect(window, &rc);
    SetWindowPos(panel, HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_NOACTIVATE);
}
void draw() {
    checkMenus();
    if (page == Page::play) {
        ShowWindow(panel, SW_HIDE);
        SetFocus(window);
        return;
    }
    rows.clear();
    context.clear();
    auto add = [](int id, std::wstring label, std::wstring value = L"", bool choice = false) {
        rows.push_back({id, std::move(label), std::move(value), choice});
    };
    switch (page) {
    case Page::home:
        heading = L"Kyujo2";
        add(home, L"Resume game");
        add(configurePaks, L"Game Boy cartridges (Game Paks)");
        add(configurePads, L"Controllers", std::to_wstring(pads::names().size()) + L" connected");
        add(choose, L"Stadium ROM and save file");
        add(settings, L"Options");
        add(about, L"About");
        add(restart, L"Restart game");
        add(leave, L"Quit");
        break;
    case Page::rom:
        heading = begun ? L"Stadium ROM and save" : L"Choose the Stadium 2 ROM";
        add(openRom, L"Choose Stadium 2 ROM");
        if (fs::exists(data / cartridge_file))
            add(useRom, L"Use imported ROM");
        if (begun) {
            add(backup, L"Back up Stadium save");
            add(folder, L"Open data folder");
        }
        add(begun ? backRow : leave, begun ? L"Back" : L"Exit");
        context = L"Choose the English (USA) Pokémon Stadium 2 cartridge.";
        break;
    case Page::paks:
        heading = begun ? L"Game Boy cartridges" : L"Game Pak Setup";
        for (int i = 0; i < 4; ++i) {
            auto group = L"Game Pak " + std::to_wstring(i + 1);
            auto rom = fs::path(get(group.c_str(), L"ROM"));
            add(firstSlot + i, L"Controller " + std::to_wstring(i + 1),
                rom.empty() ? L"Empty" : rom.stem().wstring());
        }
        if (begun)
            add(backRow, L"Back");
        else if (romValid)
            add(beginGame, L"Start game");
        else
            add(choose, L"Choose Stadium 2 ROM");
        context = begun ? L"Cartridge changes take effect after restarting the game."
                        : L"Select a port to insert a Game Boy cartridge, or start with empty slots.";
        break;
    case Page::pakSlot: {
        heading = L"Game Pak - Controller " + std::to_wstring(port + 1);
        auto rom = fs::path(get(pakSection().c_str(), L"ROM"));
        add(chooseGb, L"Game Boy ROM", rom.empty() ? L"Empty" : rom.filename().wstring());
        if (!rom.empty()) {
            auto save = fs::path(get(pakSection().c_str(), L"SaveLabel"));
            add(chooseSave, L"Save file", save.empty() ? L"New cartridge save" : save.filename().wstring());
            add(removeGb, L"Remove cartridge");
        }
        add(backRow, L"Back");
        break;
    }
    case Page::controllers: {
        heading = L"Controllers";
        auto names = pads::names();
        for (int i = 0; i < 4; ++i) {
            int index = pads::assigned(i);
            add(firstPad + i, L"Controller " + std::to_wstring(i + 1),
                index < 0 ? L"No gamepad" : names.at(index));
        }
        add(keyBindings, L"Keyboard", L"Controller 1");
        add(backRow, L"Back");
        break;
    }
    case Page::bindings: {
        heading = keyboardBinding ? L"Keyboard" : L"Controller " + std::to_wstring(port + 1);
        if (!keyboardBinding) {
            auto names = pads::names();
            int index = pads::assigned(port);
            add(assignPad, L"Assigned device", index < 0 ? L"No gamepad" : names.at(index), true);
        }
        for (unsigned i = 0; i < std::size(pads::actions); ++i)
            add(firstBinding + i, pads::actions[i], pads::label(port, i, keyboardBinding));
        add(resetBindings, L"Reset bindings");
        add(backRow, L"Back");
        auto v = pads::sample(port);
        wchar_t text[96];
        swprintf(text, std::size(text), L"Buttons %04X    Stick X %.2f    Y %.2f", unsigned(v.buttons),
                 double(v.x), double(v.y));
        context = text;
        break;
    }
    case Page::options:
        heading = L"Options";
        add(fullscreen, L"Full screen", SDL_GetWindowFlags(sdl) & SDL_WINDOW_FULLSCREEN ? L"On" : L"Off", true);
        add(volumeRow, L"Sound volume", std::to_wstring(gain.load()) + L"%", true);
        add(battleSettings, L"Battle cheats");
        add(backRow, L"Back");
        break;
    case Page::cheats:
        heading = L"Battle cheats";
        add(ko, L"Knockout (L + R)", cheatFlags.load() & 1 ? L"On" : L"Off", true);
        add(hp, L"First party Pokémon HP (Z)", cheatFlags.load() & 2 ? L"On" : L"Off", true);
        add(backRow, L"Back");
        break;
    default:
        break;
    }
    cursor = std::clamp(cursor, 0, int(rows.size()) - 1);
    scroll = std::clamp(scroll, std::max(0, cursor - 12), cursor);
    SetWindowTextW(panel, heading.c_str());
    ShowWindow(panel, SW_SHOWNA);
    InvalidateRect(panel, nullptr, FALSE);
}
void go(Page target) {
    pads::cancel_bind();
    if (target == Page::play && page != Page::play)
        blockHeldInput.fill(true);
    page = target;
    cursor = scroll = 0;
    note.clear();
    draw();
    SetFocus(window);
}
void back() {
    if (page == Page::pakSlot)
        go(Page::paks);
    else if (page == Page::bindings)
        go(Page::controllers);
    else if (page == Page::cheats)
        go(Page::options);
    else if (begun)
        go(page == Page::home ? Page::play : Page::home);
    else
        go(romValid ? Page::paks : Page::rom);
}
bool validate(const fs::path &p) {
    std::error_code ec;
    if (fs::file_size(p, ec) != (64u << 20) || ec ||
        recomp::select_rom(p, cartridge_id) != recomp::RomValidationError::Good) {
        note = L"Choose the 64 MiB USA Pokémon Stadium 2 ROM (.z64, .n64 or .v64). Check that the file and "
               L"data folder are accessible.";
        return false;
    }
    store(L"Setup", L"StadiumROM", p.wstring());
    romValid = true;
    return true;
}
void accepted() {
    if (begun) {
        go(Page::play);
        return;
    }
    if (get(L"Setup", L"GamePaksConfigured") == L"1")
        ready = true;
    else
        go(Page::paks);
}
fs::path newSavePath() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        throw std::runtime_error("Could not create the cartridge save folder.");
    wchar_t guid[42];
    StringFromGUID2(id, guid, 42);
    auto dir = data / L"Game Paks" / guid;
    fs::create_directories(dir);
    return dir / L"battery.sav";
}
void configurePak(bool chooseSave) {
    auto group = pakSection();
    fs::path rom = get(group.c_str(), L"ROM");
    if (!chooseSave) {
        rom = picker(L"Choose a Game Boy cartridge", gbfilter);
        if (rom.empty())
            return;
    }
    std::ifstream in(rom, std::ios::binary);
    std::array<unsigned char, 0x150> header{};
    in.read(reinterpret_cast<char *>(header.data()), header.size());
    unsigned char sum = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i)
        sum -= header[i] + 1;
    auto type = header[0x147];
    if (!in || sum != header[0x14d] || header[0x14a] != 1 || header[0x149] != 3 ||
        !(type == 3 || type == 0x10 || type == 0x13 || type == 0x1b)) {
        note = L"Choose an international Pokémon Game Boy cartridge with 32 KiB battery RAM.";
        return;
    }
    fs::path src = rom;
    src.replace_extension(L".sav");
    if (chooseSave) {
        src = picker(L"Import a Game Boy battery save", savefilter);
        if (src.empty())
            return;
    } else if (!fs::exists(src))
        src.clear();
    if (!chooseSave && rom == fs::path(get(group.c_str(), L"ROM")))
        return;
    auto dest = newSavePath();
    if (!src.empty()) {
        auto size = fs::file_size(src);
        if (size != 32768 && size != 32812 && size != 32816) {
            note = L"Expected 32 KiB of SRAM, optionally followed by a 44- or 48-byte clock trailer.";
            return;
        }
        std::array<char, 32768> bytes{};
        std::ifstream f(src, std::ios::binary);
        if (!f.read(bytes.data(), bytes.size()))
            throw std::runtime_error("Could not read the Game Boy save.");
        std::ofstream out(dest, std::ios::binary);
        out.write(bytes.data(), bytes.size());
        out.close();
        if (!out)
            throw std::runtime_error("Could not import the Game Boy save.");
        if (size > bytes.size())
            fs::copy_file(src, dest.parent_path() / L"original-with-clock.sav");
    }
    store(group.c_str(), L"Battery", dest.wstring());
    store(group.c_str(), L"SaveLabel", src.wstring());
    store(group.c_str(), L"ROM", rom.wstring());
    note = begun ? L"Cartridge configured for the next launch." : L"Cartridge configured.";
}
bool loadPaks() {
    for (int i = 0; i < 4; ++i) {
        auto group = L"Game Pak " + std::to_wstring(i + 1);
        fs::path rom = get(group.c_str(), L"ROM");
        if (rom.empty()) {
            host_gbpak_unload(i);
            continue;
        }
        auto save = utf8(fs::path(get(group.c_str(), L"Battery")));
        auto name = utf8(rom);
        if (save.size() > 980) {
            note = L"The cartridge save path is too long.";
            port = i;
            return false;
        }
        const char *err = host_gbpak_load(i, name.c_str(), save.c_str());
        if (err) {
            port = i;
            note = widen(err);
            return false;
        }
    }
    return true;
}
void command(int id) {
    switch (id) {
    case choose:
        go(Page::rom);
        break;
    case configurePads:
        go(Page::controllers);
        break;
    case configurePaks:
        go(Page::paks);
        break;
    case battleSettings:
        go(Page::cheats);
        break;
    case settings:
        go(Page::options);
        break;
    case home:
        if (begun)
            go(page == Page::play ? Page::home : Page::play);
        break;
    case fullscreen: {
        bool enable = !(SDL_GetWindowFlags(sdl) & SDL_WINDOW_FULLSCREEN);
        if (!SDL_SetWindowFullscreen(sdl, enable ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0))
            store(L"Options", L"Fullscreen", enable ? L"1" : L"0");
        checkMenus();
        center();
        if (page != Page::play)
            draw();
        break;
    }
    case ko:
    case hp: {
        auto flags = cheatFlags.load() ^ (id == ko ? 1u : 2u);
        cheatFlags = flags;
        store(L"Options", L"BattleCheats", std::to_wstring(flags));
        if (page == Page::cheats)
            draw();
        else
            checkMenus();
        break;
    }
    case backup: {
        auto current = data / L"saves" / L"stadium2-us.bin";
        if (!fs::exists(current)) {
            message(L"Play and save in Stadium 2 before creating a backup.");
            break;
        }
        auto out = picker(L"Back up Stadium 2 FlashRAM", L"Stadium save (*.bin)\0*.bin\0", true);
        if (!out.empty())
            fs::copy_file(current, out, fs::copy_options::overwrite_existing);
        break;
    }
    case folder:
        ShellExecuteW(window, L"open", data.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case restart:
        if (begun &&
            MessageBoxW(window,
                        L"Restart Stadium 2 using the configured cartridges? Unsaved progress will be lost.",
                        L"Kyujo2", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            relaunch = true;
            ultramodern::quit();
        }
        break;
    case leave:
        if (begun)
            ultramodern::quit();
        else
            cancelled = true;
        break;
    case about:
        message(L"Kyujo2 • Pokémon Stadium 2 (USA)\nF1 or Esc opens the menu. F11 toggles full "
                L"screen.\n\nSettings, saves and log:\n" +
                data.wstring());
        break;
    }
}
void activate(int direction) {
    if (rows.empty() || pads::binding())
        return;
    auto row = rows.at(cursor);
    if (direction && !row.choice)
        return;
    note.clear();
    int id = row.id, step = direction < 0 ? -1 : 1;
    if (id >= firstBinding) {
        if (!keyboardBinding && pads::assigned(port) < 0) {
            note = L"Assign a gamepad first.";
            draw();
            return;
        }
        pads::begin_bind(port, id - firstBinding, keyboardBinding);
        note = keyboardBinding ? L"Press the new key. Esc cancels."
                               : L"Press a gamepad button or move an axis. Esc cancels.";
    } else if (id >= firstPad) {
        port = id - firstPad;
        keyboardBinding = false;
        go(Page::bindings);
        return;
    } else if (id >= firstSlot) {
        port = id - firstSlot;
        go(Page::pakSlot);
        return;
    } else
        switch (id) {
        case backRow:
            back();
            return;
        case openRom:
        case useRom: {
            auto p =
                id == openRom ? picker(L"Select Pokémon Stadium 2 (USA)", n64filter) : data / cartridge_file;
            if (!p.empty() && validate(p))
                accepted();
            break;
        }
        case chooseGb:
        case chooseSave:
            configurePak(id == chooseSave);
            break;
        case removeGb:
            store(pakSection().c_str(), L"ROM", L"");
            store(pakSection().c_str(), L"SaveLabel", L"");
            break;
        case beginGame:
            ready = romValid;
            break;
        case keyBindings:
            port = 0;
            keyboardBinding = true;
            go(Page::bindings);
            return;
        case resetBindings:
            pads::defaults(port, keyboardBinding);
            break;
        case assignPad: {
            int count = int(pads::names().size()) + 1;
            pads::assign(port, (pads::assigned(port) + 1 + step + count) % count - 1);
            break;
        }
        case volumeRow:
            gain = (gain.load() + 25 * step + 125) % 125;
            store(L"Options", L"Volume", std::to_wstring(gain.load()));
            break;
        default:
            command(id);
            break;
        }
    draw();
}
void errorDialog(const std::exception &e) {
    MessageBoxW(window, widen(e.what()).c_str(), L"Kyujo2", MB_ICONERROR | MB_OK);
}
void navigate(int delta) {
    if (rows.empty() || pads::binding())
        return;
    cursor = (cursor + delta + int(rows.size())) % int(rows.size());
    draw();
}

std::string menuText(const std::wstring &s, int size, int width) {
    std::string result;
    for (auto ch : s) {
        if (ch == L'\n' || ch == L'\r')
            ch = L' ';
        result.push_back(ch >= 32 && ch <= 255 ? char(ch) : '?');
        if (host_text_width(size, result.c_str()) > width) {
            result.pop_back();
            break;
        }
    }
    return result;
}
RECT canvasRect(HWND h) {
    RECT rc{};
    GetClientRect(h, &rc);
    int w = std::min<int>(rc.right, rc.bottom * 4 / 3), hgt = w * 3 / 4;
    int x = (rc.right - w) / 2, y = (rc.bottom - hgt) / 2;
    return {x, y, x + w, y + hgt};
}
void paint(HWND h, HDC dc) {
    constexpr int w = 640, hgt = 480;
    static std::vector<unsigned char> base(w * hgt * 3), pixels;
    static bool initialized = false;
    if (!initialized) {
        HostCanvas bg{base.data(), w, hgt, 65536, 65536};
        menu_background(&bg);
        initialized = true;
    }
    pixels = base;
    HostCanvas cv{pixels.data(), w, hgt, 65536, 65536};
    auto title = menuText(heading, 4, 534);
    bool centered = page == Page::home || (!begun && (page == Page::rom || page == Page::paks));
    host_text_draw(&cv, 4, centered ? (640 - host_text_width(4, title.c_str())) / 2 : 52, 36, title.c_str(),
                   0xffffff, 1);
    for (int i = scroll; i < int(rows.size()) && i < scroll + 13; ++i) {
        const auto &row = rows[i];
        int y = 84 + (i - scroll) * 22;
        bool sel = i == cursor;
        if (sel)
            menu_cursor(&cv, y);
        auto color = sel ? 0xffe680u : 0xffffffu;
        auto label = menuText(row.label, 2, row.value.empty() ? 520 : 290);
        host_text_draw(&cv, 2, 62, y, label.c_str(), color, 1);
        auto val = menuText(row.value, 2, 205);
        if (row.choice && sel)
            val = "< " + val + " >";
        host_text_draw(&cv, 2, 578 - host_text_width(2, val.c_str()), y, val.c_str(), color, 1);
    }
    auto status = menuText(note.empty() ? context : note, 2, 540);
    host_text_draw(&cv, 2, 52, 396, status.c_str(), note.empty() ? 0xb4c0d8 : 0xffe680, 1);
    host_text_draw(&cv, 2, 52, 424, "Up/Down: select   Enter / A: choose   Left/Right: change   Esc / B: back",
                   0xb4c0d8, 1);
    for (size_t i = 0; i < pixels.size(); i += 3)
        std::swap(pixels[i], pixels[i + 2]);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -hgt;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    RECT rc{};
    GetClientRect(h, &rc);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    auto r = canvasRect(h);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, w, hgt, pixels.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
}
LRESULT CALLBACK panelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_PRINTCLIENT) {
            paint(h, reinterpret_cast<HDC>(wp));
            return 0;
        }
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_LBUTTONUP && !pads::binding()) {
            auto r = canvasRect(h);
            if (r.right == r.left || r.bottom == r.top)
                return 0;
            int x = (int(short(LOWORD(lp))) - r.left) * 640 / (r.right - r.left);
            int y = (int(short(HIWORD(lp))) - r.top) * 480 / (r.bottom - r.top);
            int i = scroll + (y - 82) / 22;
            if (x >= 44 && x < 594 && y >= 82 && y < 82 + 13 * 22 && i < int(rows.size())) {
                cursor = i;
                activate(rows[i].choice && x > 350 && x < 470 ? -1 : 0);
            }
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            navigate(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        }
    } catch (const std::exception &e) {
        errorDialog(e);
    }
    return DefWindowProcW(h, msg, wp, lp);
}
LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND && !l) {
        try {
            command(LOWORD(w));
        } catch (const std::exception &e) {
            errorDialog(e);
        }
        return 0;
    }
    if (msg == WM_SIZE)
        center();
    if (msg == WM_CLOSE && !begun) {
        cancelled = true;
        return 0;
    }
    return DefSubclassProc(h, msg, w, l);
}
} // namespace
bool setup(SDL_Window *s, const fs::path &root, const fs::path &argument) {
    std::vector<wchar_t> absolute(32768);
    DWORD length = GetFullPathNameW(root.c_str(), absolute.size(), absolute.data(), nullptr);
    if (!length || length >= absolute.size())
        throw std::runtime_error("Invalid data directory.");
    data = absolute.data();
    ini = data / L"Kyujo2.json";
    sdl = s;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(s, &info))
        return false;
    window = info.info.win.window;
    try {
        SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_CLIPCHILDREN);
        menu();
        SetWindowSubclass(window, windowProc, 1, 0);
        WNDCLASSW wc{};
        wc.lpfnWndProc = panelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Kyujo2MenuCanvas";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the menu canvas.");
        panel = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1, window, nullptr, wc.hInstance,
                                nullptr);
        if (!panel)
            throw std::runtime_error("Could not create the menu canvas.");
        center();
        HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
        if (!stderrHandle || stderrHandle == INVALID_HANDLE_VALUE)
            _wfreopen((data / L"Kyujo2.log").c_str(), L"w", stderr);
        pads::init(ini);
        gain = std::clamp(int(prefs::number(L"Options", L"Volume", 100)), 0, 100);
        cheatFlags = prefs::number(L"Options", L"BattleCheats", 0) & 3;
        page = Page::rom;
        auto cached = data / cartridge_file;
        if (!argument.empty()) {
            if (validate(argument))
                accepted();
        } else if (!get(L"Setup", L"StadiumROM").empty() && fs::exists(cached)) {
            if (validate(cached))
                accepted();
        }
        if (!ready)
            draw();
        do {
            while (!ready && !cancelled) {
                SDL_Event e;
                if (SDL_WaitEventTimeout(&e, 16)) {
                    update(e);
                    while (SDL_PollEvent(&e))
                        update(e);
                }
                poll();
            }
            if (cancelled)
                return false;
            if (loadPaks())
                break;
            page = Page::paks;
            ready = false;
            draw();
        } while (true);
        store(L"Setup", L"GamePaksConfigured", L"1");
        begun = true;
        go(Page::play);
        if (get(L"Options", L"Fullscreen") == L"1")
            SDL_SetWindowFullscreen(s, SDL_WINDOW_FULLSCREEN_DESKTOP);
        return true;
    } catch (const std::exception &e) {
        errorDialog(e);
        return false;
    }
}
void update(const SDL_Event &e) {
    if (e.type == SDL_QUIT) {
        if (!begun)
            cancelled = true;
        return;
    }
    if (e.type == SDL_JOYDEVICEADDED || e.type == SDL_JOYDEVICEREMOVED) {
        pads::refresh();
        if ((page == Page::controllers || page == Page::bindings) && !pads::binding())
            draw();
    }
    if (pads::capture(e)) {
        note.clear();
        confirmHeld = true;
        draw();
        return;
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        if (e.key.keysym.sym == SDLK_F11)
            command(fullscreen);
        if (e.key.keysym.sym == SDLK_F1)
            command(home);
        else if (e.key.keysym.sym == SDLK_ESCAPE) {
            if (page == Page::play)
                command(home);
            else
                back();
        } else if (page != Page::play) {
            switch (e.key.keysym.sym) {
            case SDLK_UP:
                navigate(-1);
                break;
            case SDLK_DOWN:
                navigate(1);
                break;
            case SDLK_LEFT:
                activate(-1);
                break;
            case SDLK_RIGHT:
                activate(1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                activate();
                break;
            case SDLK_BACKSPACE:
                back();
                break;
            default:
                break;
            }
        }
    }
    if (e.type == SDL_DROPFILE) {
        fs::path p = fs::u8path(e.drop.file);
        SDL_free(e.drop.file);
        if (page == Page::rom) {
            if (validate(p))
                accepted();
            else
                draw();
        }
    }
}
void poll() {
    bool menuKey = false, up = false, down = false, left = false, right = false, backKey = false,
         accept = false;
    for (int i = 0; i < 4; ++i) {
        pads::State v = pads::sample(i);
        online[i] = pads::connected(i) || host_gbpak_present(i);
        auto menuInput = pads::sample(i, false);
        if (i == 0 && page == Page::bindings) {
            static uint32_t last;
            auto now = SDL_GetTicks();
            if (now - last > 150) {
                last = now;
                draw();
            }
        }
        if (page != Page::play) {
            up |= (menuInput.buttons & 0x0800) != 0 || menuInput.y > .55f;
            down |= (menuInput.buttons & 0x0400) != 0 || menuInput.y < -.55f;
            accept |= (menuInput.buttons & 0x8000) != 0;
            left |= (menuInput.buttons & 0x0200) != 0;
            right |= (menuInput.buttons & 0x0100) != 0;
            backKey |= (menuInput.buttons & 0x4000) != 0;
            v = {};
        } else if (blockHeldInput[i]) {
            blockHeldInput[i] = v.buttons != 0;
            if (blockHeldInput[i])
                v = {};
        }
        samples[i] = uint64_t(v.buttons) | (uint64_t(uint16_t(int16_t(v.x * 32767))) << 16) |
                     (uint64_t(uint16_t(int16_t(v.y * 32767))) << 32);
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        auto *c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (c)
            menuKey |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) != 0;
    }
    if (menuKey && !menuHeld && !pads::binding())
        command(home);
    menuHeld = menuKey;
    if (page != Page::play && !pads::binding()) {
        if (up && !upHeld)
            navigate(-1);
        if (down && !downHeld)
            navigate(1);
        if (left && !leftHeld)
            activate(-1);
        if (right && !rightHeld)
            activate(1);
        if (backKey && !backHeld)
            back();
        else if (accept && !confirmHeld)
            activate();
    }
    upHeld = up;
    downHeld = down;
    leftHeld = left;
    rightHeld = right;
    backHeld = backKey;
    confirmHeld = accept;
}
bool read_input(int port, uint16_t *b, float *x, float *y) {
    if (!connected(port))
        return false;
    uint64_t v = samples[port].load();
    *b = uint16_t(v);
    *x = int16_t(v >> 16) / 32767.f;
    *y = int16_t(v >> 32) / 32767.f;
    if (port == 0 && guestRam && stadium2_battle_loaded())
        stadium2_battle_cheats(guestRam, cheatFlags.load(), *b);
    if (std::getenv("KYUJO2_TRACE_INPUT")) {
        static thread_local uint16_t prev[4]{};
        if (prev[port] != *b) {
            std::fprintf(stderr, "N64 controller %d sampled buttons %04x\n", port + 1, *b);
            prev[port] = *b;
        }
    }
    return true;
}
bool connected(int p) {
    return p >= 0 && p < 4 && (p == 0 || online[p]);
}
bool pak_present(int p) {
    return p >= 0 && p < 4 && host_gbpak_present(p);
}
void start_guest(uint8_t *rdram) {
    guestRam = rdram;
}
int volume() {
    return gain.load();
}
void report(const char *msg) {
    MessageBoxW(window, widen(msg).c_str(), L"Kyujo2", MB_OK | MB_ICONERROR);
}
void finish() {
    if (relaunch) {
        std::vector<wchar_t> exe(32768);
        GetModuleFileNameW(nullptr, exe.data(), exe.size());
        std::wstring params = L"\"" + (data / cartridge_file).wstring() + L"\" \"" + data.wstring() + L"\"";
        if (reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", exe.data(), params.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
            message(L"Could not restart Kyujo2.");
    }
}
} // namespace winui
