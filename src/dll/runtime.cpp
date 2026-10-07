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
    window_start_ms_ = Clock::NowMs();
    return true;
}

void Runtime::Stop() {
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
    const int keys[3] = {c.menu_key, c.panel_key, c.unload_key};
    for (int i = 0; i < 3; ++i) {
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !key_down_[i]) {
            if (i == 0) menu_open_ = !menu_open_;
            else if (i == 1) panel_open_ = !panel_open_;
            else InterlockedExchange(&control_->unload_request, 1);
        }
        key_down_[i] = down;
    }
}

}  // namespace grw
