#pragma once
#include <filesystem>
#include <cstdint>
struct SDL_Window;
namespace shared_menu {
bool setup(SDL_Window *, const std::filesystem::path &data, const std::filesystem::path &rom, int argc, char **argv);
void poll();
void start_guest(uint8_t *ram);
bool input(int port, uint16_t *b, float *x, float *y);
bool connected(int port);
int volume();
int scaling();
bool wide();
void finish();
}
