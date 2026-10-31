#pragma once
#include <windows.h>
#include <atomic>

extern std::atomic<bool> g_running;

void stop_feature_threads();

// exit the process right now, no cleanup
inline void instant_terminate(int exit_code = 0) {
    TerminateProcess(GetCurrentProcess(), exit_code);

    ExitProcess(exit_code);
}