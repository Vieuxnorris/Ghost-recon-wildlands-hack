#include "dll/esp.h"
#include <imgui.h>
#include <cfloat>
#include <cstdio>
#include "dll/runtime.h"

namespace grw {

namespace {

ImU32 Colour(game::Category c) {
    switch (c) {
        case game::Category::Squad: return IM_COL32(64, 255, 64, 255);
        case game::Category::Ally: return IM_COL32(0, 230, 255, 255);
        case game::Category::SantaBlanca: return IM_COL32(255, 64, 64, 255);
        case game::Category::Unidad: return IM_COL32(255, 160, 0, 255);
        case game::Category::Civilian: return IM_COL32(235, 235, 235, 255);
        case game::Category::Dead: return IM_COL32(128, 128, 128, 200);
        default: return IM_COL32(255, 255, 0, 255);
    }
}

}  // namespace

void DrawEsp(float label_size) {
    Runtime& rt = Rt();
    const ImGuiIO& io = ImGui::GetIO();
    rt.UpdateEsp(io.DisplaySize.x, io.DisplaySize.y);
    if (!rt.EspOn()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    char label[48];
    for (const game::EspItem& it : rt.Esp().items) {
        // line of sight (M6): hidden -> faded box and grey head; visible -> green head; unknown -> as before
        const bool hidden = it.los == game::kLosHidden;
        ImU32 col = Colour(it.cat);
        if (hidden) col = (col & 0x00FFFFFF) | (IM_COL32_A_MASK & IM_COL32(0, 0, 0, 90));
        const ImU32 head_col = hidden                         ? IM_COL32(150, 150, 150, 200)
                               : it.los == game::kLosVisible ? IM_COL32(40, 255, 40, 255)
                                                             : IM_COL32(255, 32, 32, 255);
        const ImVec2 a(it.box_min.x, it.box_min.y), b(it.box_max.x, it.box_max.y);
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), hidden ? IM_COL32(0, 0, 0, 90) : shadow);
        dl->AddRect(a, b, col, 0.0f, 0, 1.5f);
        if (it.head_radius > 0)
            dl->AddCircle(ImVec2(it.head.x, it.head.y), it.head_radius, head_col, 0, it.los == game::kLosVisible ? 2.0f : 1.5f);
        std::snprintf(label, sizeof label, "%s%s %.0f m", game::CategoryName(it.cat), it.vehicle ? " veh" : "",
                      it.dist);
        const ImVec2 ts = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
        const ImVec2 p((a.x + b.x - ts.x) * 0.5f, b.y + 2);
        dl->AddText(font, label_size, ImVec2(p.x + 1, p.y + 1), shadow, label);
        dl->AddText(font, label_size, p, col, label);
    }
}

}  // namespace grw
