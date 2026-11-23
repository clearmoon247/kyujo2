#include "windows_input.hpp"
#include "windows_settings.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace pads {
namespace {
constexpr int raw_axis = 1024, raw_hat = 2048;
constexpr std::array<uint16_t, 14> masks{1, 2, 4, 8, 16, 32, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
constexpr std::array<int, 18> keys{SDL_SCANCODE_L,  SDL_SCANCODE_J,   SDL_SCANCODE_K,      SDL_SCANCODE_I,
                                   SDL_SCANCODE_E,  SDL_SCANCODE_Q,   SDL_SCANCODE_D,      SDL_SCANCODE_A,
                                   SDL_SCANCODE_S,  SDL_SCANCODE_W,   SDL_SCANCODE_RETURN, SDL_SCANCODE_C,
                                   SDL_SCANCODE_X,  SDL_SCANCODE_Z,   SDL_SCANCODE_LEFT,   SDL_SCANCODE_RIGHT,
                                   SDL_SCANCODE_UP, SDL_SCANCODE_DOWN};
constexpr int axis(int number, bool positive) {
    return raw_axis + number * 2 + int(positive);
}
constexpr int hat(int direction) {
    return raw_hat + direction;
}
constexpr std::array<int, 18> standard{axis(2, true),
                                       axis(2, false),
                                       axis(3, true),
                                       axis(3, false),
                                       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                                       SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                                       SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                       SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                                       SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                       SDL_CONTROLLER_BUTTON_DPAD_UP,
                                       SDL_CONTROLLER_BUTTON_START,
                                       axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true),
                                       SDL_CONTROLLER_BUTTON_B,
                                       SDL_CONTROLLER_BUTTON_A,
                                       axis(0, false),
                                       axis(0, true),
                                       axis(1, false),
                                       axis(1, true)};
constexpr std::array<int, 18> generic{axis(2, true),
                                      axis(2, false),
                                      axis(3, true),
                                      axis(3, false),
                                      5,
                                      4,
                                      hat(SDL_HAT_RIGHT),
                                      hat(SDL_HAT_LEFT),
                                      hat(SDL_HAT_DOWN),
                                      hat(SDL_HAT_UP),
                                      7,
                                      6,
                                      1,
                                      0,
                                      axis(0, false),
                                      axis(0, true),
                                      axis(1, false),
                                      axis(1, true)};
struct Device {
    SDL_Joystick *joy{};
    SDL_GameController *pad{};
    SDL_JoystickID id{};
    std::wstring name, section;
    std::array<int, 18> map{};
};
std::vector<Device> devices;
std::array<SDL_JoystickID, 4> ports{-1, -1, -1, -1};

std::array<int, 18> keyboard;
int capture_port = -1;
unsigned capture_action;
bool capture_keyboard;
std::wstring wide(const char *s) {
    if (!s)
        return L"Gamepad";
    return std::filesystem::u8path(s).wstring();
}
void load(std::array<int, 18> &map, const std::wstring &group) {
    for (unsigned i = 0; i < map.size(); ++i)
        map[i] = prefs::number(group.c_str(), actions[i], map[i]);
}
Device *device(int port) {
    auto it =
        std::find_if(devices.begin(), devices.end(), [&](const Device &d) { return d.id == ports.at(port); });
    return it == devices.end() ? nullptr : &*it;
}
float value(Device &d, int code) {
    if (code >= raw_hat) {
        int hat_index = (code - raw_hat) / 16;
        return (SDL_JoystickGetHat(d.joy, hat_index) & ((code - raw_hat) % 16)) ? 1.f : 0.f;
    }
    if (code >= raw_axis) {
        int a = (code - raw_axis) / 2;
        float v = (d.pad ? SDL_GameControllerGetAxis(d.pad, static_cast<SDL_GameControllerAxis>(a))
                         : SDL_JoystickGetAxis(d.joy, a)) /
                  32768.f;
        return std::max(0.f, (code & 1) ? v : -v);
    }
    return d.pad ? SDL_GameControllerGetButton(d.pad, static_cast<SDL_GameControllerButton>(code))
                 : SDL_JoystickGetButton(d.joy, code);
}
} // namespace
void init(const std::filesystem::path &ini) {
    prefs::open(ini);
    keyboard = keys;
    load(keyboard, L"Keyboard");
    refresh();
}
void refresh() {
    for (auto it = devices.begin(); it != devices.end();) {
        if (SDL_JoystickGetAttached(it->joy)) {
            ++it;
            continue;
        }
        for (auto &p : ports)
            if (p == it->id)
                p = -1;
        if (it->pad)
            SDL_GameControllerClose(it->pad);
        else
            SDL_JoystickClose(it->joy);
        it = devices.erase(it);
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        auto id = SDL_JoystickGetDeviceInstanceID(i);
        if (std::any_of(devices.begin(), devices.end(), [&](const Device &d) { return d.id == id; }))
            continue;
        Device d;
        if (SDL_IsGameController(i))
            d.pad = SDL_GameControllerOpen(i);
        d.joy = d.pad ? SDL_GameControllerGetJoystick(d.pad) : SDL_JoystickOpen(i);
        if (!d.joy)
            continue;
        d.id = id;
        d.name = wide(SDL_JoystickName(d.joy));
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(d.joy), guid, sizeof(guid));
        d.section = L"Pad " + wide(guid) + (d.pad ? L" mapped" : L" raw");
        d.map = d.pad ? standard : generic;
        load(d.map, d.section);
        int pref = prefs::number(d.section.c_str(), L"Port", 0);
        auto slot = std::find(ports.begin(), ports.end(), -1);
        if (pref >= 1 && pref <= 4 && ports[pref - 1] == -1)
            ports[pref - 1] = id;
        else if (pref >= 0 && slot != ports.end())
            *slot = id;
        devices.push_back(std::move(d));
    }
}
std::vector<std::wstring> names() {
    std::vector<std::wstring> n;
    for (auto &d : devices)
        n.push_back(d.name + (d.pad ? L"" : L" (generic joystick)"));
    return n;
}
int assigned(int port) {
    auto *d = device(port);
    return d ? int(d - devices.data()) : -1;
}
void assign(int port, int index) {
    auto next = index >= 0 ? devices.at(index).id : -1;
    auto *previous = device(port);
    auto old = std::find(ports.begin(), ports.end(), next);
    if (next != -1 && old != ports.end())
        std::swap(*old, ports.at(port));
    ports.at(port) = next;
    if (previous && std::find(ports.begin(), ports.end(), previous->id) == ports.end())
        prefs::set(previous->section.c_str(), L"Port", L"-1");
    for (int i = 0; i < 4; ++i)
        if (auto *d = device(i))
            prefs::set(d->section.c_str(), L"Port", std::to_wstring(i + 1));
}
bool connected(int port) {
    return port == 0 || device(port);
}
State sample(int port, bool includeKeyboard) {
    State s;
    Device *d = device(port);
    const auto *key = SDL_GetKeyboardState(nullptr);
    std::array<float, 18> vals{};
    for (unsigned i = 0; i < vals.size(); ++i) {
        float v = d ? value(*d, d->map[i]) : 0.f;
        if (includeKeyboard && !port && keyboard[i] > 0 && keyboard[i] < SDL_NUM_SCANCODES && key[keyboard[i]])
            v = 1;
        vals[i] = v;
        if (i < masks.size() && v > 0.35f)
            s.buttons |= masks[i];
    }
    s.x = vals[15] - vals[14];
    s.y = vals[16] - vals[17];
    if (std::abs(s.x) < .12f)
        s.x = 0;
    if (std::abs(s.y) < .12f)
        s.y = 0;
    return s;
}
void begin_bind(int port, unsigned a, bool kb) {
    capture_port = port;
    capture_action = a;
    capture_keyboard = kb;
}
bool binding() {
    return capture_port >= 0;
}
void cancel_bind() {
    capture_port = -1;
}
bool capture(const SDL_Event &e) {
    if (!binding())
        return false;
    int code = -1;
    auto *d = device(capture_port);
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        cancel_bind();
        return true;
    }
    if (capture_keyboard && e.type == SDL_KEYDOWN && !e.key.repeat)
        code = e.key.keysym.scancode;
    if (!capture_keyboard && d) {
        if (d->pad && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.which == d->id)
            code = e.cbutton.button;
        if (d->pad && e.type == SDL_CONTROLLERAXISMOTION && e.caxis.which == d->id &&
            std::abs(int(e.caxis.value)) > 22000)
            code = axis(e.caxis.axis, e.caxis.value > 0);
        if (!d->pad && e.type == SDL_JOYBUTTONDOWN && e.jbutton.which == d->id)
            code = e.jbutton.button;
        if (!d->pad && e.type == SDL_JOYHATMOTION && e.jhat.which == d->id && e.jhat.value)
            code = raw_hat + e.jhat.hat * 16 + e.jhat.value;
        if (!d->pad && e.type == SDL_JOYAXISMOTION && e.jaxis.which == d->id &&
            std::abs(int(e.jaxis.value)) > 22000)
            code = axis(e.jaxis.axis, e.jaxis.value > 0);
    }
    if (code < 0)
        return false;
    (capture_keyboard ? keyboard : d->map)[capture_action] = code;
    prefs::set(capture_keyboard ? L"Keyboard" : d->section.c_str(), actions[capture_action],
               std::to_wstring(code));
    cancel_bind();
    return true;
}
void defaults(int port, bool kb) {
    auto *d = device(port);
    if (!kb && !d)
        return;
    (kb ? keyboard : d->map) = kb ? keys : (d->pad ? standard : generic);
    for (auto name : actions)
        prefs::set(kb ? L"Keyboard" : d->section.c_str(), name, L"");
}
std::wstring label(int port, unsigned a, bool kb) {
    auto *d = device(port);
    if (!kb && !d)
        return L"";
    int c = kb ? keyboard[a] : d->map[a];
    if (kb)
        return wide(SDL_GetScancodeName(static_cast<SDL_Scancode>(c)));
    if (c >= raw_hat)
        return L"Hat " + std::to_wstring((c - raw_hat) / 16 + 1) + L" direction " +
               std::to_wstring((c - raw_hat) % 16);
    if (c >= raw_axis)
        return L"Axis " + std::to_wstring((c - raw_axis) / 2 + 1) + (c & 1 ? L"+" : L"-");
    return d->pad ? wide(SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(c)))
                  : L"Button " + std::to_wstring(c + 1);
}
} // namespace pads
