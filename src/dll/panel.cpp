#include "dll/panel.h"
#include <windows.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include "dll/esp.h"
#include "dll/hwbp.h"
#include "dll/runtime.h"

namespace grw {

namespace {

// Neutral greys, one accent (the visible-head green), amber for what the mods change.
const ImVec4 kText(0.90f, 0.92f, 0.93f, 1.0f);
const ImVec4 kDim(0.56f, 0.60f, 0.63f, 1.0f);
const ImVec4 kAccent(0.45f, 0.83f, 0.60f, 1.0f);
const ImVec4 kWarn(0.96f, 0.73f, 0.32f, 1.0f);
const ImVec4 kBad(0.94f, 0.40f, 0.36f, 1.0f);
const char* const kTimes = "\xC3\x97";  // UTF-8
const char* const kDeg = "\xC2\xB0";

constexpr float kHudSize = 13.0f, kMenuSize = 15.0f;  // font sizes at 1080p
float g_scale = 0;  // the style's scale: display height / 1080, at least 1 (0 = not applied yet)

void ApplyStyle(float k) {
    const ImGuiStyle old = ImGui::GetStyle();
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.WindowRounding = 6;
    s.ChildRounding = s.PopupRounding = 4;
    s.FrameRounding = s.GrabRounding = s.TabRounding = 3;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = s.TabBorderSize = 0;
    s.GrabMinSize = 8;
    s.ScrollbarSize = 8;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.86f);
    c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_CheckMark] = c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.92f, 0.72f, 1.0f);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    s.ScaleAllSizes(k);
    // the fonts' sizes stay ImGui's (PushFont picks the size of each window)
    s.FontSizeBase = old.FontSizeBase;
    s.FontScaleMain = old.FontScaleMain;
    s.FontScaleDpi = old.FontScaleDpi;
    s._NextFrameFontSizeBase = old._NextFrameFontSizeBase;
    ImGui::GetStyle() = s;
    g_scale = k;
}

// The key's name as Windows shows it on this keyboard ("INSER", "F8"), the mouse buttons by number.
const char* KeyName(int vk, char* buf, size_t n) {
    if (vk >= 1 && vk <= 6 && vk != 3) {
        static const char* const kMouse[] = {"", "mouse 1", "mouse 2", "", "mouse 3", "mouse 4", "mouse 5"};
        return kMouse[vk];
    }
    LONG lp = LONG(MapVirtualKeyA(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) lp |= 1 << 24;  // extended
    if (lp && GetKeyNameTextA(lp, buf, int(n)) > 0) return buf;
    std::snprintf(buf, n, "0x%02X", vk);
    return buf;
}

void Gap() { ImGui::SameLine(0, ImGui::GetFontSize() * 0.9f); }

// A status dot in front of the next text.
void Dot(const ImVec4& col) {
    const float h = ImGui::GetTextLineHeight(), r = h * 0.2f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(2 * r, h));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, ImGui::GetColorU32(col));
    ImGui::SameLine(0, r * 1.5f);
}

// A thin bar, frac in [0, 1].
void Bar(float frac, float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight(), t = std::max(2.0f, std::round(h * 0.22f));
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = std::round(p.y + (h - t) * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width, y + t), IM_COL32(255, 255, 255, 30), t * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width * std::clamp(frac, 0.0f, 1.0f), y + t),
                      ImGui::GetColorU32(kAccent), t * 0.5f);
}

// "label  value" rows: the label dim, in a column as wide as `widest`.
void Label(const char* label, const char* widest = "crosshair") {
    const float x = ImGui::GetCursorPosX();
    ImGui::TextColored(kDim, "%s", label);
    ImGui::SameLine(x + ImGui::CalcTextSize(widest).x + ImGui::GetFontSize() * 0.8f);
}

using Mod = Runtime::Mod;
struct ModRow {
    Mod mod;
    const char* name;  // the HUD's short name
    const char* label;
    float lo, hi;
    float Config::*field;  // its value in the settings
    bool log;              // a logarithmic slider
};
const ModRow kModRows[] = {{Mod::Spread, "spread", "spread", 0.0f, 2.0f, &Config::mod_spread, false},
                           {Mod::BulletSpeed, "speed", "bullet speed", 0.25f, 4.0f, &Config::mod_bullet_speed, false},
                           {Mod::Damage, "damage", "damage", 0.0f, 100.0f, &Config::mod_damage, true},
                           {Mod::Recoil, "recoil", "recoil", 0.0f, 2.0f, &Config::mod_recoil, false}};
