#include <windows.h>
#include <cstdio>
#include "common/shared_control.h"
#include "core/clock.h"
#include "dll/hooks.h"
#include "dll/overlay.h"
#include "dll/hwbp.h"
#include "dll/runtime.h"

namespace {

HMODULE g_module = nullptr;
grw::ControlMapping g_control;

[[noreturn]] void Exit(DWORD code) {
    if (grw::SharedControl* c = g_control.Get()) InterlockedExchange(&c->loaded, 0);
    g_control.Close();
    FreeLibraryAndExitThread(g_module, code);
}

DWORD WINAPI MainThread(LPVOID) {
    if (!g_control.Open(GetCurrentProcessId()) || g_control.Get()->magic != grw::kControlMagic) Exit(1);
    grw::SharedControl* c = g_control.Get();

    grw::Clock::Calibrate();
    grw::Runtime& rt = grw::Rt();
    if (!rt.Start(c->base_dir, c)) Exit(1);

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    rt.Status("inject", exe);
    // The linker's PE timestamp changes on every link: proves which build is loaded.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(g_module) + dos->e_lfanew);
    char build[32];
    std::snprintf(build, sizeof build, "link 0x%08lx", nt->FileHeader.TimeDateStamp);
    rt.Status("build", build);
    const auto method = grw::hooks::Method(rt.Cfg().hook_method);
    const bool hooked = grw::hooks::Install([](IDXGISwapChain* sc) { grw::Rt().OnFrame(sc); },
                                            [](IDXGISwapChain* sc, bool before) { grw::Rt().OnResize(sc, before); },
                                            [](double cpu, double wall) { grw::Rt().OnPresented(cpu, wall); },
                                            method);
    const char* const kHow[] = {"inline", "vtable", "dummy only (no hooks)", "none (no hooks)"};
    const char* how = kHow[int(method)];
    char hooks_detail[48];
    std::snprintf(hooks_detail, sizeof hooks_detail, "%s %s", hooked ? "ok" : "failed", how);
    rt.Status("hooks", hooks_detail);

    InterlockedExchange(&c->loaded, 1);
    while (InterlockedCompareExchange(&c->unload_request, 0, 0) == 0) Sleep(100);

    grw::overlay::ReleaseBackBuffer(500);
    if (hooked) grw::hooks::Uninstall();
    grw::overlay::Shutdown();
    char detail[64];
    std::snprintf(detail, sizeof detail, "presents=%llu", grw::hooks::PresentCalls());
    rt.Status("unload", detail);
    rt.Stop();
    // after the writer thread (the only caller of Configure) has stopped, before this module unloads
    grw::hwbp::Shutdown();
    InterlockedExchange(&c->unload_request, 0);
    Exit(0);
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
