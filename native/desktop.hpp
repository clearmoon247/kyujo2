#pragma once
#include "librecomp/game.hpp"
void configure_desktop(recomp::Configuration& config);
std::filesystem::path default_data_directory();
std::filesystem::path choose_rom();
// Stops the GPU back end (when in use) once the game has stopped.
void desktop_stop_renderer();
#ifdef __SWITCH__
void show_missing_switch_rom();
void desktop_shutdown();
#endif

#ifdef KYUJO2_SHARED_MENU
struct SDL_Window;
SDL_Window *desktop_window();
// Shows a picture through the GPU back end when it is in use; false: the caller shows it with SDL's window surface.
bool desktop_present_rgb(const unsigned char *rgb, unsigned w, unsigned h);
#endif
