#pragma once

namespace grw {

// Render thread, inside the ImGui frame: reads the game (Runtime::UpdateEsp) and draws boxes, head
// markers and labels on the background draw list, the labels at label_size pixels.
void DrawEsp(float label_size);

}  // namespace grw
