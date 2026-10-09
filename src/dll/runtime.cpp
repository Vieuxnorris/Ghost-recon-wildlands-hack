#include "dll/runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include "core/clock.h"
#include "core/mem.h"
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
    RestoreDataMods();
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
        const bool overlay_ok = overlay::Frame(sc, &SetupUi, &DrawPanel, mode == 2);
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
namespace {

// A magic bullet's approaches to a target (k = 0..5), as directions of travel: from the player, from above, from above
// on the player's side, from above on the far side, from the left, from the right (h: player -> target,
// horizontal). The LOS worker tests the last kMagicApproachM of each: that is all the bullet will cross.
constexpr int kMagicApproaches = 6;
constexpr int kMagicOrder[kMagicApproaches] = {0, 2, 1, 3, 4, 5};  // preference
constexpr float kMagicApproachM = 12.0f;  // = hwbp's kMagicBefore: where the last test starts
constexpr float kMagicMinStartM = 2.0f;   // the closest start past an obstacle
// magic_pierce: their penetration energy (the R4's: 0.112). A helicopter's body costs 1.5, some obstacle (terrain? class
// 0x3AD45B8) 3500-4900: 1000 stopped 4 bullets 10 m short of a head. The damage falls with the fraction used: kept.
constexpr float kMagicPierce = 1e7f;
Vec3 MagicApproach(int k, const Vec3& player, const Vec3& head) {
    Vec3 h{head.x - player.x, head.y - player.y, 0.0f};
    const float hl = h.Length();
    h = hl > 0.01f ? h * (1.0f / hl) : Vec3{1, 0, 0};
    const Vec3 down{0, 0, -1}, side{h.y, -h.x, 0};
    const float r = 0.70710678f;
    switch (k) {
        case 0: {
            const Vec3 d = head - player;
            const float l = d.Length();
            return l > 0.01f ? d * (1.0f / l) : h;
        }
        case 1: return down;
        case 2: return (h + down) * r;
        case 3: return (h * -1.0f + down) * r;
        case 4: return side;
        default: return side * -1.0f;
    }
}
uintptr_t ApproachKey(uintptr_t addr, int k) { return addr | (uintptr_t(k) << 56); }
bool IsEnemy(game::Category c) { return c == game::Category::SantaBlanca || c == game::Category::Unidad; }

// A moving target: the last test runs along its motion through its head, start_m behind and ahead of it, so a timing
// error moves the head along that line, not off it (measured: 97 % of the missed bullets went to moving targets, at
// 11 m/s median; the kills, 4.7 m/s). Vehicles do not block the bullets' rays.
void AlongMotion(hwbp::MagicTarget& t, int& way, const Vec3& vel, float start_m) {
    const float sp = vel.Length();
    if (sp < 2.0f) return;
    t.dir[0] = vel.x / sp, t.dir[1] = vel.y / sp, t.dir[2] = vel.z / sp;
    t.before = start_m;
    t.after = start_m;
    way = 30;
}

}  // namespace

void Runtime::UpdateLineOfSight(const Config& c) {
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
        // magic bullets: which ways to this enemy's head are clear over their last metres? (piercing ones go through)
        if (MagicBullets() && c.magic_walls && !c.magic_pierce && r.enemy) {
            const Vec3 player = esp_.player_head_ok ? esp_.player_head : esp_.camera_pos;
            for (int k = 0; k < kMagicApproaches; ++k) {
                LosRequest a = r;
                a.key = ApproachKey(it.addr, k);
                a.has_from = true;
                a.from = it.head_world - MagicApproach(k, player, it.head_world) * c.magic_start_m;
                a.angle = r.angle + 0.001f * float(k);
                los_reqs_.push_back(a);
            }
        }
    }
    // rays from where the bullets start: the player's head in third person (the camera only when scoped,
    // where both coincide), camera-findings.md section 9
    const Vec3 origin = esp_.player_head_ok ? esp_.player_head : esp_.camera_pos;
    if (esp_.camera_ok) los_.Submit(origin, esp_.player_ok ? &esp_.player_feet : nullptr, los_reqs_);
}

