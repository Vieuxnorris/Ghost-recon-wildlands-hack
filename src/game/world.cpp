#include "game/world.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include "core/clock.h"
#include "core/mem.h"
#include "scan/regions.h"
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw::game {

namespace {

bool ReadCamera(uintptr_t addr, uintptr_t vt, CameraView& cam);

struct ScanResult {
    std::vector<uintptr_t> cameras, chars, weapon_states;
    std::vector<scan::Region> hot;  // regions that held at least one hit
    uint64_t bytes = 0;
    size_t regions = 0, unreadable = 0;
    double ms = 0;
};

// Every 8-aligned qword equal to one of the two vtables in the given regions.
ScanResult ScanVtables(const std::vector<scan::Region>& regions, uintptr_t camera_vt, uintptr_t char_vt,
                       uintptr_t weapon_state_vt,
                       scan::ScanBuffer& buf, const std::atomic<bool>& stop) {
    ScanResult r;
    const double t0 = Clock::NowMs();
    r.regions = regions.size();
    for (const scan::Region& reg : regions) {
        const size_t hits_before = r.chars.size() + r.cameras.size() + r.weapon_states.size();
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
                else if (q[i] == weapon_state_vt) r.weapon_states.push_back(reg.base + off + i * 8);
            }
        }
        if (r.chars.size() + r.cameras.size() + r.weapon_states.size() > hits_before) r.hot.push_back(reg);
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

void World::Start(Telemetry* tel, DWORD scan_interval_ms, DWORD list_interval_ms) {
    tel_ = tel;
    base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    interval_ms_ = scan_interval_ms;
    list_interval_ms_ = list_interval_ms;
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

// The fallback's state: its 1 MB block buffer and what the last scan found.
struct World::ScanState {
    scan::ScanBuffer buf;
    std::vector<scan::Region> hot;
    double last_full = -1e18, last = -1e18;
    uintptr_t camera = 0, weapon_state = 0;
    std::vector<uintptr_t> chars;
};

// A full pass over all writable memory (~6.7 GB, ~3 s) finds the regions that hold the objects; in between,
// only those regions are rescanned. Full again every 30 s, or every 10 s while the camera is missing.
void World::Scan(ScanState& s, bool camera_missing) {
    const scan::MemRange ex = s.buf.Range();
    const double now = Clock::NowMs();
    const bool full = s.hot.empty() || now - s.last_full >= 30'000.0 || (camera_missing && now - s.last_full >= 10'000.0);
    ScanResult r = ScanVtables(full ? scan::ReadableRegions(true, &ex, 1) : s.hot, base_ + kCameraVtableRva,
                               base_ + kCharVtableRva, base_ + kWeaponStateVtableRva, s.buf, stop_);
    if (stop_.load()) return;
    s.last = Clock::NowMs();
    if (full) s.last_full = now;
    s.hot = r.hot;
    // The DLL's own stacks also hold the vtable values it compares against: keep plausible cameras only.
    s.camera = 0;
    size_t valid_cameras = 0;
    for (uintptr_t c : r.cameras) {
        CameraView v;
        if (ReadCamera(c, base_ + kCameraVtableRva, v)) {
            s.camera = c;
            ++valid_cameras;
        }
    }
    // ... and the weapon state's: keep the candidate whose weapon handle resolves
    s.weapon_state = 0;
    for (uintptr_t w : r.weapon_states) {
        uintptr_t h = 0, obj = 0;
        int32_t cnt = 0;
        uint64_t vt = 0;
        if (mem::Read(w + kWeaponStateWeaponOff, h) && mem::Read(h, obj) && mem::Read(h + 0xC, cnt) && cnt < 0 &&
            mem::Read(obj, vt) && vt == base_ + kWeaponVtableRva)
            s.weapon_state = w;
    }
    s.chars = std::move(r.chars);
    if (tel_) {
        Line l;
        JsonLine(l, "esp_scan", tel_->SinceStartMs(), 0)
            .Str("kind", full ? "full" : "hot")
            .Int("hot_regions", int64_t(s.hot.size()))
            .Int("cameras", int64_t(r.cameras.size()))
            .Int("valid_cameras", int64_t(valid_cameras))
            .Int("chars", int64_t(s.chars.size()))
            .Num("ms", r.ms)
            .Int("mb", int64_t(r.bytes >> 20))
            .Int("regions", int64_t(r.regions))
            .Int("unreadable_blocks", int64_t(r.unreadable))
            .Finish();
        tel_->Push(Channel::Worker, l);
    }
    mem::ClearThreadCache();  // the scan touched every region; don't keep thousands of entries around
}

void World::Run() {
    std::unique_ptr<ScanState> scan;
    std::vector<uintptr_t> list_chars;
    const std::vector<uintptr_t> none;
    double list_bad_since = -1, weapon_bad_since = -1, camera_bad_since = -1;
    // telemetry window
    double window_ms = Clock::NowMs();
    uint32_t passes = 0, passes_ok = 0, searched = 0, scans = 0;
    double cpu_sum = 0, cpu_max = 0;
    RegistryPass last;
    bool was_ok = true;
    while (!stop_.load()) {
        const double now = Clock::NowMs();
        const double c0 = Clock::ThreadCpuMs();
        const RegistryPass reg = registry_.Read(base_, &scan::ProcessReader, nullptr, list_chars);
        const double cpu = Clock::ThreadCpuMs() - c0;
        ++passes;
        passes_ok += reg.ok;
        searched += reg.searched;
        cpu_sum += cpu;
        if (cpu > cpu_max) cpu_max = cpu;
        last = reg;

        // A chain failing for 2 s (not one bad read while the game edits its list) falls back to scanning.
        const auto bad_for = [now](bool bad, double& since) {
            if (!bad) since = -1;
            else if (since < 0) since = now;
            return bad ? now - since : 0.0;
        };
        const bool camera_missing = camera_missing_.load(std::memory_order_relaxed);
        const bool list_bad = bad_for(!reg.ok, list_bad_since) > 2000.0;
        const bool weapon_bad = bad_for(!reg.weapon_state, weapon_bad_since) > 2000.0;
        const bool camera_bad = bad_for(camera_missing, camera_bad_since) > 2000.0;
        if (list_bad || weapon_bad || camera_bad) {
            if (!scan) scan = std::make_unique<ScanState>();
            if (now - scan->last >= double(interval_ms_.load())) {
                Scan(*scan, camera_missing);
                ++scans;
            }
        } else if (scan && now - scan->last > 60'000.0) {
            scan.reset();  // a minute without fallback: free its buffer
        }
        if (stop_.load()) break;

        const std::vector<uintptr_t>& chars = reg.ok ? list_chars : scan ? scan->chars : none;
        const uintptr_t weapon = reg.weapon_state ? reg.weapon_state : scan ? scan->weapon_state : 0;
        const uintptr_t camera = scan ? scan->camera : 0;
        AcquireSRWLockExclusive(&lock_);
        if (chars != chars_ || weapon != weapon_state_ || camera != camera_ || reg.ok != from_list_) {
            chars_ = chars;
            weapon_state_ = weapon;
            camera_ = camera;
            from_list_ = reg.ok;
            ++generation_;
        }
        ReleaseSRWLockExclusive(&lock_);

        if (tel_ && (Clock::NowMs() - window_ms >= 10'000.0 || reg.ok != was_ok)) {
            Line l;
            JsonLine(l, "esp_list", tel_->SinceStartMs(), 0)
                .Bool("ok", reg.ok)
                .Int("slots", last.slots)
                .Int("entities", last.entities)
                .Int("source", last.source)
                .Int("chars", int64_t(list_chars.size()))
                .Bool("weapon", last.weapon_state != 0)
                .Bool("camera_missing", camera_missing)
                .Int("passes", passes)
                .Int("passes_ok", passes_ok)
                .Int("searched", searched)
                .Int("scans", scans)
                .Num("cpu_avg_ms", passes ? cpu_sum / passes : 0.0)
                .Num("cpu_max_ms", cpu_max)
                .Finish();
            tel_->Push(Channel::Worker, l);
            window_ms = Clock::NowMs();
            passes = passes_ok = searched = scans = 0;
            cpu_sum = cpu_max = 0;
        }
        was_ok = reg.ok;
        WaitForSingleObject(wake_, list_interval_ms_.load());
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

namespace {
// Weak handle {object, ..., i32 count at +0xC}: alive when the count is negative.
uintptr_t Resolve(uintptr_t handle) {
    uintptr_t obj = 0;
    int32_t cnt = 0;
    if (!mem::Read(handle, obj) || !mem::Read(handle + 0xC, cnt) || cnt >= 0) return 0;
    return obj;
}
}  // namespace

void World::ReadWeapon(EspFrame& out) const {
    out.weapon_speed = out.weapon_damage = 0;
    out.weapon_id = 0;
    out.bullet_ok = out.spread_ok = false;
    out.weapon = WeaponView{};
    out.weapon_state = r_weapon_state_;
    if (!r_weapon_state_) return;
    uintptr_t h = 0, w = 0, dh = 0, d = 0;
    uint64_t vt = 0;
    if (!mem::Read(r_weapon_state_ + kWeaponStateWeaponOff, h) || !(w = Resolve(h))) return;
    if (!mem::Read(w, vt) || vt != base_ + kWeaponVtableRva) return;
    out.weapon.logic = w;
    ReadBullets(w, out);
    if (!mem::Read(w + kWeaponDataOff, dh) || !(d = Resolve(dh))) return;
    if (!mem::Read(d, vt) || vt != base_ + kWeaponDataVtableRva) return;
    ReadWeaponView(w, d, out);
    out.weapon_id = uint64_t(w) * 31 + d;
    float speed = 0, damage = 0;
    if (mem::Read(d + kWeaponDataSpeedOff, speed) && std::isfinite(speed) && speed > 10 && speed < 3000)
        out.weapon_speed = speed;
    if (mem::Read(d + kWeaponDataDamageOff, damage) && std::isfinite(damage)) out.weapon_damage = damage;
}

namespace {
// A game string {u32 length, u32 capacity, u32, UTF-8} into dst (cut, always terminated).
bool ReadGameString(uintptr_t s, char* dst, size_t cap) {
    uint32_t len = 0;
    dst[0] = 0;
    if (!s || !mem::Read(s, len) || len == 0 || len > 256) return false;
    const size_t n = std::min<size_t>(len, cap - 1);
    if (!mem::Read(s + kGameStringTextOff, dst, n)) return false;
    dst[n] = 0;
    return true;
}
}  // namespace

// Weapon w with design data d: its parts' names (every 500 ms), the menu's stats, damage, magazine and the
// shooter's live spread (model.h).
void World::ReadWeaponView(uintptr_t w, uintptr_t d, EspFrame& out) const {
    WeaponView& v = out.weapon;
    // names: rarely change, and 16 strings
    const double now = Clock::NowMs();
    if (w != names_weapon_ || now - names_ms_ > 500.0) {
        names_weapon_ = w;
        names_ms_ = now;
        names_ = WeaponView{};
        uintptr_t h = 0, t = 0;
        uint64_t vt = 0;
        if (mem::Read(w + kWeaponPartNamesOff, h) && (t = Resolve(h))) {
            while (mem::Read(t - kPartRowStride, vt) && vt == base_ + kPartRowVtableRva) t -= kPartRowStride;
            for (uint32_t i = 0; i < kMaxParts && mem::Read(t, vt) && vt == base_ + kPartRowVtableRva;
                 ++i, t += kPartRowStride) {
                uintptr_t label = 0, name = 0;
                mem::Read(t + kPartRowLabelOff, label);
                mem::Read(t + kPartRowNameOff, name);
                ReadGameString(label, names_.slot[i], sizeof names_.slot[i]);
                ReadGameString(name, names_.part[i], sizeof names_.part[i]);
                names_.parts = i + 1;
            }
            names_.parts_ok = names_.parts > 0;
        }
    }
    v.parts_ok = names_.parts_ok;
    v.parts = names_.parts;
    std::memcpy(v.slot, names_.slot, sizeof v.slot);
    std::memcpy(v.part, names_.part, sizeof v.part);
    // stats: D's base, the parts' summed {add, mul}
    uintptr_t mods = 0;
    float pairs[2 * kStatCount];
    if (mem::Read(d + kDataStatsOff, v.base, sizeof v.base) && mem::Read(d + kDataModsOff, mods) && mods &&
        mem::Read(mods + kModsStatPairsOff, pairs, sizeof pairs)) {
        v.stats_ok = true;
        for (size_t i = 0; i < kStatCount; ++i) {
            v.add[i] = pairs[2 * i];
            v.mul[i] = pairs[2 * i + 1];
            if (!std::isfinite(v.base[i]) || !std::isfinite(v.add[i]) || !std::isfinite(v.mul[i])) v.stats_ok = false;
        }
    }
    uintptr_t mh = 0, mag = 0;
    int32_t rounds = 0;
    if (mem::Read(d + kDataMagazineOff, mh) && (mag = Resolve(mh)) && mem::Read(mag + kMagazineRoundsOff, rounds) &&
        rounds > 0 && rounds < 1000)
        v.magazine = rounds;
    // the spread the next shot will use: attribute 0x48, republished by the game at every update
    uintptr_t root = 0, container = 0, entries = 0, attr = 0;
    uint64_t vt = 0;
    float spread = 0;
    if (mem::Read(base_ + kAttrRootRva, root) && root && mem::Read(root + kAttrContainerOff, container) && container &&
        mem::Read(container, entries) && entries &&
        mem::Read(entries + uintptr_t(kAttrSpreadId) * kAttrEntryStride, attr) && attr && mem::Read(attr, vt) &&
        vt == base_ + kAttrValueVtableRva && mem::Read(attr + kAttrValueOff, spread) && std::isfinite(spread) &&
        spread >= 0 && spread < 10)
        v.spread_live = spread;
}

// The bullets of weapon w: its ballistics block and its shooter's spread (model.h).
void World::ReadBullets(uintptr_t w, EspFrame& out) const {
    uintptr_t m = 0, sh = 0, s = 0, a = 0;
    uint64_t vt = 0;
    float v[2];
    if (mem::Read(w + kWeaponBallisticsOff, m) && m && mem::Read(m, vt) && vt == base_ + kBallisticsVtableRva &&
        mem::Read(m + kBallisticsSpeedOff, v[0]) && mem::Read(m + kBallisticsFirstSegmentOff, v[1]) &&
        std::isfinite(v[0]) && v[0] > 10 && v[0] < kMaxBulletSpeed && std::isfinite(v[1]) && v[1] >= 0 && v[1] < kMaxFirstSegment) {
        out.bullet_ok = true;
        out.bullet_V = v[0];
        out.bullet_V_block = v[0];
        out.bullet_first_segment = v[1];
        out.weapon.ballistics = m;
        float dmg = 0;
        if (mem::Read(m + kBallisticsDamageOff, dmg) && std::isfinite(dmg) && dmg >= 0) out.weapon.damage = dmg;
    }
    if (mem::Read(w + kWeaponShooterOff, sh) && (s = Resolve(sh)) && mem::Read(s, vt) &&
        vt == base_ + kShooterVtableRva && (out.weapon.shooter = s) && mem::Read(s + kShooterSpreadOff, a) && a &&
        mem::Read(a, vt) &&
        vt == base_ + kSpreadVtableRva && mem::Read(a + kSpreadValueOff, v[0]) &&
        mem::Read(a + kSpreadRangeOff, v[1]) && std::isfinite(v[0]) && std::isfinite(v[1])) {
        out.spread_ok = true;
        out.spread = v[0];
        out.spread_range = v[1];
        out.weapon.spread_comp = a;
        // the request: what the bullets will really use
        uintptr_t rq = 0;
        float rv[3];
        if (mem::Read(s + kShooterRequestOff, rq) && rq && mem::Read(rq, vt) && vt == base_ + kRequestVtableRva &&
            mem::Read(rq + kRequestSpeedOff, rv, sizeof rv) && std::isfinite(rv[0]) && rv[0] > 10 && rv[0] < kMaxBulletSpeed &&
            std::isfinite(rv[2]) && rv[2] >= 0 && rv[2] < kMaxFirstSegment) {
            out.weapon.request = rq;
            out.bullet_ok = true;
            out.bullet_V = rv[0];
            out.bullet_first_segment = rv[2];
        }
    }
    out.bullet_V_table = out.bullet_V;
}

void World::BuildFrame(const EspOptions& opt, EspFrame& out, StageStats& stats) {
    out.items.clear();
    out.offscreen.clear();
    out.camera_ok = false;
    out.player_ok = false;
    out.player_entity = 0;
    out.player_flags = 0;
    out.player_head_ok = false;
    out.active = out.on_screen = out.heads = 0;

    {
        AcquireSRWLockShared(&lock_);
        if (generation_ != seen_generation_) {
            seen_generation_ = generation_;
            r_camera_ = camera_;
            r_weapon_state_ = weapon_state_;
            r_from_list_ = from_list_;
            r_chars_ = chars_;
        }
        ReleaseSRWLockShared(&lock_);
    }
    out.known = uint32_t(r_chars_.size());
    out.from_list = r_from_list_;

    const StageSpan cam_span;
    CameraView cam;
    uintptr_t cam_addr = 0;
    bool cam_ok = mem::Read(base_ + kCameraPtrRva, cam_addr) && cam_addr &&
                  ReadCamera(cam_addr, base_ + kCameraVtableRva, cam);
    if (!cam_ok) cam_ok = r_camera_ && ReadCamera(r_camera_, base_ + kCameraVtableRva, cam);
    cam_span.End(stats, Stage::Camera);
    camera_missing_.store(!cam_ok, std::memory_order_relaxed);
    if (!cam_ok) return;
    out.camera_ok = true;
    out.fov_deg = cam.fov_v * 57.29578f;
    out.camera_pos = cam.pos;
    out.camera = cam;
    out.aim = AimState{};
    ReadWeapon(out);
    const float sy = 1.0f / std::tan(cam.fov_v * 0.5f);
    const float cos_head_cone = std::cos(opt.head_cone_deg * 0.017453293f);

    const uintptr_t char_vt = base_ + kCharVtableRva, entity_vt = base_ + kEntityVtableRva;
    bones_.resize(kBonesRead * kBoneFloats);
    const StageSpan ent_span;
    double bones_cpu = 0, bones_wall = 0;
    for (uintptr_t a : r_chars_) {
        // only the fields used: 3-4 cache lines of the object instead of 10
        uint64_t v = 0;
        uintptr_t owner = 0, pose = 0;
        GameMatrix m;
        const mem::Piece cp[] = {{0, &v, 8}, {kCharOwnerOff, &owner, 8}, {kCharMatrixOff, &m, sizeof m},
                                 {kCharPoseOff, &pose, 8}};
        if (!mem::ReadPieces(a, kCharReadSize, cp, 4) || v != char_vt) continue;
        const Vec3 feet = m.Position();
        if (!(feet.Length() >= 1.0f)) continue;  // inactive pool
        const float dist = (feet - cam.pos).Length();
        // the player is never out of range: right after a teleport the camera is there and this matrix not yet
        if ((dist > opt.max_dist && owner != r_player_entity_) || !m.Plausible()) continue;

        uint32_t flags = 0;
        const mem::Piece op[] = {{0, &v, 8}, {kEntityFlagsOff, &flags, 4}};
        const bool flags_ok = mem::ReadPieces(owner, kEntityFlagsOff + 4, op, 2) && v == entity_vt;
        if (!flags_ok) flags = 0;
        const Category cat = flags_ok ? Classify(flags) : Category::Unknown;
        if (cat == Category::Player) {
            ReadAim(owner, out.aim);
            out.player_ok = true;
            out.player_feet = feet;
            out.player_entity = r_player_entity_ = owner;
            out.player_flags = flags;
            // the player's head: where the bullets start in third person (camera-findings.md section 9)
            uintptr_t bones_ptr;
            Vec3 head_m, crown_m;
            if (pose && mem::Read(pose + kPoseBonesOff, bones_ptr) &&
                mem::Read(bones_ptr, bones_.data(), bones_.size() * sizeof(float)) &&
                HeadFromBones(bones_.data(), kBonesRead, head_m, crown_m)) {
                out.player_head_ok = true;
                out.player_head = m.ToWorld(head_m);
            }
            continue;
        }
        if (cat == Category::Civilian && !opt.civilians) continue;
        if (cat == Category::Dead && !opt.dead) continue;
        ++out.active;

        const bool enemy = cat == Category::SantaBlanca || cat == Category::Unidad;
        Vec2 sf;
        float depth;
        if (!Project(cam, feet, opt.width, opt.height, sf, depth) ||
            sf.x < -200 || sf.x > opt.width + 200 || sf.y < -200 || sf.y > opt.height + 400) {
            EspFrame::OffscreenChar o{a, feet, cat, flags_ok && (flags & kFlagVehicle) != 0, false, {}};
            uintptr_t bones_ptr;
            Vec3 head_m, crown_m;
            if (opt.enemy_heads && enemy && pose && mem::Read(pose + kPoseBonesOff, bones_ptr) &&
                mem::Read(bones_ptr, bones_.data(), bones_.size() * sizeof(float)) &&
                HeadFromBones(bones_.data(), kBonesRead, head_m, crown_m)) {
                o.head_ok = true;
                o.head = m.ToWorld(head_m);
            }
            out.offscreen.push_back(o);
            continue;
        }
        ++out.on_screen;

        EspItem it;
        it.addr = a;
        it.dist = dist;
        it.feet = feet;
        it.flags = flags;
        it.cat = cat;
        it.vehicle = flags_ok && (flags & kFlagVehicle);
        Vec3 top = feet + Vec3{0, 0, 1.8f};

        // Bones (4 KB per character) only where a head is worth drawing or aiming at: near, or close to the
        // crosshair (the aim's cone plus a margin). Far ones elsewhere get a box from the feet.
        const Vec3 to_chest = feet + Vec3{0, 0, 1.4f} - cam.pos;
        const bool want_head = dist <= opt.heads_within_m || (opt.enemy_heads && enemy) ||
                               to_chest.Dot(cam.forward) >= cos_head_cone * to_chest.Length();
        const StageSpan bone_span;
        uintptr_t bones_ptr;
        if (want_head && pose && mem::Read(pose + kPoseBonesOff, bones_ptr) &&
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