static_assert(std::size(kModRows) == size_t(Mod::Count), "one row per mod");

// The weapon in hand, as the game's menu computes it, and what its next bullet will do.
void WeaponInfo(const game::EspFrame& e, const Aimbot::Status& a) {
    const game::WeaponView& w = e.weapon;
    if (!w.parts_ok && !w.stats_ok && !e.bullet_ok) {
        ImGui::TextColored(kDim, "weapon not read yet");
        return;
    }
    if (e.bullet_ok) {
        Label("bullet");
        ImGui::Text("%.0f m/s", e.bullet_V);
        Gap();
        ImGui::TextColored(kDim, "damage");
        ImGui::SameLine();
        ImGui::Text("%.0f", w.damage);
        Gap();
        ImGui::TextColored(kDim, "mag");
        ImGui::SameLine();
        ImGui::Text("%d", w.magazine);
    }
    if (w.spread_live >= 0) {
        Label("spread");
        ImGui::TextColored(w.spread_factor != 1.0f ? kWarn : kText, "%.4f", w.spread_live * w.spread_factor);
        if (a.target && a.cruise_mps > 0 && e.bullet_ok && e.bullet_V > 0) {  // updated only while locked
            Gap();
            ImGui::TextColored(kDim, "next bullet %.0f m/s (%.0f %%)", a.cruise_mps, 100.0f * a.cruise_mps / e.bullet_V);
        }
    }
    const float bar_w = ImGui::GetFontSize() * 6.0f;
    if (w.stats_ok && ImGui::BeginTable("stats", 3, ImGuiTableFlags_SizingFixedFit)) {
        static const char* const kStat[game::kStatCount] = {"damage", "accuracy", "handling",   "range",
                                                            "rate",   "silence",  "penetration"};
        for (size_t i = 0; i < game::kStatCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", kStat[i]);
            ImGui::TableNextColumn();
            Bar(w.Stat(i) / 100.0f, bar_w);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", w.Stat(i));
        }
        ImGui::EndTable();
    }
    if (w.parts_ok && ImGui::BeginTable("parts", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (uint32_t i = 0; i < w.parts; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", w.slot[i]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(w.part[i]);
        }
        ImGui::EndTable();
    }
}

void AimInfo(Runtime& rt, bool full) {
    const Aimbot::Status& a = rt.AimStatus();
    Label("target");
    if (!a.accumulator) ImGui::TextColored(kBad, "view not found");
    else if (a.target) ImGui::Text("%.0f m  error %.2f%s", a.target_dist, a.error_deg, kDeg);
    else ImGui::TextColored(kDim, a.key ? "none in the cone" : "-");
    if (a.target && a.flight_ms > 0) {
        Label("lead");
        ImGui::Text("%.0f ms", a.flight_ms);
        Gap();
        ImGui::TextColored(kDim, "drop %.2f m", a.drop_m);
    }
    Label("path");
    if (a.bullet.Valid())
        ImGui::Text("learned to %.0f m%s", a.table_max_m,
                    a.trajectory == 3 ? " (unused: no drop)" : a.trajectory == 2 ? " (beyond)" : "");
    else
        ImGui::TextColored(kWarn, "fire one shot far away");
    if (rt.TrackerArmed()) {
        Label("tracking");
        ImGui::TextColored(kDim, "%.0f exceptions/s", rt.TrackerCallsPerS());
    }
    if (!full) return;
    Label("crosshair");
    ImGui::TextColored(kDim, "%+.2f / %+.2f%s%s", a.offset_yaw_deg, a.offset_pitch_deg, kDeg,
                       a.recoil_hold ? "  recoil hold" : "");
    if (a.bullet.Valid()) {
        Label("bullets");
        ImGui::TextColored(kDim, "V %.0f m/s, first segment %.1f m", a.bullet.V, a.bullet.first_segment);
    }
    Label("writes");
    ImGui::TextColored(kDim, "%llu", static_cast<unsigned long long>(a.writes));
}

void EspInfo(Runtime& rt) {
    const game::EspFrame& e = rt.Esp();
    if (!rt.EspOn()) ImGui::TextColored(kDim, "ESP off");
    else if (!e.camera_ok) ImGui::TextColored(kWarn, "camera not found yet");
    else
        ImGui::TextColored(kDim, "%u characters %s, %u in range, %u on screen, %u heads", e.known,
                           e.from_list ? "(list)" : "(scan)", e.active, e.on_screen, e.heads);
}

void DebugInfo(Runtime& rt) {
    const Percentiles* s = rt.LastStats();
    if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (size_t i = 0; i < kStageCount; ++i) {
            if (s[i].n == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "%s", StageName(Stage(i)));
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s[i].p50_cpu);
            ImGui::TableNextColumn();
            ImGui::TextColored(kDim, "p99 %.3f ms", s[i].p99_cpu);
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kDim, "mods: %llu shots, %llu recoil frames scaled",
                       static_cast<unsigned long long>(rt.SpreadModHits()),
                       static_cast<unsigned long long>(rt.RecoilModHits()));
    if (rt.MagicBullets()) {
        const hwbp::MagicStats ms = hwbp::Magic();
        ImGui::TextColored(kDim, "magic: %llu bullets, last to %.0f m", static_cast<unsigned long long>(ms.bullets),
                           ms.last_dist_m);
    }
    if (rt.StraightBullets() || rt.TeleportBullets()) {
        const hwbp::StraightStats st = hwbp::Straightened();
        ImGui::TextColored(kDim, "bullets: %llu straightened (last %.1f m of drop removed), %llu teleported",
                           static_cast<unsigned long long>(st.paths), st.drop_m,
                           static_cast<unsigned long long>(st.teleported));
    }
    ImGui::TextColored(kDim, "frame %llu  dropped %llu  pipe %s", static_cast<unsigned long long>(rt.Frame()),
                       static_cast<unsigned long long>(rt.Tel().Dropped(Channel::Render)),
                       rt.Tel().PipeConnected() ? "on" : "off");
}