void Runtime::LogMagicHits(const MagicPending& p, bool killed) {
    static hwbp::MagicHitDump d;  // render thread only; ~3 KB
    if (!hwbp::MagicHits(p.seq, d)) return;
    const Vec3 from{d.from[0], d.from[1], d.from[2]}, to{d.to[0], d.to[1], d.to[2]};
    const Vec3 head{d.head[0], d.head[1], d.head[2]};
    Vec3 u = to - from;
    const float len = u.Length();
    u = len > 0 ? u * (1.0f / len) : Vec3{};
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char hex[0xB0 * 2 + 1], buf[24];
    auto to_hex = [&hex](const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i) std::snprintf(hex + 2 * i, 3, "%02x", b[i]);
        return hex;
    };
    auto ptr = [&buf](uint64_t v) {
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
        return buf;
    };
    const auto tr = magic_tracks_.find(p.addr);
    {
        Line l;
        JsonLine j(l, "magic_seg", tel_->SinceStartMs(), Frame());
        j.Int("seq", int64_t(p.seq)).Bool("killed", killed).Str("target", ptr(d.target)).Num("len", len);
        j.Num("head_along", (head - from).Dot(u));
        j.Num("fx", from.x).Num("fy", from.y).Num("fz", from.z).Num("tx", to.x).Num("ty", to.y).Num("tz", to.z);
        j.Num("hx", head.x).Num("hy", head.y).Num("hz", head.z);
        if (tr != magic_tracks_.end()) j.Num("feet_dz", tr->second.feet.z - head.z);
        j.Num("flown", d.flown).Num("segment", d.segment);
        j.Int("n_fwd", d.n_fwd).Int("n_rev", d.n_rev).Int("n_kept", d.n_kept);
        j.Str("s0", to_hex(d.s0, sizeof d.s0));
        j.Str("s1", to_hex(d.s1, sizeof d.s1));
        j.Str("s2", to_hex(d.s2, sizeof d.s2));
        j.Str("s3", to_hex(d.s3, sizeof d.s3));
        j.Finish();
        tel_->Push(Channel::Render, l);
    }
    // each ray hit: where along the segment from the head (< 0: before it), how far off its line, entering or leaving
    for (int q = 0; q < 2; ++q) {
        const int n = std::min(q ? d.n_rev : d.n_fwd, hwbp::kMagicDumpHits);
        for (int i = 0; i < n; ++i) {
            const hwbp::MagicRayHit& h = q ? d.rev[i] : d.fwd[i];
            float f[0x80 / 4];
            std::memcpy(f, h.raw, sizeof f);
            uint64_t obj;
            std::memcpy(&obj, h.raw + 0x30, sizeof obj);
            const Vec3 pos{f[0], f[1], f[2]}, nrm{f[4], f[5], f[6]};
            const float along = (pos - head).Dot(u);
            const Vec3 off = (pos - head) - u * along;
            Line l;
            JsonLine j(l, "magic_hit", tel_->SinceStartMs(), Frame());
            j.Int("seq", int64_t(p.seq)).Str("q", q ? "r" : "f").Int("i", i).Num("along", along).Num("perp", off.Length());
            j.Num("nd", nrm.Dot(u)).Num("dist", f[0x48 / 4]);
            j.Str("obj", ptr(obj));
            j.Str("vt", ptr(h.vtable >= image && h.vtable < image + 0x60000000ull ? h.vtable - image : h.vtable));
            j.Str("owner", ptr(h.owner)).Bool("tgt", h.owner == d.target || obj == d.target);
            j.Str("raw", to_hex(h.raw, sizeof h.raw));
            j.Finish();
            tel_->Push(Channel::Render, l);
        }
    }
    for (int i = 0; i < std::min(d.n_kept, hwbp::kMagicDumpKept); ++i) {
        const uint8_t* r = d.kept[i].raw;
        float f[4];
        std::memcpy(f, r + 0x10, sizeof f);
        uint64_t obj;
        std::memcpy(&obj, r + 0x40, sizeof obj);
        const Vec3 pos{f[0], f[1], f[2]};
        const float along = (pos - head).Dot(u);
        Line l;
        JsonLine j(l, "magic_kept", tel_->SinceStartMs(), Frame());
        j.Int("seq", int64_t(p.seq)).Int("i", i).Num("along", along).Num("perp", ((pos - head) - u * along).Length());
        j.Str("obj", ptr(obj)).Bool("tgt", obj == d.target).Int("flag", r[0xA4]);
        j.Str("raw", to_hex(r, sizeof d.kept[i].raw));
        j.Finish();
        tel_->Push(Channel::Render, l);
    }
}

