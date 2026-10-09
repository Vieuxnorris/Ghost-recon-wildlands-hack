#pragma once
#include <dxgi.h>

namespace grw::overlay {

// Render thread: lazily initialises ImGui on this swap chain (then calls setup once: fonts, style), runs draw(),
// and renders on the back buffer when `render` is true (false = build the ImGui frame only; used to measure what
// rendering costs).
bool Frame(IDXGISwapChain* sc, void (*setup)(), void (*draw)(), bool render);
void BeforeResize();
void AfterResize(IDXGISwapChain* sc);
// DLL main thread, BEFORE removing the DXGI hooks: the next Present drops our back-buffer reference
// (falls back to this thread after timeout_ms) and the overlay stops drawing.
void ReleaseBackBuffer(DWORD timeout_ms);
// DLL main thread, after the DXGI hooks are removed.
void Shutdown();

}  // namespace grw::overlay