// One line: aim (locked target: distance and flight time), ESP, the mods in effect, a learning hint.
void StatusLine(Runtime& rt, bool aim_on) {
    const Aimbot::Status& a = rt.AimStatus();
    Dot(!aim_on ? kDim : !a.accumulator ? kBad : a.target ? kAccent : kDim);
    ImGui::TextColored(aim_on && a.target ? kText : kDim, aim_on ? "AIM" : "AIM off");
    if (aim_on && a.target) {
        ImGui::SameLine();
        ImGui::Text("%.0f m", a.target_dist);
        if (a.flight_ms > 0) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, "%.0f ms", a.flight_ms);
        }
    }
    Gap();
    Dot(rt.EspOn() ? kAccent : kDim);
    ImGui::TextColored(rt.EspOn() ? kText : kDim, "ESP");
    if (rt.MagicBullets()) {
        Gap();
        ImGui::TextColored(kWarn, "magic");
    } else if (rt.TeleportBullets()) {
        Gap();
        ImGui::TextColored(kWarn, "teleport");
    } else if (rt.InstantBullets() || rt.StraightBullets()) {
        Gap();
        ImGui::TextColored(kWarn, "%s", !rt.StraightBullets() ? "instant" : !rt.InstantBullets() ? "no drop" : "laser");
    }
    for (const ModRow& r : kModRows) {
        const float k = rt.ModFactor(r.mod);
        if (k == 1.0f || (r.mod == Mod::BulletSpeed && rt.InstantBullets())) continue;
        Gap();
        ImGui::TextColored(kWarn, "%s %s%.2g", r.name, kTimes, k);
    }
    if (rt.Esp().bullet_ok && !a.bullet.Valid()) {
        Gap();
        ImGui::TextColored(kDim, "path: fire once far away");
    }
}

