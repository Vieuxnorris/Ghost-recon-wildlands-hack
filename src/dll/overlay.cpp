#include "dll/overlay.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include "core/clock.h"
#include "dll/runtime.h"
#include "telemetry/spsc_ring.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace grw::overlay {

namespace {

struct WinMsg {
    HWND hwnd;
    UINT msg;
    WPARAM w;
    LPARAM l;
};

enum class State { Uninit, Ready, Failed };

State g_state = State::Uninit;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
float g_width = 0, g_height = 0;
double g_last_ms = 0;
bool g_imgui_ready = false;  // both backends initialised
SpscRing<WinMsg, 256> g_msgs;
std::atomic<int> g_wndproc_inflight{0};
// Unload handshake: the render thread drops its back-buffer reference before the hooks go away,
// otherwise a ResizeBuffers landing between unhook and Shutdown fails on our RTV.
std::atomic<bool> g_release_requested{false};
std::atomic<bool> g_released{false};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_wndproc_inflight.fetch_add(1, std::memory_order_acq_rel);
    if (Rt().MenuOpen()) g_msgs.Push({h, m, w, l});  // replayed into ImGui on the render thread
    const LRESULT r = CallWindowProcW(g_orig_wndproc, h, m, w, l);
    g_wndproc_inflight.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

void CreateRtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    g_width = float(d.Width);
    g_height = float(d.Height);
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

bool Init(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_dev)))) return false;  // not D3D11 (e.g. D3D12)
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC sd{};
    sc->GetDesc(&sd);
    g_hwnd = sd.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    g_imgui_ready = true;
    CreateRtv(sc);
    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    g_last_ms = Clock::NowMs();
    return g_rtv != nullptr;
}

void NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    if (Rt().MenuOpen()) {
        WinMsg m;
        while (g_msgs.Pop(m)) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.w, m.l);
        ImGui_ImplWin32_NewFrame();  // cursor and focus: only worth its system calls when the menu is open
        io.MouseDrawCursor = true;
    } else {
        WinMsg m;
        while (g_msgs.Pop(m)) {}
        const double now = Clock::NowMs();
        io.DeltaTime = float(now - g_last_ms > 0.0 ? (now - g_last_ms) / 1000.0 : 1.0 / 60.0);
        io.DisplaySize = ImVec2(g_width, g_height);
        io.MouseDrawCursor = false;
    }
    g_last_ms = Clock::NowMs();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
}

}  // namespace

bool Frame(IDXGISwapChain* sc, void (*draw)()) {
    if (g_release_requested.load(std::memory_order_acquire)) {
        BeforeResize();
        g_released.store(true, std::memory_order_release);
        return g_state == State::Ready;
    }
    if (g_state == State::Uninit) g_state = Init(sc) ? State::Ready : State::Failed;
    if (g_state != State::Ready || !g_rtv) return g_state == State::Ready;

    NewFrame();
    draw();
    ImGui::Render();

    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
    return true;
}

void BeforeResize() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

void AfterResize(IDXGISwapChain* sc) {
    if (g_state == State::Ready && !g_release_requested.load(std::memory_order_acquire)) CreateRtv(sc);
}

void ReleaseBackBuffer(DWORD timeout_ms) {
    g_release_requested.store(true, std::memory_order_release);
    for (DWORD waited = 0; waited < timeout_ms && !g_released.load(std::memory_order_acquire); ++waited) Sleep(1);
    // No Present arrived in time: nothing is rendering, so releasing from this thread is safe.
    if (!g_released.load(std::memory_order_acquire)) BeforeResize();
}

void Shutdown() {
    if (g_orig_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
        while (g_wndproc_inflight.load(std::memory_order_acquire) != 0) Sleep(1);
        Sleep(100);  // a message may sit between our WndProc's entry and its counter
        g_orig_wndproc = nullptr;
    }
    if (g_imgui_ready) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_ready = false;
    }
    BeforeResize();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_ctx = nullptr;
    g_dev = nullptr;
    g_state = State::Uninit;
}

}  // namespace grw::overlay
