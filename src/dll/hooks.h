#pragma once
#include <dxgi.h>
#include <cstdint>

namespace grw::hooks {

using FrameCallback = void (*)(IDXGISwapChain* sc);
using ResizeCallback = void (*)(IDXGISwapChain* sc, bool before);
// After the original Present returned: its CPU and wall time (overlays further down the chain included).
using PresentedCallback = void (*)(double cpu_ms, double wall_ms);

enum class Method { Inline = 0, Vtable = 1, DummyOnly = 2, None = 3 };  // 2 and 3: diagnostics, no hooks

// Hooks IDXGISwapChain::Present, IDXGISwapChain1::Present1 and ResizeBuffers, either with MinHook on the
// functions' first bytes or by swapping dxgi's vtable slots (leaves the prologues to other overlays).
bool Install(FrameCallback on_frame, ResizeCallback on_resize, PresentedCallback on_presented, Method method);
// Disables the hooks, then waits until no thread is inside a detour.
void Uninstall();
uint64_t PresentCalls();

}  // namespace grw::hooks
