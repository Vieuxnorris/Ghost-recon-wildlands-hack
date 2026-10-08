#include "game/world.h"
#include <cmath>
#include <cstring>
#include "core/clock.h"
#include "core/mem.h"
#include "scan/regions.h"
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw::game {

namespace {

bool ReadCamera(uintptr_t addr, uintptr_t vt, CameraView& cam);

struct ScanResult {
    std::vector<uintptr_t> cameras, chars;
    std::vector<scan::Region> hot;  // regions that held at least one hit
    uint64_t bytes = 0;
    size_t regions = 0, unreadable = 0;
    double ms = 0;
};

// Every 8-aligned qword equal to one of the two vtables in the given regions.
ScanResult ScanVtables(const std::vector<scan::Region>& regions, uintptr_t camera_vt, uintptr_t char_vt,
                       scan::ScanBuffer& buf, const std::atomic<bool>& stop) {
    ScanResult r;
    const double t0 = Clock::NowMs();
    r.regions = regions.size();
    for (const scan::Region& reg : regions) {
        const size_t hits_before = r.chars.size() + r.cameras.size();
        for (size_t off = 0; off < reg.size; off += scan::ScanBuffer::kBlock) {
            if (stop.load(std::memory_order_relaxed)) return r;
            const size_t len = (reg.size - off < scan::ScanBuffer::kBlock) ? reg.size - off : scan::ScanBuffer::kBlock;
            if (!mem::Read(reg.base + off, buf.Data(), len)) {
                ++r.unreadable;
                continue;
            }
            r.bytes += len;
            const auto* q = reinterpret_cast<const uint64_t*>(buf.Data());
            for (size_t i = 0, n = len / 8; i < n; ++i) {
                if (q[i] == char_vt) r.chars.push_back(reg.base + off + i * 8);
                else if (q[i] == camera_vt) r.cameras.push_back(reg.base + off + i * 8);
            }
        }
        if (r.chars.size() + r.cameras.size() > hits_before) r.hot.push_back(reg);
    }
    r.ms = Clock::NowMs() - t0;
    return r;
}

bool ReadCamera(uintptr_t addr, uintptr_t vt, CameraView& cam) {
    uint8_t d[kCameraMatrixOff + 64];
    if (!mem::Read(addr, d, sizeof d)) return false;
    uint64_t v;
    std::memcpy(&v, d, 8);
    if (v != vt) return false;
    GameMatrix m;
    std::memcpy(&m, d + kCameraMatrixOff, sizeof m);
    std::memcpy(&cam.fov_v, d + kCameraFovOff, 4);
    if (!m.Plausible() || !(cam.fov_v > 0.05f && cam.fov_v < 3.0f)) return false;
    cam.right = m.Row(0);
    cam.forward = m.Row(1);
    cam.up = m.Row(2);
    cam.pos = m.Position();
    return true;
}

}  // namespace

void World::Start(Telemetry* tel, DWORD interval_ms) {
    tel_ = tel;
    base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    interval_ms_ = interval_ms;
    stop_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

void World::Stop() {
    stop_ = true;
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    if (wake_) CloseHandle(wake_);
    wake_ = nullptr;
}

void World::Run() {
    scan::ScanBuffer buf;
    const scan::MemRange ex = buf.Range();
    std::vector<scan::Region> hot;
    double last_full = -1e18;
    while (!stop_.load()) {
        // A full pass over all writable memory (~6 GB, seconds) finds the regions that hold the objects;
        // in between, only those regions are rescanned. Full again every 30 s, or every 10 s while the
        // camera is missing.
        const double now = Clock::NowMs();
        const bool full = hot.empty() || now - last_full >= 30'000.0 || (rescan_.load() && now - last_full >= 10'000.0);
        ScanResult r = ScanVtables(full ? scan::ReadableRegions(true, &ex, 1) : hot, base_ + kCameraVtableRva,
                                   base_ + kCharVtableRva, buf, stop_);
        if (stop_.load()) break;
        if (full) last_full = now;
        hot = r.hot;
        const size_t n_chars = r.chars.size();
        // The DLL's own stacks also hold the vtable values it compares against: keep plausible cameras only.
        uintptr_t camera = 0;
        size_t valid_cameras = 0;
        for (uintptr_t c : r.cameras) {
            CameraView v;
            if (ReadCamera(c, base_ + kCameraVtableRva, v)) {
                camera = c;
                ++valid_cameras;
            }
        }
        {
            AcquireSRWLockExclusive(&lock_);
            camera_ = camera;
            chars_ = std::move(r.chars);
            ++generation_;
            ReleaseSRWLockExclusive(&lock_);
        }
        if (tel_) {
            Line l;
            JsonLine(l, "esp_scan", tel_->SinceStartMs(), 0)
                .Str("kind", full ? "full" : "hot")
                .Int("hot_regions", int64_t(hot.size()))
                .Int("cameras", int64_t(r.cameras.size()))
                .Int("valid_cameras", int64_t(valid_cameras))
                .Int("chars", int64_t(n_chars))
                .Num("ms", r.ms)
                .Int("mb", int64_t(r.bytes >> 20))
                .Int("regions", int64_t(r.regions))
                .Int("unreadable_blocks", int64_t(r.unreadable))
                .Finish();
            tel_->Push(Channel::Worker, l);
        }
        mem::ClearThreadCache();  // the scan touched every region; don't keep thousands of entries around
        // At most one scan per second while the camera is missing (menus, loading), else every interval.
        const double scanned = Clock::NowMs();
        rescan_ = false;
        while (!stop_.load()) {
            const double waited = Clock::NowMs() - scanned;
            if (waited >= (rescan_.load() ? 1000.0 : double(interval_ms_.load()))) break;
            WaitForSingleObject(wake_, 100);
        }
    }
}

void World::ReadAim(uintptr_t entity, AimState& aim) const {
    uintptr_t holder = 0, comp = 0;
    if (!mem::Read(entity + kEntityAimHolderOff, holder) || !mem::Read(holder + kAimHolderCompOff, comp)) return;
    uint8_t d[kAimPitchMaxOff + 4];
    if (!mem::Read(comp, d, sizeof d)) return;
    uint64_t vt;
    std::memcpy(&vt, d, 8);
    if (vt != base_ + kAimCompVtableRva) return;
    std::memcpy(&aim.yaw, d + kAimYawOff, 4);
    std::memcpy(&aim.pitch, d + kAimPitchOff, 4);
    std::memcpy(&aim.pitch_min, d + kAimPitchMinOff, 4);
    std::memcpy(&aim.pitch_max, d + kAimPitchMaxOff, 4);
    aim.comp = comp;
    aim.ok = std::isfinite(aim.yaw) && std::isfinite(aim.pitch) && aim.pitch_min < aim.pitch_max;
}

void World::BuildFrame(const EspOptions& opt, EspFrame& out, StageStats& stats) {
    out.items.clear();
    out.camera_ok = false;
    out.active = out.on_screen = out.heads = 0;

    {
        AcquireSRWLockShared(&lock_);
        if (generation_ != seen_generation_) {
            seen_generation_ = generation_;
            r_camera_ = camera_;
            r_chars_ = chars_;
        }
        ReleaseSRWLockShared(&lock_);
    }
    out.known = uint32_t(r_chars_.size());

    const StageSpan cam_span;
    CameraView cam;
    const bool cam_ok = r_camera_ && ReadCamera(r_camera_, base_ + kCameraVtableRva, cam);
    cam_span.End(stats, Stage::Camera);
    if (!cam_ok) {
        rescan_ = true;
        return;
    }
    out.camera_ok = true;
    out.fov_deg = cam.fov_v * 57.29578f;
    out.camera_pos = cam.pos;
    out.camera = cam;
    out.aim = AimState{};
    const float sy = 1.0f / std::tan(cam.fov_v * 0.5f);

    const uintptr_t char_vt = base_ + kCharVtableRva, entity_vt = base_ + kEntityVtableRva;
    bones_.resize(kBonesRead * kBoneFloats);
    const StageSpan ent_span;
    double bones_cpu = 0, bones_wall = 0;
    for (uintptr_t a : r_chars_) {
        uint8_t d[kCharReadSize];
        if (!mem::Read(a, d, sizeof d)) continue;
        uint64_t v;
        std::memcpy(&v, d, 8);
        if (v != char_vt) continue;
        GameMatrix m;
        std::memcpy(&m, d + kCharMatrixOff, sizeof m);
        if (!m.Plausible()) continue;
        const Vec3 feet = m.Position();
        if (feet.Length() < 1.0f) continue;  // inactive pool
        const float dist = (feet - cam.pos).Length();
        if (dist > opt.max_dist) continue;

        uint32_t flags = 0;
        uintptr_t owner;
        std::memcpy(&owner, d + kCharOwnerOff, 8);
        uint8_t od[kEntityFlagsOff + 4];
        bool flags_ok = false;
        if (mem::Read(owner, od, sizeof od)) {
            std::memcpy(&v, od, 8);
            if (v == entity_vt) {
                std::memcpy(&flags, od + kEntityFlagsOff, 4);
                flags_ok = true;
            }
        }
        const Category cat = flags_ok ? Classify(flags) : Category::Unknown;
        if (cat == Category::Player) {
            ReadAim(owner, out.aim);
            continue;
        }
        if (cat == Category::Civilian && !opt.civilians) continue;
        if (cat == Category::Dead && !opt.dead) continue;
        ++out.active;

        Vec2 sf;
        float depth;
        if (!Project(cam, feet, opt.width, opt.height, sf, depth)) continue;
        if (sf.x < -200 || sf.x > opt.width + 200 || sf.y < -200 || sf.y > opt.height + 400) continue;
        ++out.on_screen;

        EspItem it;
        it.addr = a;
        it.dist = dist;
        it.flags = flags;
        it.cat = cat;
        it.vehicle = flags_ok && (flags & kFlagVehicle);
        Vec3 top = feet + Vec3{0, 0, 1.8f};

        const StageSpan bone_span;
        uintptr_t pose;
        std::memcpy(&pose, d + kCharPoseOff, 8);
        uintptr_t bones_ptr;
        if (pose && mem::Read(pose + kPoseBonesOff, bones_ptr) &&
            mem::Read(bones_ptr, bones_.data(), bones_.size() * sizeof(float))) {
            Vec3 head_m, crown_m;
            if (HeadFromBones(bones_.data(), kBonesRead, head_m, crown_m)) {
                Vec2 sh;
                float hd;
                it.head_world = m.ToWorld(head_m);
                if (Project(cam, it.head_world, opt.width, opt.height, sh, hd)) {
                    it.head = sh;
                    it.head_radius = std::fmax(2.0f, 0.11f * sy * opt.height * 0.5f / hd);
                    ++out.heads;
                }
                top = m.ToWorld(crown_m) + Vec3{0, 0, 0.1f};
            }
        }
        const double c1 = Clock::ThreadCpuMs(), w1 = Clock::NowMs();
        bones_cpu += c1 - bone_span.cpu0;
        bones_wall += w1 - bone_span.wall0;

        Vec2 st;
        if (!Project(cam, top, opt.width, opt.height, st, depth)) st = {sf.x, sf.y - 4};
        const float h = std::fmax(std::fabs(sf.y - st.y), 4.0f);
        const float cx = (sf.x + st.x) * 0.5f;
        it.box_min = {cx - h * 0.22f, std::fmin(sf.y, st.y)};
        it.box_max = {cx + h * 0.22f, std::fmax(sf.y, st.y)};
        out.items.push_back(it);
    }
    ent_span.End(stats, Stage::Entities);
    stats.Add(Stage::Bones, float(bones_cpu), float(bones_wall));
}

}  // namespace grw::game
