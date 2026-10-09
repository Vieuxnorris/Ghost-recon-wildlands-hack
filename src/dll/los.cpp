#include "dll/los.h"

#include <windows.h>
#include <malloc.h>
#include <algorithm>
#include <cstdio>
#include "core/clock.h"
#include "dll/raycast.h"
#include "game/world.h"
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw {
namespace {

// A Havok thread's setup, as the game's own threads do it (GRW.exe+3010220):
// router = new(0x80); hkMemoryRouter ctor; hkMemorySystem::getInstance()->threadInit(router, name, 3);
// hkBaseSystem::initThread(&result, router) ... hkBaseSystem::quitThread(&result); ->threadQuit(router, 3).
constexpr uintptr_t kRouterCtorRva = 0x2F5F250;
constexpr uintptr_t kMemorySystemRva = 0x2F83580;
constexpr uintptr_t kInitThreadRva = 0x2F68940;
constexpr uintptr_t kQuitThreadRva = 0x2F68990;
constexpr size_t kRouterSize = 0x80;
constexpr size_t kThreadInitSlot = 0x18 / 8, kThreadQuitSlot = 0x20 / 8;
constexpr int kRouterFlags = 3;  // persistent | temporary

using VoidFn = void(__fastcall*)(void*);
using GetFn = void*(__fastcall*)();
using ThreadInitFn = void(__fastcall*)(void* system, void* router, const char* name, int flags);
using ThreadQuitFn = void(__fastcall*)(void* system, void* router, int flags);
using ResultFn = void(__fastcall*)(int* result, void* router);

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// SEH only (no C++ objects). Returns the exception code, 0 on success.
DWORD HavokThreadInit(void* router) {
    __try {
        const uintptr_t b = Base();
        reinterpret_cast<VoidFn>(b + kRouterCtorRva)(router);
        void* sys = reinterpret_cast<GetFn>(b + kMemorySystemRva)();
        auto** vt = *reinterpret_cast<void***>(sys);
        reinterpret_cast<ThreadInitFn>(vt[kThreadInitSlot])(sys, router, "grwLineOfSight", kRouterFlags);
        int result = 0;
        reinterpret_cast<ResultFn>(b + kInitThreadRva)(&result, router);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

DWORD HavokThreadQuit(void* router) {
    __try {
        const uintptr_t b = Base();
        int result = 0;
        reinterpret_cast<ResultFn>(b + kQuitThreadRva)(&result, nullptr);
        void* sys = reinterpret_cast<GetFn>(b + kMemorySystemRva)();
        auto** vt = *reinterpret_cast<void***>(sys);
        reinterpret_cast<ThreadQuitFn>(vt[kThreadQuitSlot])(sys, router, kRouterFlags);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

void PushStatus(Telemetry* tel, const char* event, DWORD code) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%lx", code);
    Line l;
    JsonLine(l, "status", tel->SinceStartMs(), 0).Str("event", event).Str("detail", buf).Finish();
    tel->Push(Channel::Los, l);
}

}  // namespace

void LineOfSight::Start(Telemetry* tel) {
    tel_ = tel;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void LineOfSight::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void LineOfSight::SetCamera(const Vec3& pos, const Vec3& forward) {
    const float v[6] = {pos.x, pos.y, pos.z, forward.x, forward.y, forward.z};
    for (int i = 0; i < 6; ++i) cam_[i].store(v[i], std::memory_order_relaxed);
}

void LineOfSight::Submit(const Vec3& origin, const Vec3* player_feet, std::vector<LosRequest>& reqs) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    req_origin_ = origin;
    req_player_ok_ = player_feet != nullptr;
    if (player_feet) req_player_feet_ = *player_feet;
    reqs_.swap(reqs);
    ++req_gen_;
}

void LineOfSight::Collect(std::unordered_map<uintptr_t, LosVerdict>& verdicts) {
    std::lock_guard<std::mutex> lk(out_mtx_);
    for (const auto& [key, v] : out_) verdicts[key] = v;
    out_.clear();
}

void LineOfSight::SetParams(const LosParams& p) {
    std::lock_guard<std::mutex> lk(params_mtx_);
    params_ = p;
}

void LineOfSight::Run() {
    void* router = _aligned_malloc(kRouterSize, 16);
    const DWORD init = router ? HavokThreadInit(router) : DWORD(-1);
    PushStatus(tel_, "los_havok_init", init);
    if (init == 0) {
        double next_test_ms = 0;
        while (!stop_.load()) {
            LosParams p;
            {
                std::lock_guard<std::mutex> lk(params_mtx_);
                p = params_;
            }
            const double now = Clock::NowMs();
            if (test_.load(std::memory_order_relaxed) && now >= next_test_ms) {
                next_test_ms = now + 500.0;
                RayTest();
            }
            Pass();
            Sleep(DWORD(p.interval_ms));
        }
        PushStatus(tel_, "los_havok_quit", HavokThreadQuit(router));
    }
    // A router that failed to initialise may still be referenced by Havok: leak it rather than free it.
    if (init == 0) _aligned_free(router);
}

namespace {
float DistToSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float len2 = ab.Dot(ab);
    const float t = len2 > 0.0f ? std::clamp((p - a).Dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).Length();
}
constexpr float kPlayerHeight = 1.9f;
constexpr uint16_t kHitboxMaterial = 8;  // characters' hitbox bodies (layers 5, 15, 21 seen)
}  // namespace

LosVerdict LineOfSight::Check(const Vec3& origin, const Vec3* player_feet, const LosRequest& r, const LosParams& p) {
    LosVerdict v;
    v.t_ms = Clock::NowMs();
    const Vec3 to = r.point;
    const Vec3 start = r.has_from ? r.from : origin;
    if (r.has_from) player_feet = nullptr;  // far from the player: nothing of its own on the way
    const Vec3 d = to - start;
    const float len = d.Length();
    if (len < 0.01f) return v;
    const Vec3 dir = d * (1.0f / len);
    v.target_dist = len;
    Vec3 from = start;
    float travelled = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const double t0 = Clock::NowMs();
        const raycast::Result res = raycast::Cast(from, to, p.mask, p.flag);
        stats_.ray_us += (Clock::NowMs() - t0) * 1000.0;
        ++stats_.rays;
        if (!res.ok) {
            stats_.faults += res.fault ? 1 : 0;
            return v;  // unknown
        }
        if (!res.hit) {
            v.state = game::kLosVisible;
            return v;
        }
        const float seg = (to - from).Length();
        const float hit = travelled + res.fraction * seg;
        if (attempt < 4) {
            v.hits[attempt] = hit;
            std::memcpy(v.raw[attempt], res.raw, sizeof res.raw);
        }
        v.last_hit = res.pos;
        // the player's own body and weapon (camera behind or inside them): pass through
        const bool self = hit < p.self_skip_m ||
                          (player_feet && DistToSegment(res.pos, *player_feet,
                                                        *player_feet + Vec3{0, 0, kPlayerHeight}) <= p.self_capsule_m);
        if (self) {
            ++stats_.self_skips;
            travelled = hit + 0.05f;
            if (travelled >= len) {
                v.state = game::kLosVisible;
                return v;
            }
            from = start + dir * travelled;
            continue;
        }
        // the target's own hitboxes (material 8: shoulder, arm, gear in front of the head) are the target
        const bool own_hitbox = res.material == kHitboxMaterial &&
                                DistToSegment(res.pos, r.feet, r.point + Vec3{0, 0, 0.25f}) <= p.hitbox_m;
        if (own_hitbox || len - hit <= (r.vehicle ? p.vehicle_tol_m : p.head_tol_m)) {
            v.state = game::kLosVisible;
        } else {
            v.state = game::kLosHidden;
            v.hit_dist = hit;
        }
        return v;
    }
    return v;
}

void LineOfSight::Pass() {
    Vec3 origin, player_feet;
    bool player_ok;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        if (reqs_.empty()) return;
        origin = req_origin_;
        player_ok = req_player_ok_;
        player_feet = req_player_feet_;
        work_.assign(reqs_.begin(), reqs_.end());
    }
    LosParams p;
    {
        std::lock_guard<std::mutex> lk(params_mtx_);
        p = params_;
    }
    const int max_rays = p.max_rays;
    // enemies first, then closest to the crosshair
    std::sort(work_.begin(), work_.end(), [](const LosRequest& a, const LosRequest& b) {
        if (a.enemy != b.enemy) return a.enemy;
        return a.angle < b.angle;
    });
    if (work_.size() > size_t(max_rays)) work_.resize(size_t(max_rays));
    const double t0 = Clock::NowMs();
    std::vector<std::pair<uintptr_t, LosVerdict>> done;
    done.reserve(work_.size());
    for (const LosRequest& r : work_) {
        const LosVerdict v = Check(origin, player_ok ? &player_feet : nullptr, r, p);
        if (v.state == game::kLosVisible) ++stats_.visible;
        if (v.state == game::kLosHidden) ++stats_.hidden;
        done.emplace_back(r.key, v);
    }
    const double ms = Clock::NowMs() - t0;
    ++stats_.passes;
    stats_.pass_ms += ms;
    stats_.max_pass_ms = std::max(stats_.max_pass_ms, ms);
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        out_.insert(out_.end(), done.begin(), done.end());
    }
    const double now = Clock::NowMs();
    if (now >= next_stats_ms_) {
        next_stats_ms_ = now + 1000.0;
        Line l;
        JsonLine(l, "los", tel_->SinceStartMs(), 0)
            .Int("passes", int64_t(stats_.passes))
            .Int("rays", int64_t(stats_.rays))
            .Num("us_per_ray", stats_.rays ? stats_.ray_us / double(stats_.rays) : 0.0)
            .Num("pass_ms", stats_.passes ? stats_.pass_ms / double(stats_.passes) : 0.0)
            .Num("max_pass_ms", stats_.max_pass_ms)
            .Int("visible", int64_t(stats_.visible))
            .Int("hidden", int64_t(stats_.hidden))
            .Int("self_skips", int64_t(stats_.self_skips))
            .Int("faults", int64_t(stats_.faults))
            .Finish();
        tel_->Push(Channel::Los, l);
        stats_ = {};
        // this pass, target by target (at most 8 lines a second)
        for (size_t i = 0; p.debug && i < done.size() && i < 8; ++i) {
            const LosVerdict& v = done[i].second;
            char key[24];
            std::snprintf(key, sizeof key, "%llx", static_cast<unsigned long long>(done[i].first));
            Line d;
            JsonLine j(d, "los_detail", tel_->SinceStartMs(), 0);
            j.Str("key", key).Int("state", v.state).Bool("enemy", work_[i].enemy).Num("dist", v.target_dist);
            for (int h = 0; h < 4 && v.hits[h] >= 0; ++h) {
                const char* names[4] = {"h0", "h1", "h2", "h3"};
                j.Num(names[h], v.hits[h]);
                char hex[0x50 * 2 + 1];
                for (int b = 0; b < 0x50; ++b) std::snprintf(hex + 2 * b, 3, "%02x", v.raw[h][b]);
                Line hl;
                JsonLine(hl, "los_hit", tel_->SinceStartMs(), 0)
                    .Str("key", key)
                    .Int("i", h)
                    .Num("dist", v.hits[h])
                    .Str("raw", hex)
                    .Finish();
                tel_->Push(Channel::Los, hl);
            }
            j.Num("hx", v.last_hit.x).Num("hy", v.last_hit.y).Num("hz", v.last_hit.z);
            j.Num("tx", work_[i].point.x).Num("ty", work_[i].point.y).Num("tz", work_[i].point.z);
            j.Num("ox", origin.x).Num("oy", origin.y).Num("oz", origin.z);
            j.Finish();
            tel_->Push(Channel::Los, d);
        }
    }
}

