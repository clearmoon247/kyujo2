// The run's log file and a last line for fatal faults.
//
// Windows starts Kyujo2 as a GUI program without a console and Android sends stderr nowhere, so the runtime's
// messages ("Failed to find function at ...", assertion failures, RSP exits) were lost exactly when a game
// crashed. Both streams now go to kyujo-sdl-log.txt in the data folder; the previous run's log is kept as
// kyujo-sdl-log.prev.txt. Android's "Save logs" menu entry copies these two files.
#include "crash_log.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
LONG WINAPI unhandled(EXCEPTION_POINTERS *info) {
    const auto *record = info->ExceptionRecord;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(record->ExceptionAddress), &module);
    if (module) GetModuleFileNameA(module, name, sizeof name);
    std::fprintf(stderr, "Kyujo2: fatal exception %08lx at %p (%s + 0x%llx)\n", record->ExceptionCode,
                 record->ExceptionAddress, name,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) -
                                                 reinterpret_cast<uintptr_t>(module)));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
void aborted(int) {
    std::fputs("Kyujo2: the program stopped itself (abort). The lines above give the reason.\n", stderr);
    std::fflush(stderr);
}
#elif !defined(__SWITCH__)
void fatal(int signal, siginfo_t *info, void *) {
    // Only async-signal-safe calls here.
    char line[96] = "Kyujo2: fatal signal ";
    char *p = line + std::strlen(line);
    auto put = [&p](unsigned long long value, int base) {
        char digits[24];
        int n = 0;
        do { digits[n++] = "0123456789abcdef"[value % base]; value /= base; } while (value && n < 24);
        while (n) *p++ = digits[--n];
    };
    put(static_cast<unsigned>(signal), 10);
    std::memcpy(p, " at 0x", 6); p += 6;
    put(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr), 16);
    *p++ = '\n';
    ssize_t ignored = write(2, line, static_cast<size_t>(p - line));
    (void)ignored;
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
#endif
}

void start_log_file(const std::filesystem::path &directory) {
    namespace fs = std::filesystem;
    bool redirect = true;
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__SWITCH__)
    // A desktop Linux build started from a terminal keeps writing there.
    redirect = !isatty(2) || std::getenv("KYUJO2_LOG_FILE");
#endif
    if (redirect) {
        const auto log = directory / "kyujo-sdl-log.txt";
        const auto previous = directory / "kyujo-sdl-log.prev.txt";
        std::error_code ec;
        fs::remove(previous, ec);
        fs::rename(log, previous, ec);
        // Both streams append, so their writes cannot overwrite each other.
#ifdef _WIN32
        _wfreopen(log.c_str(), L"a", stderr);
        _wfreopen(log.c_str(), L"a", stdout);
#else
        std::freopen(log.c_str(), "a", stderr);
        std::freopen(log.c_str(), "a", stdout);
#endif
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
    const std::time_t now = std::time(nullptr);
    char stamp[64] = "";
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "Kyujo2 %s, started %s\n", KYUJO2_REVISION, stamp);
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled);
    std::signal(SIGABRT, aborted);
#elif !defined(__SWITCH__)
    struct sigaction action {};
    action.sa_sigaction = fatal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(signal, &action, nullptr);
#endif
}