void DrawHud(Runtime& rt, bool details, bool aim_on) {
    const float k = g_scale;
    ImGui::SetNextWindowPos(ImVec2(12 * k, 10 * k), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.38f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * k, 4 * k));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * k);
    ImGui::PushFont(nullptr, kHudSize * k);
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    StatusLine(rt, aim_on);
    if (details) {
        const game::EspFrame& e = rt.Esp();
        ImGui::Separator();
        WeaponInfo(e, rt.AimStatus());
        ImGui::Separator();
        AimInfo(rt, false);
        EspInfo(rt);
        ImGui::Separator();
        DebugInfo(rt);
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
}

// Settings rows: the label in a column (amber when off its default), the control filling the rest.
float g_label_w = 0;  // set per tab
void RowLabel(const char* label, bool changed) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(changed ? kWarn : kText, "%s", label);
    ImGui::SameLine(g_label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
// A slider; right-click: back to its default.
bool SliderRow(const char* label, float* v, float lo, float hi, const char* fmt, float def, bool log = false) {
    ImGui::PushID(label);
    RowLabel(label, *v != def);
    bool changed = ImGui::SliderFloat("##v", v, lo, hi, fmt, log ? ImGuiSliderFlags_Logarithmic : 0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != def) {
        *v = def;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}
// A checkbox on an int setting (0/1).
bool Check(const char* label, int* v) {
    bool b = *v != 0;
    if (!ImGui::Checkbox(label, &b)) return false;
    *v = b ? 1 : 0;
    return true;
}

// A key setting: click, then press a key or a mouse button (Escape: cancel). Keys already down when the capture
// starts (the click itself) count only once released and pressed again.
int g_capture = 0;  // the id of the row capturing, 0 = none
bool g_capture_down[256] = {};
bool KeyRow(const char* label, int* vk, int id, int forbidden) {
    char name[32];
    ImGui::PushID(id);
    RowLabel(label, false);
    bool changed = false;
    if (g_capture == id) {
        ImGui::Button("press a key or a mouse button...", ImVec2(-FLT_MIN, 0));
        for (int k = 1; k < 255; ++k) {
            const bool down = (GetAsyncKeyState(k) & 0x8000) != 0;
            if (!down) {
                g_capture_down[k] = false;
                continue;
            }
            if (g_capture_down[k] || k == forbidden) continue;
            if (k != VK_ESCAPE) {
                *vk = k;
                changed = true;
            }
            g_capture = 0;
            break;
        }
    } else if (ImGui::Button(KeyName(*vk, name, sizeof name), ImVec2(-FLT_MIN, 0))) {
        g_capture = id;
        for (int k = 0; k < 256; ++k) g_capture_down[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
    }
    ImGui::PopID();
    return changed;
}

bool TabAim(Runtime& rt, Config& c) {
    const Config d;
    char deg[16], dps[16];
    std::snprintf(deg, sizeof deg, "%%.1f%s", kDeg);
    std::snprintf(dps, sizeof dps, "%%.0f%s/s", kDeg);
    bool changed = Check("aimbot", &c.aim_enabled);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("lead moving targets", &c.aim_lead);
    changed |= KeyRow("aim key", &c.aim_key, 1, c.menu_key);
    changed |= SliderRow("smoothing", &c.aim_tau_ms, 10.0f, 300.0f, "%.0f ms", d.aim_tau_ms);
    changed |= SliderRow("cone", &c.aim_cone_deg, 1.0f, 45.0f, deg, d.aim_cone_deg);
    changed |= SliderRow("max speed", &c.aim_max_speed_deg_s, 30.0f, 1440.0f, dps, d.aim_max_speed_deg_s);
    float dz = c.aim_deadzone_frac * 100.0f;
    if (SliderRow("dead zone", &dz, 0.0f, 100.0f, "%.0f %% of the head", d.aim_deadzone_frac * 100.0f)) {
        c.aim_deadzone_frac = dz / 100.0f;
        changed = true;
    }
    ImGui::Separator();
    AimInfo(rt, true);
    return changed;
}

bool TabVisuals(Runtime& rt, Config& c) {
    const Config d;
    bool changed = false;
    bool esp = rt.EspOn();
    if (ImGui::Checkbox("ESP", &esp)) rt.SetEspOn(esp);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("civilians", &c.esp_civilians);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("dead", &c.esp_dead);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("line of sight", &c.los_enabled);
    changed |= SliderRow("range", &c.esp_max_distance_m, 50.0f, 2000.0f, "%.0f m", d.esp_max_distance_m);
    changed |= KeyRow("ESP key", &c.esp_key, 2, c.menu_key);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("HUD");
    ImGui::SameLine(g_label_w);
    const int hud = rt.HudLevel();
    static const char* const kHud[] = {"hidden", "status line", "details"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kHud[i], hud == i)) rt.SetHudLevel(i);
    }
    changed |= KeyRow("HUD key", &c.panel_key, 3, c.menu_key);
    ImGui::Separator();
    EspInfo(rt);
    return changed;
}

bool TabDebug(Runtime& rt, Config& c) {
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("learn paths");
    ImGui::SameLine(g_label_w);
    static const char* const kTrack[] = {"off", "always", "while useful"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kTrack[i], c.proj_track == i)) c.proj_track = i, changed = true;
    }
    changed |= KeyRow("menu key", &c.menu_key, 4, -1);
    changed |= Check("magic bullets: dump what their collision test meets (telemetry)", &c.magic_debug);
    ImGui::Separator();
    DebugInfo(rt);
    return changed;
}