void LineOfSight::RayTest() {
    const Vec3 from{cam_[0].load(), cam_[1].load(), cam_[2].load()};
    const Vec3 fwd{cam_[3].load(), cam_[4].load(), cam_[5].load()};
    if (fwd.Length() < 0.5f) return;  // no camera yet
    const Vec3 to = from + fwd * 1000.0f;
    const double t0 = Clock::NowMs();
    const raycast::Result r = raycast::Cast(from, to);
    const double us = (Clock::NowMs() - t0) * 1000.0;
    Line l;
    JsonLine j(l, "ray_test", tel_->SinceStartMs(), 0);
    j.Bool("ok", r.ok).Bool("fault", r.fault).Bool("hit", r.hit).Num("us", us).Int("stage", r.stage);
    if (r.fault) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%x", r.fault_code);
        j.Str("code", buf);
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(r.fault_rva));
        j.Str("at", buf);
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(r.fault_data));
        j.Str("addr", buf);
    }
    j.Num("fx", from.x).Num("fy", from.y).Num("fz", from.z).Num("dx", fwd.x).Num("dy", fwd.y).Num("dz", fwd.z);
    if (r.hit) {
        j.Num("frac", r.fraction).Num("dist", r.fraction * 1000.0f).Num("hx", r.pos.x).Num("hy", r.pos.y).Num("hz", r.pos.z);
        j.Num("nx", r.normal.x).Num("ny", r.normal.y).Num("nz", r.normal.z).Int("body", r.body);
    }
    j.Finish();
    tel_->Push(Channel::Los, l);
}

}  // namespace grw
