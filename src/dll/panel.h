#pragma once

namespace grw {

// Once, right after ImGui is created: the font (Windows' Bahnschrift, else Segoe UI, else ImGui's own) and style.
void SetupUi();

// Render thread, inside an ImGui frame: the ESP, the HUD (panel key: a discreet status line, then the details)
// and the menu (menu key: weapon mods and settings, in tabs).
void DrawPanel();

}  // namespace grw