void Runtime::UpdateMagic(const Config& cfg) {
    const float cfg_start = cfg.magic_start_m;
    const bool cfg_walls = cfg.magic_walls != 0, vertical_est = cfg.magic_vertical_est != 0;
    hwbp::SetMagicPierce(cfg.magic_pierce ? kMagicPierce : 0.0f);
    const double now = Clock::NowMs();
    // the bullets sent since the last frame, then their result: the target dead within 1.5 s, or not
    hwbp::MagicShot shots[16];
    for (int n; (n = hwbp::MagicShotsSince(magic_seq_, shots, 16)) > 0;)
        for (int i = 0; i < n; ++i)
            magic_pending_.push_back({shots[i].addr, now, shots[i].dist, shots[i].approach, shots[i].seq});
    for (auto p = magic_pending_.begin(); p != magic_pending_.end();) {
        const game::EspItem* item = nullptr;
        for (const game::EspItem& it : esp_.items)
            if (it.addr == p->addr) item = &it;
        const game::EspFrame::OffscreenChar* off = nullptr;
        if (!item)
            for (const auto& o : esp_.offscreen)
                if (o.addr == p->addr) off = &o;
        const bool dead = (item && item->cat == game::Category::Dead) || (off && off->cat == game::Category::Dead);
        if (!dead && now - p->ms < 1500.0) {
            ++p;
            continue;
        }
        Line l;
        JsonLine(l, "magic_shot", tel_->SinceStartMs(), Frame())
            .Int("seq", int64_t(p->seq))
            .Num("dist", p->dist)
            .Int("approach", p->approach)
            .Bool("vehicle", (item && item->vehicle) || (off && off->vehicle))
            .Bool("seen", item != nullptr || off != nullptr)
            .Bool("on_screen", item != nullptr)
            .Int("way", magic_tracks_.count(p->addr) ? magic_tracks_[p->addr].k : -2)
            .Bool("est_head", magic_tracks_.count(p->addr) && magic_tracks_[p->addr].est)
            .Num("start_m", magic_tracks_.count(p->addr) ? magic_tracks_[p->addr].start_m : -1.0f)
            .Str("cat", magic_tracks_.count(p->addr) ? game::CategoryName(magic_tracks_[p->addr].cat) : "?")
            .Num("speed", magic_tracks_.count(p->addr) ? magic_tracks_[p->addr].vel.Length() : -1.0f)
            .Num("dz", magic_tracks_.count(p->addr) ? magic_tracks_[p->addr].pos.z - esp_.camera_pos.z : 0.0f)
            .Bool("killed", dead)
            .Num("dead_ms", dead ? now - p->ms : -1.0)
            .Finish();
        tel_->Push(Channel::Render, l);
        if (cfg.magic_debug) LogMagicHits(*p, dead);
        magic_recent_[magic_shots_ % kMagicRecent] = dead;
        ++magic_shots_;
        magic_kills_ += dead;
        p = magic_pending_.erase(p);
    }
    if (!MagicBullets() || !tracker_armed_.load(std::memory_order_relaxed) || !esp_.camera_ok) {
        hwbp::SetMagicTargets(nullptr, 0);
        return;
    }
    struct Cand {
        hwbp::MagicTarget t;
        int shots;
        float angle;
    };
    static std::vector<Cand> cands;  // render thread only
    cands.clear();
    const Vec3 player = esp_.player_head_ok ? esp_.player_head : esp_.camera_pos;
    const Vec3 fwd = esp_.camera.forward;
    // the options: the aim's target only, a cone around the crosshair, a range
    const uintptr_t locked = cfg.magic_target == 0 ? aim_.Last().target : 0;
    if (cfg.magic_target == 0 && !locked) {
        hwbp::SetMagicTargets(nullptr, 0);
        return;
    }
    const float max_angle = cfg.magic_fov_deg * 0.017453293f;
    auto wanted = [&](uintptr_t addr, game::Category cat, const Vec3& head) {
        if (!IsEnemy(cat) || (locked && addr != locked)) return false;
        const Vec3 to = head - esp_.camera_pos;
        const float len = to.Length();
        if (len > cfg.magic_max_m) return false;
        return cfg.magic_fov_deg >= 180.0f || (len > 0 && std::acos(std::clamp(to.Dot(fwd) / len, -1.0f, 1.0f)) <= max_angle);
    };
    for (const game::EspItem& it : esp_.items) {
        if (!IsEnemy(it.cat)) continue;
        // the head from the bones, else from the feet (far targets off the crosshair have no pose read)
        const Vec3 head = it.head_radius > 0 ? it.head_world : it.feet + Vec3{0, 0, it.vehicle ? 1.0f : 1.55f};
        if (!wanted(it.addr, it.cat, head)) continue;
        MagicTrack& tr = magic_tracks_[it.addr];
        if (tr.ms > 0 && now - tr.ms > 1.0 && now - tr.ms < 1000.0) {
            const float dts = float((now - tr.ms) / 1000.0);
            const Vec3 inst = (head - tr.pos) * (1.0f / dts);
            if (inst.Length() < 150.0f) tr.vel = tr.vel + (inst - tr.vel) * (1.0f - std::exp(-dts / 0.15f));  // helicopters
        } else if (tr.ms > 0 && now - tr.ms >= 1000.0) {
            tr.vel = {};
        }
        if (tr.ms == 0 || now - tr.ms > 1.0) {
            tr.pos = head;
            tr.ms = now;
        }
        tr.feet = it.feet;
        Cand c{};
        c.t.addr = it.addr;
        c.t.head[0] = head.x, c.t.head[1] = head.y, c.t.head[2] = head.z;
        c.t.vel[0] = tr.vel.x, c.t.vel[1] = tr.vel.y, c.t.vel[2] = tr.vel.z;
        // the first clear way in order of preference; none known: from the bullet's side
        tr.k = -1;
        tr.est = it.head_radius <= 0;
        tr.cat = it.cat;
        tr.vehicle = it.vehicle;
        tr.start_m = cfg_start;
        c.t.before = cfg_start;
        for (int k : kMagicOrder) {
            if (!cfg_walls) break;
            const auto v = los_verdicts_.find(ApproachKey(it.addr, k));
            if (v == los_verdicts_.end() || v->second.state != game::kLosVisible || now - v->second.t_ms > 1500.0)
                continue;
            tr.k = k;
            if (k > 0) {
                const Vec3 d = MagicApproach(k, player, head);
                c.t.dir[0] = d.x, c.t.dir[1] = d.y, c.t.dir[2] = d.z;
            }
            break;
        }
        // every way blocked within its last 12 m: the one blocked farthest from the head, the test starting just past
        // its obstacle (kMagicMinStartM at least: from 0.5 m, 1 hit in 6)
        if (tr.k < 0 && cfg_walls) {
            float best = 0;
            for (int k = 0; k < kMagicApproaches; ++k) {
                const auto v = los_verdicts_.find(ApproachKey(it.addr, k));
                if (v == los_verdicts_.end() || v->second.state != game::kLosHidden || now - v->second.t_ms > 1500.0)
                    continue;
                const float start = cfg_start - v->second.hit_dist - 0.3f;
                if (start > best) {
                    best = start;
                    tr.k = 10 + k;
                }
            }
            if (best >= kMagicMinStartM) {
                const Vec3 d = MagicApproach(tr.k - 10, player, head);
                c.t.dir[0] = d.x, c.t.dir[1] = d.y, c.t.dir[2] = d.z;
                c.t.before = best;
                tr.start_m = best;
            } else {
                tr.k = -1;
            }
        }
        // a head not read: from above, down its whole body (a wrong height then still hits it)
        if (tr.est && vertical_est && tr.k < 0) {
            c.t.dir[0] = 0, c.t.dir[1] = 0, c.t.dir[2] = -1;
            c.t.after = 2.6f;
            tr.k = 20;
        }
        AlongMotion(c.t, tr.k, tr.vel, cfg_start);
        c.shots = 0;
        for (const MagicPending& p : magic_pending_) c.shots += p.addr == it.addr;
        const Vec3 to = head - esp_.camera_pos;
        const float len = to.Length();
        c.angle = len > 0 ? std::acos(std::clamp(to.Dot(fwd) / len, -1.0f, 1.0f)) : 0.0f;
        cands.push_back(c);
    }
    // the enemies off the screen: their head when its bones were read (enemy_heads), else their root
    for (const auto& o : esp_.offscreen) {
        if (!IsEnemy(o.cat)) continue;
        const Vec3 head = o.head_ok ? o.head : o.feet + Vec3{0, 0, o.vehicle ? 1.0f : 1.55f};
        if (!wanted(o.addr, o.cat, head)) continue;
        MagicTrack& tr = magic_tracks_[o.addr];
        if (tr.ms > 0 && now - tr.ms > 1.0 && now - tr.ms < 1000.0) {
            const float dts = float((now - tr.ms) / 1000.0);
            const Vec3 inst = (head - tr.pos) * (1.0f / dts);
            if (inst.Length() < 150.0f) tr.vel = tr.vel + (inst - tr.vel) * (1.0f - std::exp(-dts / 0.15f));
        } else if (tr.ms > 0 && now - tr.ms >= 1000.0) {
            tr.vel = {};
        }
        if (tr.ms == 0 || now - tr.ms > 1.0) {
            tr.pos = head;
            tr.ms = now;
        }
        tr.feet = o.feet;
        tr.k = -1;
        tr.est = !o.head_ok;
        tr.cat = o.cat;
        tr.vehicle = o.vehicle;
        tr.start_m = cfg_start;
        Cand c{};
        c.t.addr = o.addr;
        c.t.before = cfg_start;
        if (tr.est && vertical_est) {
            c.t.dir[0] = 0, c.t.dir[1] = 0, c.t.dir[2] = -1;
            c.t.after = 2.6f;
            tr.k = 20;
        }
        AlongMotion(c.t, tr.k, tr.vel, cfg_start);
        c.t.head[0] = head.x, c.t.head[1] = head.y, c.t.head[2] = head.z;
        c.t.vel[0] = tr.vel.x, c.t.vel[1] = tr.vel.y, c.t.vel[2] = tr.vel.z;
        c.shots = 0;
        for (const MagicPending& p : magic_pending_) c.shots += p.addr == o.addr;
        const Vec3 to = head - esp_.camera_pos;
        const float len = to.Length();
        c.angle = len > 0 ? std::acos(std::clamp(to.Dot(fwd) / len, -1.0f, 1.0f)) : 3.2f;
        cands.push_back(c);
    }
    // one bullet per enemy in turn (the least shot first), nearest the crosshair first; or all on the nearest
    const bool spread = cfg.magic_spread != 0;
    std::sort(cands.begin(), cands.end(), [spread](const Cand& a, const Cand& b) {
        return spread && a.shots != b.shots ? a.shots < b.shots : a.angle < b.angle;
    });
    if (!spread && cands.size() > 1) cands.resize(1);
    hwbp::MagicTarget out[hwbp::kMaxMagicTargets];
    int n = 0;
    for (const Cand& c : cands)
        if (n < hwbp::kMaxMagicTargets) out[n++] = c.t;
    hwbp::SetMagicTargets(out, n);
    if (now >= magic_purge_ms_) {
        magic_purge_ms_ = now + 1000.0;
        std::erase_if(magic_tracks_, [now](const auto& kv) { return now - kv.second.ms > 3000.0; });
    }
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
    opt.enemy_heads = MagicBullets();  // the magic bullets aim at every enemy, off the screen too
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
        const float head[3] = {esp_.player_head.x, esp_.player_head.y, esp_.player_head.z};
        hwbp::SetPlayerHead(head, esp_.player_head_ok);
    }
    // weapon mods: the spread at each shot (writer thread arms it), the ballistics block here
    spread_comp_.store(esp_.weapon.spread_comp, std::memory_order_relaxed);
    weapon_logic_.store(esp_.weapon.logic, std::memory_order_relaxed);
    aim_comp_.store(esp_.aim.ok ? esp_.aim.comp : 0, std::memory_order_relaxed);
    esp_.weapon.spread_factor = ModFactor(Mod::Spread);
    ApplyDataMods();
    bullet_V_.store(esp_.bullet_ok ? esp_.bullet_V_table : 0.0f, std::memory_order_relaxed);  // learning: game's V
    bullet_first_.store(esp_.bullet_ok ? esp_.bullet_first_segment : 0.0f, std::memory_order_relaxed);
    if (c.los_enabled) UpdateLineOfSight(c);
    const StageSpan aim_span;
    AcquireSRWLockShared(&book_lock_);
    BulletKey key;
    // the table of the bullets the weapon in hand fires now (its parts included), else of its last shots
    // (keyed by the game's V: the speed mod changes the time, not the path)
    const TrajectoryTable* tab = esp_.bullet_ok ? book_.FindSetup(esp_.bullet_V_table, esp_.bullet_first_segment, &key)
                                                : book_.Find({esp_.weapon_speed, esp_.weapon_damage}, &key);
    aim_.Update(esp_, c, *tel_, Frame(), tab, key, &book_.Curve());
    ReleaseSRWLockShared(&book_lock_);
    UpdateMagic(c);
    if (aim_.Last().target && aim_.Last().trajectory == 2) aim_beyond_ms_.store(Clock::NowMs(), std::memory_order_relaxed);
    aim_span.End(*stats_, Stage::Aim);
    if (Clock::NowMs() - esp_window_ms_ >= 1000.0) PushEspStats();
}

