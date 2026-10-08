#include "dll/runtime.h"
#include <windows.h>
#include <cstdio>
#include "core/clock.h"
#include "dll/overlay.h"
#include "dll/panel.h"
#include "telemetry/json_line.h"

namespace grw {

Runtime& Rt() {
    static Runtime rt;
    return rt;
}

bool Runtime::Start(const std::wstring& base_dir, SharedControl* control) {
    control_ = control;
    stats_ = std::make_unique<StageStats>();
    config_ = std::make_unique<ConfigStore>(base_dir + L"\\grw.ini");
    config_->Poll();
    tel_ = std::make_unique<Telemetry>();
    TelemetryOptions opt;
    opt.dir = base_dir + L"\\telemetry";
    opt.on_tick = [this] { Tick(); };
    if (!tel_->Start(opt)) return false;
    window_start_ms_ = esp_window_ms_ = Clock::NowMs();
    const Config c = config_->Snapshot();
    esp_on_ = c.esp_enabled != 0;
    world_.Start(tel_.get(), DWORD(c.esp_scan_interval_ms));
    return true;
}

void Runtime::Stop() {
    world_.Stop();
    if (tel_) tel_->Stop();
}

void Runtime::Status(const char* event, const char* detail) {
    Line l;
    JsonLine(l, "status", tel_->SinceStartMs(), Frame()).Str("event", event).Str("detail", detail).Finish();
    tel_->Push(Channel::Control, l);
}

void Runtime::OnFrame(IDXGISwapChain* sc) {
    const StageSpan total;
    if (last_frame_ms_ > 0) stats_->Add(Stage::Interval, 0.0f, float(total.wall0 - last_frame_ms_));
    last_frame_ms_ = total.wall0;
    frame_.fetch_add(1, std::memory_order_relaxed);
    ++window_frames_;

    const int mode = overlay_mode_.load(std::memory_order_relaxed);
    if (mode > 0) {
        const StageSpan draw;
        const bool overlay_ok = overlay::Frame(sc, &DrawPanel, mode == 2);
        draw.End(*stats_, Stage::Draw);
        if (!overlay_reported_) {
            overlay_reported_ = true;
            Line l;
            JsonLine(l, "status", tel_->SinceStartMs(), Frame())
                .Str("event", "overlay")
                .Str("detail", overlay_ok ? "ok" : "failed")
                .Finish();
            tel_->Push(Channel::Render, l);
        }
    }

    total.End(*stats_, Stage::Total);
    if (Clock::NowMs() - window_start_ms_ >= 10'000.0) PushFrameStats();
}

void Runtime::OnResize(IDXGISwapChain* sc, bool before) {
    if (before) overlay::BeforeResize();
    else overlay::AfterResize(sc);
}

void Runtime::OnPresented(double cpu_ms, double wall_ms) { stats_->Add(Stage::Chain, float(cpu_ms), float(wall_ms)); }

void Runtime::UpdateEsp(float width, float height) {
    if (!esp_on_.load(std::memory_order_relaxed)) {
        esp_.items.clear();
        return;
    }
    const Config c = config_->Snapshot();
    game::EspOptions opt;
    opt.width = width;
    opt.height = height;
    opt.max_dist = c.esp_max_distance_m;
    opt.civilians = c.esp_civilians != 0;
    opt.dead = c.esp_dead != 0;
    world_.BuildFrame(opt, esp_, *stats_);
    if (Clock::NowMs() - esp_window_ms_ >= 1000.0) PushEspStats();
}

void Runtime::PushEspStats() {
    esp_window_ms_ = Clock::NowMs();
    Line l;
    JsonLine j(l, "esp", tel_->SinceStartMs(), Frame());
    j.Bool("camera", esp_.camera_ok)
        .Num("fov", esp_.fov_deg)
        .Num("cx", esp_.camera_pos.x)
        .Num("cy", esp_.camera_pos.y)
        .Num("cz", esp_.camera_pos.z)
        .Int("known", esp_.known)
        .Int("active", esp_.active)
        .Int("on_screen", esp_.on_screen)
        .Int("heads", esp_.heads);
    // the three nearest drawn characters: category, distance, head marker
    const game::EspItem* closest[3] = {};
    for (const game::EspItem& it : esp_.items)
        for (int k = 0; k < 3; ++k)
            if (!closest[k] || it.dist < closest[k]->dist) {
                for (int m = 2; m > k; --m) closest[m] = closest[m - 1];
                closest[k] = &it;
                break;
            }
    char key[16], val[48];
    for (int k = 0; k < 3 && closest[k]; ++k) {
        std::snprintf(key, sizeof key, "n%d", k);
        std::snprintf(val, sizeof val, "%s %.1fm 0x%x %s", game::CategoryName(closest[k]->cat), closest[k]->dist,
                      closest[k]->flags, closest[k]->head_radius > 0 ? "head" : "nohead");
        j.Str(key, val);
    }
    j.Finish();
    tel_->Push(Channel::Render, l);
}

void Runtime::PushFrameStats() {
    stats_->Flush(last_stats_);
    Line l;
    JsonLine j(l, "frame_stats", tel_->SinceStartMs(), Frame());
    j.Int("frames", int64_t(window_frames_)).Int("dropped_render", int64_t(tel_->Dropped(Channel::Render)));
    char key[32];
    for (size_t i = 0; i < kStageCount; ++i) {
        const Percentiles& p = last_stats_[i];
        if (p.n == 0) continue;
        const char* name = StageName(Stage(i));
        std::snprintf(key, sizeof key, "%s_p50", name);
        j.Num(key, p.p50_cpu);
        std::snprintf(key, sizeof key, "%s_p99", name);
        j.Num(key, p.p99_cpu);
        std::snprintf(key, sizeof key, "%s_wall_p50", name);
        j.Num(key, p.p50_wall);
        std::snprintf(key, sizeof key, "%s_wall_p99", name);
        j.Num(key, p.p99_wall);
    }
    j.Finish();
    tel_->Push(Channel::Render, l);
    window_frames_ = 0;
    window_start_ms_ = Clock::NowMs();
}

void Runtime::Tick() {
    const ReloadResult r = config_->Poll();
    if (r.kind != ReloadResult::Unchanged) {
        Line l;
        JsonLine(l, "status", tel_->SinceStartMs(), Frame())
            .Str("event", "config")
            .Str("detail", r.kind == ReloadResult::Applied ? "applied" : r.parse.error.c_str())
            .Int("line", r.parse.line)
            .Finish();
        tel_->Push(Channel::Writer, l);
    }

    const Config c = config_->Snapshot();
    overlay_mode_.store(c.overlay_mode, std::memory_order_relaxed);
    world_.SetInterval(DWORD(c.esp_scan_interval_ms));
    const int keys[4] = {c.menu_key, c.panel_key, c.unload_key, c.esp_key};
    for (int i = 0; i < 4; ++i) {
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !key_down_[i]) {
            if (i == 0) menu_open_ = !menu_open_;
            else if (i == 1) panel_open_ = !panel_open_;
            else if (i == 3) esp_on_ = !esp_on_;
            else InterlockedExchange(&control_->unload_request, 1);
        }
        key_down_[i] = down;
    }
}

}  // namespace grw
