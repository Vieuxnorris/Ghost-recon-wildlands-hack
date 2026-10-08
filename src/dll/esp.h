#pragma once

namespace grw {

// Render thread, inside the ImGui frame: reads the game (Runtime::UpdateEsp) and draws boxes, head
// markers and labels on the background draw list.
void DrawEsp();

}  // namespace grw