// Render thread: bullet speed and damage of the weapon in hand, as factors of the game's values. The game rewrites
// these values when the parts change (and a new weapon object gets new blocks): its value is then taken again as
// the base. Back to 1: the game's value is put back.
// Speed: the bullets take V from the shooter's request (model.h, kShooterRequestOff); the ballistics block's V is
// kept equal to it (the setup reads it for the first step). Damage: the ballistics block, read at each shot.
void Runtime::ApplyDataMods() {
    game::WeaponView& w = esp_.weapon;
    // One value at one address: the game's under ours (factor k, or `absolute` when > 0). A value that is not ours
    // is the game's, unless it lies more than 3x away from `ref` (> 0: the game's value known elsewhere): then it is
    // a leftover of an older DLL, repaired. Returns the game's value, -1 = unknown.
    auto apply = [this](int kind, uintptr_t addr, float k, float absolute, float ref) -> float {
        float cur = 0;
        if (!addr || !mem::Read(addr, cur) || !std::isfinite(cur)) return -1;
        auto it = data_mods_.find(addr);
        if (it != data_mods_.end() && (it->second.kind != kind || cur != it->second.written)) {
            data_mods_.erase(it);  // the game rewrote it
            it = data_mods_.end();
        }
        float game = cur;
        if (it != data_mods_.end()) game = it->second.game;
        else if (ref > 0 && (cur > 3.0f * ref || cur < ref / 3.0f)) game = ref;
        if (k == 1.0f && absolute <= 0) {
            if (cur != game) mem::Write(addr, game);
            if (it != data_mods_.end()) data_mods_.erase(it);
            return game;
        }
        const float want = absolute > 0 ? absolute : game * k;
        if (cur != want && !mem::Write(addr, want)) return game;
        if (data_mods_.size() > 32)  // weapons left behind: drop the entries the game rewrote or freed
            for (auto i = data_mods_.begin(); i != data_mods_.end();) {
                float v = 0;
                i = mem::Read(i->first, v) && v == i->second.written ? std::next(i) : data_mods_.erase(i);
            }
        data_mods_[addr] = {kind, game, want};
        return game;
    };
    const float speed_k = ModFactor(Mod::BulletSpeed);
    const bool instant = InstantBullets();
    // the game's V: the ballistics block's copy, which the mods leave alone (an older DLL wrote it: plausible only)
    const float block_v = esp_.bullet_V_block > 10 && esp_.bullet_V_block < 3000 ? esp_.bullet_V_block : 0.0f;
    if (w.request) {
        const float game_v = apply(0, w.request + game::kRequestSpeedOff, speed_k, instant ? kInstantBulletSpeed : 0.0f,
                                   block_v);
        if (game_v > 10 && game_v < 3000) {
            esp_.bullet_V_table = game_v;
            // the block's copy, if an older DLL left its value there
            if (w.ballistics && esp_.bullet_V_block != game_v)
                mem::Write(w.ballistics + game::kBallisticsSpeedOff, game_v);
        }
    }
    // the reference for the damage: the weapon's design data (camera-findings.md section 6), which the mods leave alone
    if (w.ballistics)
        apply(1, w.ballistics + game::kBallisticsDamageOff, ModFactor(Mod::Damage), 0.0f, esp_.weapon_damage);
    // straight or teleported bullets: the tracker rewrites the paths of the player's bullets (hwbp::SetBulletPath)
    const bool armed = tracker_armed_.load(std::memory_order_relaxed);
    esp_.bullet_teleport = TeleportBullets() && armed;
    esp_.bullet_straight = (StraightBullets() || TeleportBullets()) && armed;
    ApplyAmmoFlag(InfiniteAmmo());
}

