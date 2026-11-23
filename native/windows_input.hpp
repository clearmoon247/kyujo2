#pragma once
#include <SDL.h>
#include <string>
#include <filesystem>
#include <vector>

namespace pads {
inline constexpr const wchar_t *actions[] = {
    L"C Right",     L"C Left",     L"C Down",     L"C Up",        L"R",        L"L",
    L"D-pad Right", L"D-pad Left", L"D-pad Down", L"D-pad Up",    L"Start",    L"Z",
    L"B",           L"A",          L"Stick Left", L"Stick Right", L"Stick Up", L"Stick Down"};
struct State {
    uint16_t buttons{};
    float x{}, y{};
};
void init(const std::filesystem::path &ini);
void refresh();
std::vector<std::wstring> names();
int assigned(int port);
void assign(int port, int device);
bool connected(int port);
State sample(int port, bool includeKeyboard = true);
void begin_bind(int port, unsigned action, bool keyboard);
bool capture(const SDL_Event &event);
void cancel_bind();
bool binding();
void defaults(int port, bool keyboard);
std::wstring label(int port, unsigned action, bool keyboard);
} // namespace pads
