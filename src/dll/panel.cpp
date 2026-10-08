#include "dll/panel.h"
#include <imgui.h>
#include "dll/esp.h"
#include "dll/runtime.h"

namespace grw {

void DrawPanel() {
    DrawEsp();
    Runtime& rt = Rt();
    const bool menu = rt.MenuOpen();

    if (rt.PanelOpen()) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       (menu ? 0 : ImGuiWindowFlags_NoInputs);
        ImGui::Begin("grw##panel", nullptr, flags);
        ImGui::Text("frame %llu", static_cast<unsigned long long>(rt.Frame()));
        const Percentiles* s = rt.LastStats();
        for (size_t i = 0; i < kStageCount; ++i) {
            if (s[i].n == 0) continue;
            ImGui::Text("%-8s p50 %.3f  p99 %.3f ms", StageName(Stage(i)), s[i].p50_cpu, s[i].p99_cpu);
        }
        const game::EspFrame& e = rt.Esp();
        if (!rt.EspOn()) ImGui::TextUnformatted("esp off (F6)");
        else if (!e.camera_ok) ImGui::TextUnformatted("esp: camera not found yet");
        else
            ImGui::Text("esp: fov %.1f  chars %u known, %u in range, %u on screen, %u heads", e.fov_deg, e.known,
                        e.active, e.on_screen, e.heads);
        const Aimbot::Status& a = rt.AimStatus();
        ImGui::Text("aim: accumulator %s  key %s  target %s  err %.2f deg  writes %llu", a.accumulator ? "ok" : "missing",
                    a.key ? "held" : "-", a.target ? "locked" : "-", a.error_deg,
                    static_cast<unsigned long long>(a.writes));
        ImGui::Text("     crosshair offset yaw %+.2f pitch %+.2f deg%s  flight %.0f ms", a.offset_yaw_deg,
                    a.offset_pitch_deg, a.recoil_hold ? "  (recoil hold)" : "", a.flight_ms);
        ImGui::Text("dropped %llu  pipe %s", static_cast<unsigned long long>(rt.Tel().Dropped(Channel::Render)),
                    rt.Tel().PipeConnected() ? "on" : "off");
        ImGui::End();
    }

    if (menu) {
        const Config c = rt.Cfg();
        ImGui::SetNextWindowPos(ImVec2(10, 160), ImGuiCond_FirstUseEver);
        ImGui::Begin("grw settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("aim: tau %.0f ms, cone %.1f deg, max %.0f deg/s, dead zone %.0f %%", c.aim_tau_ms,
                    c.aim_cone_deg, c.aim_max_speed_deg_s, c.aim_deadzone_frac * 100.0f);
        ImGui::Text("esp: max distance %.0f m", c.esp_max_distance_m);
        ImGui::Text("keys: menu 0x%02X  panel 0x%02X  unload 0x%02X  aim 0x%02X", c.menu_key, c.panel_key,
                    c.unload_key, c.aim_key);
        ImGui::Separator();
        ImGui::TextUnformatted("Edit runtime\\grw.ini: changes apply when the file is saved.");
        ImGui::End();
    }
}

}  // namespace grw
