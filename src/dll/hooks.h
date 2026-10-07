#pragma once
#include <dxgi.h>
#include <cstdint>

namespace grw::hooks {

using FrameCallback = void (*)(IDXGISwapChain* sc);
using ResizeCallback = void (*)(IDXGISwapChain* sc, bool before);

// MinHook on IDXGISwapChain::Present, IDXGISwapChain1::Present1 and ResizeBuffers.
bool Install(FrameCallback on_frame, ResizeCallback on_resize);
// Disables the hooks, then waits until no thread is inside a detour.
void Uninstall();
uint64_t PresentCalls();

}  // namespace grw::hooks