bool TabWeapon(Runtime& rt, Config& c) {
    bool changed = false;
    char fmt[16];
    std::snprintf(fmt, sizeof fmt, "%s%%.2f", kTimes);
    for (const ModRow& r : kModRows) {
        ImGui::BeginDisabled(r.mod == Mod::BulletSpeed && c.mod_instant_bullets);  // instant overrides the factor
        changed |= SliderRow(r.label, &(c.*r.field), r.lo, r.hi, fmt, 1.0f, r.log);
        ImGui::EndDisabled();
    }
    if (ImGui::SmallButton("all to x1")) {
        for (const ModRow& r : kModRows) c.*r.field = 1.0f;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "right-click a slider: its default");
    ImGui::Separator();
    ImGui::TextColored(kDim, "super weapon");
    changed |= Check("infinite ammo", &c.mod_infinite_ammo);
    ImGui::SameLine();
    const int flag = rt.AmmoFlag();
    ImGui::TextColored(kDim, flag < 0 ? "(game flag not found)" : flag ? "on, every weapon" : "off");
    ImGui::PushID("bullets per shot");
    RowLabel("bullets per shot", c.mod_bullets_per_shot != 1);
    changed |= ImGui::SliderInt("##v", &c.mod_bullets_per_shot, 1, 32);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && c.mod_bullets_per_shot != 1) {
        c.mod_bullets_per_shot = 1;
        changed = true;
    }
    ImGui::PopID();
    if (c.mod_bullets_per_shot > 1 && c.hwbp_addr != 0)
        ImGui::TextColored(kWarn, "bullets per shot needs DR0: a research address (hwbp_addr) holds it");
    ImGui::Separator();
    WeaponInfo(rt.Esp(), rt.AimStatus());
    return changed;
}

bool TabBullets(Runtime& rt, Config& c) {
    bool changed = false;
    ImGui::TextColored(kDim, "the player's bullets");
    changed |= Check("instant", &c.mod_instant_bullets);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("no drop", &c.mod_straight_bullets);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("teleport", &c.mod_teleport_bullets);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("magic", &c.mod_magic_bullets);
    ImGui::TextColored(kDim, "instant: 30 km/s   no drop: straight   teleport: hitscan   magic: to the heads");
    ImGui::Separator();
    ImGui::BeginDisabled(!c.mod_magic_bullets);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("targets");
    ImGui::SameLine(g_label_w);
    if (ImGui::RadioButton("aim lock", c.magic_target == 0)) c.magic_target = 0, changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("any enemy", c.magic_target == 1)) c.magic_target = 1, changed = true;
    changed |= Check("one bullet per enemy", &c.magic_spread);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("through walls", &c.magic_walls);
    changed |= Check("pierce vehicles", &c.magic_pierce);
    ImGui::SameLine(0, ImGui::GetFontSize());
    changed |= Check("unread heads from above", &c.magic_vertical_est);
    char deg[16];
    std::snprintf(deg, sizeof deg, "%%.0f%s", kDeg);
    changed |= SliderRow("cone", &c.magic_fov_deg, 1.0f, 180.0f, deg, 180.0f);
    changed |= SliderRow("range", &c.magic_max_m, 50.0f, 2000.0f, "%.0f m", 1200.0f);
    changed |= SliderRow("last test", &c.magic_start_m, 2.0f, 30.0f, "%.1f m", 12.0f);
    ImGui::EndDisabled();
    if (c.magic_max_m > c.esp_max_distance_m)
        ImGui::TextColored(kWarn, "the ESP reads characters to %.0f m only (Visuals)", c.esp_max_distance_m);
    const Runtime::MagicTally t = rt.MagicResults();
    if (t.shots) {
        ImGui::Separator();
        ImGui::TextColored(kDim, "magic: %u kills / last %u bullets   (%u / %u since injection)", t.recent_kills,
                           t.recent, t.kills, t.shots);
    }
    return changed;
}

