#pragma once
#include <filesystem>
#ifndef KYUJO2_REVISION
#define KYUJO2_REVISION "development build"
#endif
// Sends stderr and stdout to kyujo-sdl-log.txt in `directory` (keeping the previous run's log) and records fatal
// faults there. A Linux build started from a terminal keeps its terminal unless KYUJO2_LOG_FILE is set.
void start_log_file(const std::filesystem::path &directory);