void Runtime::ApplyAmmoFlag(bool on) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t obj = 0, vt = 0;
    uint8_t cur = 0;
    if (!mem::Read(base + game::kGameRulesPtrRva, obj) || !obj || !mem::Read(obj, vt) ||
        vt != base + game::kGameRulesVtableRva || !mem::Read(obj + game::kRulesInfiniteAmmoOff, cur)) {
        ammo_flag_ = -1;
        return;
    }
    if (on && cur == 0) {
        if (mem::Write(obj + game::kRulesInfiniteAmmoOff, uint8_t(1))) {
            ammo_flag_obj_ = obj;
            cur = 1;
        }
    } else if (!on && ammo_flag_obj_) {
        if (ammo_flag_obj_ == obj && cur == 1 && mem::Write(obj + game::kRulesInfiniteAmmoOff, uint8_t(0))) cur = 0;
        ammo_flag_obj_ = 0;
    }
    ammo_flag_ = cur;
}

// The game's values back where ours still stand (the next DLL would take ours for the game's).
void Runtime::RestoreDataMods() {
    for (const auto& [addr, d] : data_mods_) {
        float cur = 0;
        if (mem::Read(addr, cur) && cur == d.written) mem::Write(addr, d.game);
    }
    data_mods_.clear();
    ApplyAmmoFlag(false);
}

