#include "dll/hooks.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <atomic>
#include "core/clock.h"

namespace grw::hooks {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeFn g_resize = nullptr;
FrameCallback g_on_frame = nullptr;
ResizeCallback g_on_resize = nullptr;
PresentedCallback g_on_presented = nullptr;
std::atomic<int> g_inflight{0};
std::atomic<uint64_t> g_present_calls{0};
thread_local int t_depth = 0;

struct Guard {
    Guard() { g_inflight.fetch_add(1, std::memory_order_acq_rel); ++t_depth; }
    ~Guard() { --t_depth; g_inflight.fetch_sub(1, std::memory_order_acq_rel); }
};

bool Frame(IDXGISwapChain* sc, UINT flags) {
    if (t_depth != 1 || (flags & DXGI_PRESENT_TEST)) return false;  // nested Present or a test present
    g_present_calls.fetch_add(1, std::memory_order_relaxed);
    g_on_frame(sc);
    return true;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    Guard g;
    if (!Frame(sc, flags)) return g_present(sc, sync, flags);
    const double cpu0 = Clock::ThreadCpuMs(), wall0 = Clock::NowMs();
    const HRESULT hr = g_present(sc, sync, flags);
    g_on_presented(Clock::ThreadCpuMs() - cpu0, Clock::NowMs() - wall0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    Guard g;
    if (!Frame(sc, flags)) return g_present1(sc, sync, flags, params);
    const double cpu0 = Clock::ThreadCpuMs(), wall0 = Clock::NowMs();
    const HRESULT hr = g_present1(sc, sync, flags, params);
    g_on_presented(Clock::ThreadCpuMs() - cpu0, Clock::NowMs() - wall0);
    return hr;
}

HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    Guard g;
    g_on_resize(sc, true);
    const HRESULT hr = g_resize(sc, n, w, h, f, fl);
    g_on_resize(sc, false);
    return hr;
}

Method g_method = Method::Inline;
void** g_slots[3] = {};  // vtable slots swapped by Method::Vtable: Present, Present1, ResizeBuffers

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Reads the DXGI vtable from a throwaway device + swap chain on a hidden window.
bool ReadVtable(void*& present, void*& present1, void*& resize, void** (&slots)[3]) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"grw_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        void** vt = *reinterpret_cast<void***>(sc);
        present = vt[8];
        resize = vt[13];
        slots[0] = &vt[8];
        slots[2] = &vt[13];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            void** vt1 = *reinterpret_cast<void***>(sc1);
            present1 = vt1[22];
            slots[1] = &vt1[22];
            sc1->Release();
        }
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

bool Install(FrameCallback on_frame, ResizeCallback on_resize, PresentedCallback on_presented, Method method) {
    g_on_frame = on_frame;
    g_on_resize = on_resize;
    g_on_presented = on_presented;
    g_method = method;
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
    if (method == Method::None) return true;
    if (!ReadVtable(present, present1, resize, g_slots)) return false;
    if (method == Method::DummyOnly) return true;

    if (method == Method::Vtable) {
        // The originals are whatever the slots hold now; another vtable hook would stay in the chain.
        g_present = reinterpret_cast<PresentFn>(*g_slots[0]);
        g_resize = reinterpret_cast<ResizeFn>(*g_slots[2]);
        if (g_slots[1]) g_present1 = reinterpret_cast<Present1Fn>(*g_slots[1]);
        bool ok = WriteSlot(g_slots[0], reinterpret_cast<void*>(&PresentHook)) &&
                  WriteSlot(g_slots[2], reinterpret_cast<void*>(&ResizeHook));
        if (ok && g_slots[1]) ok = WriteSlot(g_slots[1], reinterpret_cast<void*>(&Present1Hook));
        if (!ok) Uninstall();
        return ok;
    }

    if (MH_Initialize() != MH_OK) return false;
    bool ok = MH_CreateHook(present, reinterpret_cast<void*>(&PresentHook),
                            reinterpret_cast<void**>(&g_present)) == MH_OK &&
              MH_CreateHook(resize, reinterpret_cast<void*>(&ResizeHook),
                            reinterpret_cast<void**>(&g_resize)) == MH_OK;
    if (ok && present1)
        ok = MH_CreateHook(present1, reinterpret_cast<void*>(&Present1Hook),
                           reinterpret_cast<void**>(&g_present1)) == MH_OK;
    if (ok) ok = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    if (!ok) MH_Uninitialize();
    return ok;
}

void Uninstall() {
    if (g_method == Method::Vtable) {
        // Restore only the slots that still point at us (a write may have failed half way).
        if (g_slots[0] && *g_slots[0] == reinterpret_cast<void*>(&PresentHook)) WriteSlot(g_slots[0], g_present);
        if (g_slots[1] && *g_slots[1] == reinterpret_cast<void*>(&Present1Hook)) WriteSlot(g_slots[1], g_present1);
        if (g_slots[2] && *g_slots[2] == reinterpret_cast<void*>(&ResizeHook)) WriteSlot(g_slots[2], g_resize);
    } else if (g_method == Method::Inline) {
        MH_DisableHook(MH_ALL_HOOKS);
    }
    while (g_inflight.load(std::memory_order_acquire) != 0) Sleep(1);
    Sleep(100);  // a thread may sit between a detour's entry and its Guard
    if (g_method == Method::Inline) MH_Uninitialize();
}

uint64_t PresentCalls() { return g_present_calls.load(std::memory_order_relaxed); }

}  // namespace grw::hooks
