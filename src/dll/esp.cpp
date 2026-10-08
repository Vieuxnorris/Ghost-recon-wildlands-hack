#include "dll/esp.h"
#include <imgui.h>
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

void DrawEsp() {
    Runtime& rt = Rt();
    const ImGuiIO& io = ImGui::GetIO();
    rt.UpdateEsp(io.DisplaySize.x, io.DisplaySize.y);
    if (!rt.EspOn()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    char label[48];
    for (const game::EspItem& it : rt.Esp().items) {
        const ImU32 col = Colour(it.cat);
        const ImVec2 a(it.box_min.x, it.box_min.y), b(it.box_max.x, it.box_max.y);
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), shadow);
        dl->AddRect(a, b, col, 0.0f, 0, 1.5f);
        if (it.head_radius > 0)
            dl->AddCircle(ImVec2(it.head.x, it.head.y), it.head_radius, IM_COL32(255, 32, 32, 255), 0, 1.5f);
        std::snprintf(label, sizeof label, "%s%s %.0f m", game::CategoryName(it.cat), it.vehicle ? " veh" : "",
                      it.dist);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 p((a.x + b.x - ts.x) * 0.5f, b.y + 2);
        dl->AddText(ImVec2(p.x + 1, p.y + 1), shadow, label);
        dl->AddText(p, col, label);
    }
}

}  // namespace grw