bool Runtime::SaveCfg() {
    const bool ok = config_->Save();
    if (ok) cfg_dirty_.store(false, std::memory_order_relaxed);
    return ok;
}

Runtime::MagicTally Runtime::MagicResults() const {
    MagicTally t;
    t.shots = magic_shots_;
    t.kills = magic_kills_;
    t.recent = std::min<uint32_t>(magic_shots_, kMagicRecent);
    for (uint32_t i = 0; i < t.recent; ++i) t.recent_kills += magic_recent_[i];
    return t;
}

uint64_t Runtime::SpreadModHits() const { return hwbp::ScaleModHits(0); }
uint64_t Runtime::RecoilModHits() const { return hwbp::ScaleModHits(1); }

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
    // the player's weapon objects now: W, shooter, weapon state, the fire handler of the last shot (research)
    const hwbp::ShotStats shots = hwbp::ShotStatsNow();
    char ptrs[80];
    std::snprintf(ptrs, sizeof ptrs, "%llx %llx %llx %llx", static_cast<unsigned long long>(esp_.weapon.logic),
                  static_cast<unsigned long long>(esp_.weapon.shooter), static_cast<unsigned long long>(esp_.weapon_state),
                  static_cast<unsigned long long>(hwbp::LastShotHandler()));
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
    // the weapon's bullets (its parts included) and the shooter's spread: what the speed law starts from
    if (esp_.bullet_ok) j.Num("bullet_V", esp_.bullet_V).Num("bullet_first", esp_.bullet_first_segment);
    if (esp_.spread_ok) j.Num("spread", esp_.spread).Num("spread_range", esp_.spread_range);
    const game::WeaponView& wv = esp_.weapon;
    if (wv.spread_live >= 0) j.Num("spread_live", wv.spread_live);
    if (wv.damage > 0) j.Num("damage", wv.damage);
    if (wv.magazine) j.Int("magazine", wv.magazine);
    if (wv.stats_ok) {
        char stats[96];
        std::snprintf(stats, sizeof stats, "%.1f %.1f %.1f %.1f %.1f %.1f %.1f", wv.Stat(0), wv.Stat(1), wv.Stat(2),
                      wv.Stat(3), wv.Stat(4), wv.Stat(5), wv.Stat(6));
        j.Str("stats", stats);
    }
    j.Num("mod_spread", ModFactor(Mod::Spread))
        .Num("mod_speed", ModFactor(Mod::BulletSpeed))
        .Num("mod_damage", ModFactor(Mod::Damage))
        .Num("mod_recoil", ModFactor(Mod::Recoil))
        .Bool("mod_instant", InstantBullets())
        .Bool("mod_straight", StraightBullets())
        .Bool("mod_teleport", TeleportBullets())
        .Bool("mod_magic", MagicBullets())
        .Bool("mod_inf_ammo", InfiniteAmmo())
        .Int("mod_pellets", pellets_.load(std::memory_order_relaxed))
        .Int("pellet_shots", int64_t(hwbp::PelletShots()))
        .Int("player_shots", int64_t(shots.count))
        .Str("ptrs", ptrs)
        .Num("shot_ms", shots.interval_ms)
        .Int("ammo_flag", ammo_flag_)
        .Int("mod_spread_hits", int64_t(hwbp::ScaleModHits(0)))
        .Int("mod_recoil_hits", int64_t(hwbp::ScaleModHits(1)));
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
    const float V = bullet_V_.load(std::memory_order_relaxed), first = bullet_first_.load(std::memory_order_relaxed);
    if (!(w.speed > 0) || !id) return false;
    if (Clock::NowMs() - aim_beyond_ms_.load(std::memory_order_relaxed) < 5000.0) return true;
    AcquireSRWLockShared(&book_lock_);
    // known setup (read from the weapon): a table for it; else the weapon's link, confirmed by a shot
    const bool learned = V > 0 ? book_.FindSetup(V, first) != nullptr : book_.Find(w) != nullptr;
    ReleaseSRWLockShared(&book_lock_);
    if (!learned) return true;
    return !(V > 0) &&
           std::find(confirmed_weapons_.begin(), confirmed_weapons_.end(), id) == confirmed_weapons_.end();
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
            .Int("straightened", int64_t(hwbp::Straightened().paths))
            .Num("straight_drop_m", hwbp::Straightened().drop_m)
            .Num("straight_first_m", hwbp::Straightened().first_m)
            .Int("straight_points", hwbp::Straightened().points)
            .Int("teleported", int64_t(hwbp::Straightened().teleported))
            .Int("magic", int64_t(hwbp::Magic().bullets))
            .Num("magic_dist_m", hwbp::Magic().last_dist_m)
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
        if (r.kind == ReloadResult::Applied) cfg_dirty_.store(false, std::memory_order_relaxed);
    }

    const Config c = config_->Snapshot();
    overlay_mode_.store(c.overlay_mode, std::memory_order_relaxed);
    // the mods' factors: grw.ini when it was (re)loaded, the menu's sliders in between
    const float cfg_mods[size_t(Mod::Count)] = {c.mod_spread, c.mod_bullet_speed, c.mod_damage, c.mod_recoil};
    for (size_t i = 0; i < size_t(Mod::Count); ++i)
        if (cfg_mods[i] != cfg_mods_[i]) {
            cfg_mods_[i] = cfg_mods[i];
            mods_[i].store(cfg_mods[i], std::memory_order_relaxed);
        }
    if (c.mod_instant_bullets != cfg_instant_) {
        cfg_instant_ = c.mod_instant_bullets;
        instant_.store(c.mod_instant_bullets != 0, std::memory_order_relaxed);
    }
    if (c.mod_infinite_ammo != cfg_infinite_ammo_) {
        cfg_infinite_ammo_ = c.mod_infinite_ammo;
        infinite_ammo_.store(c.mod_infinite_ammo != 0, std::memory_order_relaxed);
    }
    if (c.mod_straight_bullets != cfg_straight_) {
        cfg_straight_ = c.mod_straight_bullets;
        straight_.store(c.mod_straight_bullets != 0, std::memory_order_relaxed);
    }
    if (c.mod_teleport_bullets != cfg_teleport_) {
        cfg_teleport_ = c.mod_teleport_bullets;
        teleport_.store(c.mod_teleport_bullets != 0, std::memory_order_relaxed);
    }
    if (c.mod_magic_bullets != cfg_magic_) {
        cfg_magic_ = c.mod_magic_bullets;
        magic_.store(c.mod_magic_bullets != 0, std::memory_order_relaxed);
    }
    world_.SetIntervals(DWORD(c.esp_scan_interval_ms), DWORD(c.esp_list_interval_ms));
    uintptr_t watch[4] = {uintptr_t(c.hwbp_addr), uintptr_t(c.hwbp_addr2), uintptr_t(c.hwbp_addr3),
                          uintptr_t(c.hwbp_addr4)};
    hwbp::Mode modes[4];
    for (hwbp::Mode& m : modes) m = hwbp::Mode(c.hwbp_read);
    // The tracker costs one exception per simulation step of every projectile in the game (all characters'):
    // in mode 2 it is armed only while it teaches something, plus 3 s for the bullets still in flight.
    // straight, teleported and magic bullets need it at every step of the player's bullets, but their paths teach
    // nothing (the learning skips them): their steps are then not copied
    const int path = TeleportBullets() ? 2 : StraightBullets() ? 1 : 0;
    hwbp::SetBulletPath(path);
    const bool rewrites = path || MagicBullets();
    bool track = c.proj_track == 1;
    if (c.proj_track == 2 && !rewrites) {
        const double now = Clock::NowMs();
        if (LearningWanted()) track_until_ms_ = now + 3000.0;
        track = now < track_until_ms_;
    }
    hwbp::SetTrackSamples(track);
    hwbp::SetMagicDebug(c.magic_debug != 0);
    if (rewrites) track = true;
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
    // Weapon mods through instruction breakpoints, in place of research addresses: the spread at each shot (DR2),
    // the camera's recoil every frame (DR3).
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    hwbp::ScaleMod spread;
    spread.reg = hwbp::Reg::Rdi;
    spread.object = spread_comp_.load(std::memory_order_relaxed);
    spread.offs[0] = uint16_t(game::kSpreadValueOff);
    spread.n = 1;
    spread.factor = ModFactor(Mod::Spread);
    const bool spread_mod = spread.factor != 1.0f && spread.object;
    hwbp::ScaleMod recoil;
    recoil.reg = hwbp::Reg::Rax;
    recoil.object = aim_comp_.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) recoil.offs[i] = game::kAimRecoilOffs[i];
    recoil.n = 8;
    recoil.factor = ModFactor(Mod::Recoil);
    const bool recoil_mod = recoil.factor != 1.0f && recoil.object;
    if (spread_mod) {
        watch[2] = image + game::kSpreadApplyRva;
        modes[2] = hwbp::Mode::Execute;
        spread.slot = 2;
        hwbp::SetScaleMod(0, spread);  // before arming
    }
    if (recoil_mod) {
        watch[3] = image + game::kRecoilApplyRva;
        modes[3] = hwbp::Mode::Execute;
        recoil.slot = 3;
        hwbp::SetScaleMod(1, recoil);
    }
    // bullets per shot through the fire handler's pellet loop (DR0, unless a research address holds it): one
    // exception per shot of the weapon in hand, which also times the shots (player_shots, shot_ms)
    hwbp::PelletMod pellets;
    pellets.weapon = weapon_logic_.load(std::memory_order_relaxed);
    pellets.count = c.mod_bullets_per_shot;
    const bool pellet_mod = pellets.count > 1 && pellets.weapon && !watch[0];
    if (pellet_mod) {
        watch[0] = image + game::kFireAfterShotRva;
        modes[0] = hwbp::Mode::Execute;
        pellets.slot = 0;
        pellets.loop = image + game::kFirePelletLoopRva;
        pellets.pellets_off = game::kFireParamsPelletsOff;
        pellets.next_off = game::kFireParamsNextPelletOff;
        hwbp::SetPelletMod(pellets);  // before arming
    }
    pellets_.store(pellet_mod ? pellets.count : 1, std::memory_order_relaxed);
    hwbp::SetTrigger(c.hwbp_trigger, c.hwbp_window_ms);
    hwbp::Configure(watch, modes, c.hwbp_len, c.hwbp_max_hits);
    if (!track) hwbp::SetTracker(-1);
    if (!spread_mod) hwbp::SetScaleMod(0, hwbp::ScaleMod{});  // after disarming
    if (!recoil_mod) hwbp::SetScaleMod(1, hwbp::ScaleMod{});
    if (!pellet_mod) hwbp::SetPelletMod(hwbp::PelletMod{});
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
            // straightened bullets (the mod) do not teach the game's paths
            const bool straight = rt->StraightBullets() || rt->TeleportBullets() || rt->MagicBullets();
            const bool learn = std::fabs(u.z) < 0.1736f && !straight;  // 10 deg
            const bool just_fired = s.elapsed < 0.1f && weapon.speed > 0 && !straight;  // the weapon in hand
            // The speed curve (trajectory.h, SpeedCurve): the speed shown here was computed at the previous step,
            // from that step's angle acos(launch vector . direction), plus the gravity term of its time.
            const Vec3 dir{s.dir[0], s.dir[1], s.dir[2]}, launch{s.dir0[0], s.dir0[1], s.dir0[2]};
            const float angle = std::acos(std::fmax(0.0f, std::fmin(1.0f, launch.Dot(dir)))) * 57.29578f;
            auto& prev = rt->curve_prev_;
            if (prev.size() > 512) prev.clear();
            const auto it = prev.find(s.obj);
            float ratio = -1, ratio_angle = 0;
            if (it != prev.end() && s.elapsed > it->second.elapsed && s.mode == 4 && s.V > 0 && s.v > 20.5f) {
                const CurveStep& last = it->second;
                ratio = (s.v - 4.905f * last.elapsed * last.elapsed * std::fmax(-last.dir_z, 0.0f)) / s.V;
                ratio_angle = last.angle_deg;
            }
            prev[s.obj] = {angle, s.elapsed, dir.z};
            AcquireSRWLockExclusive(&rt->book_lock_);
            if (just_fired) rt->book_.Link(weapon, key);
            if (learn) rt->book_.Add(key, r.Dot(u), -r.Dot(up), s.elapsed);
            if (ratio > 0 && !straight) rt->book_.AddCurve(ratio_angle, ratio);
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
            else if (i == 1) hud_level_ = (hud_level_ + 1) % 3;  // minimal -> details -> hidden
            else if (i == 3) esp_on_ = !esp_on_;
            else InterlockedExchange(&control_->unload_request, 1);
        }
        key_down_[i] = down;
    }
}

}  // namespace grw
