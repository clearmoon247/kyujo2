#pragma once
#include <SDL.h>
#include <array>
#include <filesystem>
#include <cstdint>

namespace winui {
bool setup(SDL_Window *window, const std::filesystem::path &data, const std::filesystem::path &argument);
void update(const SDL_Event &event);
void poll();
bool read_input(int port, uint16_t *buttons, float *x, float *y);
bool connected(int port);
bool pak_present(int port);
void start_guest(uint8_t *rdram);
void finish();
int volume();
void report(const char *message);
} // namespace winui
