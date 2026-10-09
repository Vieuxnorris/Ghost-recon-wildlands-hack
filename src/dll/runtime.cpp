#include "dll/runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include "core/clock.h"
#include "dll/overlay.h"
#include "dll/hwbp.h"
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
    world_.Start(tel_.get(), DWORD(c.esp_scan_interval_ms), DWORD(c.esp_list_interval_ms));
    los_.Start(tel_.get());
    book_path_ = base_dir + L"\\trajectories.txt";
    if (std::ifstream in{book_path_, std::ios::binary}) {
        std::stringstream text;
        text << in.rdbuf();
        TrajectoryBook loaded;
        if (loaded.Parse(text.str())) book_ = std::move(loaded);
    }
    return true;
}

void Runtime::Stop() {
    los_.Stop();
    world_.Stop();
    probe_.Stop();
    SaveTrajectories();
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

// Render thread: the latest verdicts onto this frame's items, then this frame's heads to the worker.
void Runtime::UpdateLineOfSight() {
    los_.Collect(los_verdicts_);
    const double now = Clock::NowMs();
    if (now >= los_purge_ms_) {
        los_purge_ms_ = now + 1000.0;
        std::erase_if(los_verdicts_, [now](const auto& kv) { return now - kv.second.t_ms > 3000.0; });
    }
    los_reqs_.clear();
    const Vec3 fwd = esp_.camera.forward;
    for (game::EspItem& it : esp_.items) {
        if (const auto v = los_verdicts_.find(it.addr); v != los_verdicts_.end()) {
            it.los = v->second.state;
            it.los_age_ms = float(now - v->second.t_ms);
        }
        if (it.head_radius <= 0 || it.cat == game::Category::Dead) continue;
        LosRequest r;
        r.key = it.addr;
        r.point = it.head_world;
        r.feet = it.feet;
        r.vehicle = it.vehicle;
        r.enemy = it.cat == game::Category::SantaBlanca || it.cat == game::Category::Unidad;
        const Vec3 d = it.head_world - esp_.camera_pos;
        const float len = d.Length();
        r.angle = len > 0.0f ? std::acos(std::clamp(d.Dot(fwd) / len, -1.0f, 1.0f)) : 0.0f;
        los_reqs_.push_back(r);
    }
    // rays from where the bullets start: the player's head in third person (the camera only when scoped,
    // where both coincide), camera-findings.md section 9
    const Vec3 origin = esp_.player_head_ok ? esp_.player_head : esp_.camera_pos;
    if (esp_.camera_ok) los_.Submit(origin, esp_.player_ok ? &esp_.player_feet : nullptr, los_reqs_);
}

void Runtime::UpdateEsp(float width, float height) {
    const Config c = config_->Snapshot();
    if (!esp_on_.load(std::memory_order_relaxed) && !c.aim_enabled) {  // the aim needs the same reads
        esp_.items.clear();
        return;
    }
    game::EspOptions opt;
    opt.width = width;
    opt.height = height;
    opt.max_dist = c.esp_max_distance_m;
    opt.head_cone_deg = c.aim_enabled ? c.aim_cone_deg + 7.0f : 0.0f;
    opt.civilians = c.esp_civilians != 0;
    opt.dead = c.esp_dead != 0;
    world_.BuildFrame(opt, esp_, *stats_);
    if (esp_.camera_ok) {
        const float pos[3] = {esp_.camera_pos.x, esp_.camera_pos.y, esp_.camera_pos.z};
        const float fwd[3] = {esp_.camera.forward.x, esp_.camera.forward.y, esp_.camera.forward.z};
        hwbp::SetCamera(pos, fwd);
        cam_x_.store(esp_.camera_pos.x, std::memory_order_relaxed);
        cam_y_.store(esp_.camera_pos.y, std::memory_order_relaxed);
        cam_z_.store(esp_.camera_pos.z, std::memory_order_relaxed);
        los_.SetCamera(esp_.camera_pos, {fwd[0], fwd[1], fwd[2]});
        weapon_speed_.store(esp_.weapon_speed, std::memory_order_relaxed);
        weapon_damage_.store(esp_.weapon_damage, std::memory_order_relaxed);
        weapon_id_.store(esp_.weapon_id, std::memory_order_relaxed);
    }
    if (c.los_enabled) UpdateLineOfSight();
    const StageSpan aim_span;
    AcquireSRWLockShared(&book_lock_);
    BulletKey key;
    const TrajectoryTable* tab = book_.Find({esp_.weapon_speed, esp_.weapon_damage}, &key);
    aim_.Update(esp_, c, *tel_, Frame(), tab, key);
    ReleaseSRWLockShared(&book_lock_);
    if (aim_.Last().target && aim_.Last().trajectory == 2) aim_beyond_ms_.store(Clock::NowMs(), std::memory_order_relaxed);
    aim_span.End(*stats_, Stage::Aim);
    if (Clock::NowMs() - esp_window_ms_ >= 1000.0) PushEspStats();
}

// Writer thread (and Stop): the learned paths survive restarts.
void Runtime::SaveTrajectories() {
    book_saved_ms_ = Clock::NowMs();
    std::string text;
    AcquireSRWLockExclusive(&book_lock_);
    if (book_.Dirty()) {
        text = book_.Serialize();
        book_.ClearDirty();
    }
    ReleaseSRWLockExclusive(&book_lock_);
    if (text.empty() || book_path_.empty()) return;
    const std::wstring tmp = book_path_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
    MoveFileExW(tmp.c_str(), book_path_.c_str(), MOVEFILE_REPLACE_EXISTING);
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
    if (esp_.player_head_ok)
        j.Num("phx", esp_.player_head.x).Num("phy", esp_.player_head.y).Num("phz", esp_.player_head.z);
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
    // cpu, then wall times: one line would not fit (ms, rounded to 0.1 us)
    for (int wall = 0; wall < 2; ++wall) {
        Line l;
        JsonLine j(l, wall ? "frame_wall" : "frame_stats", tel_->SinceStartMs(), Frame());
        if (!wall) j.Int("frames", int64_t(window_frames_)).Int("dropped_render", int64_t(tel_->Dropped(Channel::Render)));
        char key[32];
        for (size_t i = 0; i < kStageCount; ++i) {
            const Percentiles& p = last_stats_[i];
            if (p.n == 0) continue;
            const char* name = StageName(Stage(i));
            std::snprintf(key, sizeof key, "%s_p50", name);
            j.Num(key, std::round((wall ? p.p50_wall : p.p50_cpu) * 1e4) / 1e4);
            std::snprintf(key, sizeof key, "%s_p99", name);
            j.Num(key, std::round((wall ? p.p99_wall : p.p99_cpu) * 1e4) / 1e4);
        }
        j.Finish();
        tel_->Push(Channel::Render, l);
    }
    window_frames_ = 0;
    window_start_ms_ = Clock::NowMs();
}

// Writer thread (proj_track = 2): the tracker teaches something only while the weapon in hand has no learned
// path, has not been fired since injection (its first shot confirms which path it uses), or the aim
// extrapolates past the learned range.
bool Runtime::LearningWanted() const {
    const WeaponSig w{weapon_speed_.load(std::memory_order_relaxed), weapon_damage_.load(std::memory_order_relaxed)};
    const uint64_t id = weapon_id_.load(std::memory_order_relaxed);
    if (!(w.speed > 0) || !id) return false;
    if (std::find(confirmed_weapons_.begin(), confirmed_weapons_.end(), id) == confirmed_weapons_.end()) return true;
    if (Clock::NowMs() - aim_beyond_ms_.load(std::memory_order_relaxed) < 5000.0) return true;
    AcquireSRWLockShared(&book_lock_);
    const bool learned = book_.Find(w) != nullptr;
    ReleaseSRWLockShared(&book_lock_);
    return !learned;
}

void Runtime::PushTrackerStats(bool armed) {
    const double now = Clock::NowMs();
    if (now - tracker_window_ms_ < 10'000.0) return;
    const uint64_t calls = hwbp::TrackerCalls(), samples = hwbp::TrackerSamples();
    const double s = tracker_window_ms_ > 0 ? (now - tracker_window_ms_) / 1000.0 : 0.0;
    if (s > 0) {
        tracker_calls_per_s_.store(float(double(calls - tracker_calls0_) / s), std::memory_order_relaxed);
        Line l;
        JsonLine(l, "proj_stats", tel_->SinceStartMs(), Frame())
            .Bool("armed", armed)
            .Int("calls", int64_t(calls - tracker_calls0_))
            .Int("samples", int64_t(samples - tracker_samples0_))
            .Num("calls_per_s", double(calls - tracker_calls0_) / s)
            .Int("confirmed_weapons", int64_t(confirmed_weapons_.size()))
            .Finish();
        tel_->Push(Channel::Writer, l);
    }
    tracker_window_ms_ = now;
    tracker_calls0_ = calls;
    tracker_samples0_ = samples;
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
    world_.SetIntervals(DWORD(c.esp_scan_interval_ms), DWORD(c.esp_list_interval_ms));
    uintptr_t watch[4] = {uintptr_t(c.hwbp_addr), uintptr_t(c.hwbp_addr2), uintptr_t(c.hwbp_addr3),
                          uintptr_t(c.hwbp_addr4)};
    hwbp::Mode modes[4];
    for (hwbp::Mode& m : modes) m = hwbp::Mode(c.hwbp_read);
    // The tracker costs one exception per simulation step of every projectile in the game (all characters'):
    // in mode 2 it is armed only while it teaches something, plus 3 s for the bullets still in flight.
    bool track = c.proj_track == 1;
    if (c.proj_track == 2) {
        const double now = Clock::NowMs();
        if (LearningWanted()) track_until_ms_ = now + 3000.0;
        track = now < track_until_ms_;
    }
    if (track) {
        watch[1] = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + game::kProjSpeedFnRva;
        modes[1] = hwbp::Mode::Execute;
    }
    if (track) hwbp::SetTracker(1);  // before arming; after disarming below, so no step is taken for a research hit
    tracker_armed_.store(track, std::memory_order_relaxed);
    PushTrackerStats(track);
    los_.SetTest(c.los_test != 0);
    LosParams lp;
    lp.interval_ms = c.los_interval_ms;
    lp.max_rays = c.los_max_rays;
    lp.mask = c.los_mask;
    lp.flag = c.los_flag != 0;
    lp.debug = c.los_debug != 0;
    los_.SetParams(lp);
    if (const uintptr_t w = probe_.Watch()) {  // the bullet probe borrows DR3 for a moment
        watch[3] = w;
        modes[3] = hwbp::Mode::Write;
    }
    hwbp::SetTrigger(c.hwbp_trigger, c.hwbp_window_ms);
    hwbp::Configure(watch, modes, c.hwbp_len, c.hwbp_max_hits);
    if (!track) hwbp::SetTracker(-1);
    hwbp::Drain(
        *tel_,
        c.bullet_probe ? +[](const hwbp::HitView& h, void* ctx) {
            if (h.rip_rva != game::kFireRva || !h.ok_rdx) return;
            float o[3], d[3];
            std::memcpy(o, h.mem_rdx + game::kFireOriginOff, sizeof o);
            std::memcpy(d, h.mem_rdx + game::kFireDirOff, sizeof d);
            auto* rt = static_cast<Runtime*>(ctx);
            const Vec3 origin{o[0], o[1], o[2]};
            const Vec3 cam{rt->cam_x_.load(), rt->cam_y_.load(), rt->cam_z_.load()};
            if ((origin - cam).Length() > 6.0f) return;  // another character's shot
            const bullet::WeaponInfo weapon{rt->weapon_speed_.load(), rt->weapon_damage_.load()};
            rt->probe_.Shot(origin, {d[0], d[1], d[2]}, weapon, *rt->tel_);
        } : nullptr,
        this,
        [](const hwbp::ProjView& s, void* ctx) {
            // along the launch direction, and below it in its vertical plane
            const Vec3 o{s.origin[0], s.origin[1], s.origin[2]}, p{s.pos[0], s.pos[1], s.pos[2]};
            Vec3 u{s.dir0[0], s.dir0[1], s.dir0[2]};
            const float ul = u.Length();
            if (!(ul > 0.5f)) return;
            u = u * (1.0f / ul);
            Vec3 right{u.y, -u.x, 0};
            const float rl = right.Length();
            right = rl > 1e-4f ? right * (1.0f / rl) : Vec3{1, 0, 0};
            const Vec3 up{right.y * u.z - right.z * u.y, right.z * u.x - right.x * u.z, right.x * u.y - right.y * u.x};
            const Vec3 r = p - o;
            auto* rt = static_cast<Runtime*>(ctx);
            const BulletKey key{s.V, s.v0, s.first_segment};
            const WeaponSig weapon{rt->weapon_speed_.load(), rt->weapon_damage_.load()};
            // Steep shots fly another profile (live/74: -15 deg starts at 0.69 V instead of 0.83 V): learn the
            // near-horizontal ones only, the aim's use.
            const bool learn = std::fabs(u.z) < 0.1736f;  // 10 deg
            const bool just_fired = s.elapsed < 0.1f && weapon.speed > 0;  // the weapon in hand
            AcquireSRWLockExclusive(&rt->book_lock_);
            if (just_fired) rt->book_.Link(weapon, key);
            if (learn) rt->book_.Add(key, r.Dot(u), -r.Dot(up), s.elapsed);
            ReleaseSRWLockExclusive(&rt->book_lock_);
            const uint64_t id = rt->weapon_id_.load(std::memory_order_relaxed);
            auto& conf = rt->confirmed_weapons_;
            if (just_fired && id && std::find(conf.begin(), conf.end(), id) == conf.end()) conf.push_back(id);
        });
    if (Clock::NowMs() - book_saved_ms_ > 5000.0) SaveTrajectories();
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
