#include "app/Application.h"

#ifdef _WIN32
#include <windows.h>

// Request high-performance GPU on dual-GPU systems (NVIDIA Optimus / AMD Switchable)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
// Use WinMain to avoid console window, but also support console
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Attach to parent console if launched from terminal
    AttachConsole(ATTACH_PARENT_PROCESS);

    Application app;
    if (!app.init()) return 1;
    app.run();
    app.shutdown();
    return 0;
}
#else
int main() {
    Application app;
    if (!app.init()) return 1;
    app.run();
    app.shutdown();
    return 0;
}
#endif
