#include <windows.h>
#include "common/shared_control.h"

namespace {

HMODULE g_module = nullptr;
grw::ControlMapping g_control;

DWORD WINAPI MainThread(LPVOID) {
    if (!g_control.Open(GetCurrentProcessId()) || g_control.Get()->magic != grw::kControlMagic) {
        g_control.Close();
        FreeLibraryAndExitThread(g_module, 1);
    }
    grw::SharedControl* c = g_control.Get();

    InterlockedExchange(&c->loaded, 1);
    while (InterlockedCompareExchange(&c->unload_request, 0, 0) == 0) Sleep(100);

    InterlockedExchange(&c->unload_request, 0);
    InterlockedExchange(&c->loaded, 0);
    g_control.Close();
    FreeLibraryAndExitThread(g_module, 0);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