void DrawMenu(Runtime& rt, const Config& current) {
    const float k = g_scale;
    const float w = 430 * k;
    ImGui::SetNextWindowPos(ImVec2(12 * k, 40 * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    ImGui::PushFont(nullptr, kMenuSize * k);
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    char k1[32], k2[32], k3[32];
    ImGui::TextColored(kAccent, "GRW");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%s menu  %s HUD  %s ESP", KeyName(current.menu_key, k1, sizeof k1),
                       KeyName(current.panel_key, k2, sizeof k2), KeyName(current.esp_key, k3, sizeof k3));
    // the tabs edit a copy of the settings, in use as soon as it changes (Save: also in grw.ini)
    Config c = current;
    bool changed = false;
    g_label_w = ImGui::CalcTextSize("bullet speed").x + ImGui::GetFontSize();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Aim")) {
            changed |= TabAim(rt, c);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Visuals")) {
            changed |= TabVisuals(rt, c);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Weapon")) {
            changed |= TabWeapon(rt, c);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Bullets")) {
            changed |= TabBullets(rt, c);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            changed |= TabDebug(rt, c);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) rt.SetCfg(c);
    ImGui::Separator();
    static double saved_ms = -1e9;
    static bool save_failed = false;
    if (ImGui::SmallButton("save")) {
        save_failed = !rt.SaveCfg();
        saved_ms = ImGui::GetTime() * 1000.0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("reload")) rt.ReloadCfg();
    ImGui::SameLine();
    if (save_failed) ImGui::TextColored(kBad, "could not write runtime/grw.ini");
    else if (rt.CfgDirty()) ImGui::TextColored(kWarn, "unsaved: lost at the next injection");
    else if (ImGui::GetTime() * 1000.0 - saved_ms < 2000.0) ImGui::TextColored(kAccent, "saved");
    else ImGui::TextColored(kDim, "runtime/grw.ini");
    ImGui::End();
    ImGui::PopFont();
}

}  // namespace

void SetupUi() {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = nullptr;
    char dir[MAX_PATH], path[MAX_PATH];
    const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 32)
        for (const char* name : {"bahnschrift.ttf", "segoeui.ttf"}) {
            std::snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, name);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES &&
                (font = io.Fonts->AddFontFromFileTTF(path, kMenuSize)) != nullptr)
                break;
        }
    if (!font) io.Fonts->AddFontDefault();
}

void DrawPanel() {
    Runtime& rt = Rt();
    const float k = std::clamp(ImGui::GetIO().DisplaySize.y / 1080.0f, 1.0f, 2.0f);
    if (std::fabs(k - g_scale) > 0.01f) ApplyStyle(k);
    DrawEsp(kHudSize * k);
    const bool menu = rt.MenuOpen();
    const int hud = rt.HudLevel();
    if (hud == 0 && !menu) return;
    const Config c = rt.Cfg();
    if (hud > 0) DrawHud(rt, hud == 2 && !menu, c.aim_enabled != 0);  // the menu shows the details itself
    if (menu) DrawMenu(rt, c);
}

}  // namespace grw
