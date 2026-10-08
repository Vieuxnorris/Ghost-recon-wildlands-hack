#include "dll/bullet_probe.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "core/clock.h"
#include "core/mem.h"
#include "scan/ray_filter.h"
#include "scan/regions.h"
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw::bullet {

namespace {

constexpr size_t kMaxCandidates = 200'000;  // per thread
constexpr int kMaxReported = 24, kMaxDumped = 8, kMaxTracked = 4;
constexpr double kTrackMs = 8000, kStillMs = 400;
// The bullet record (live/72): the current position P, the shot origin at P - 0x40, the elapsed time at
// P - 0x70, the distance flown at P - 0x2C, the speed at P + 0x28. Snapshot [P - 0x80, P + 0x40) per step.
constexpr ptrdiff_t kRecOriginOff = -0x40, kRecFrom = -0x80, kRecSpeedOff = 0x28;
constexpr double kWatchMs = 600;
constexpr size_t kRecBytes = 0xC0;

struct Candidate {
    uintptr_t addr;
    float along;
};

struct Moving {
    uintptr_t addr;
    scan::RayFilter::Where a, b;
    float speed;
};

bool ReadPos(uintptr_t addr, Vec3& p) {
    float v[3];
    if (!mem::Read(addr, v, sizeof v)) return false;
    p = {v[0], v[1], v[2]};
    return true;
}

void Push(Telemetry& tel, const Line& l) { tel.Push(Channel::Probe, l); }

void Hex(const uint8_t* m, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) std::snprintf(out + i * 2, 3, "%02x", m[i]);
}

void Dump(Telemetry& tel, uintptr_t addr, double t) {
    uint8_t m[0x100];
    if (!mem::Read(addr - 0x80, m, sizeof m)) return;
    char hex[0x40 * 2 + 1], at[24];
    std::snprintf(at, sizeof at, "%llx", static_cast<unsigned long long>(addr));
    for (int off = 0; off < 0x100; off += 0x40) {
        Hex(m + off, 0x40, hex);
        Line l;
        JsonLine(l, "bullet_mem", t, 0).Str("addr", at).Int("off", off - 0x80).Str("hex", hex).Finish();
        Push(tel, l);
    }
}

// The position at addr belongs to the bullet record of this shot: its origin field is the shot origin.
bool IsRecord(uintptr_t addr, const Vec3& origin) {
    float o[3];
    return mem::Read(addr + kRecOriginOff, o, sizeof o) && o[0] == origin.x && o[1] == origin.y && o[2] == origin.z;
}

// Every triplet of the regions near the line of fire whose origin field matches: the record, at once.
uintptr_t FindRecordIn(const std::vector<scan::Region>& regions, const scan::RayFilter& f, scan::ScanBuffer& buf) {
    for (const scan::Region& r : regions)
        for (size_t off = 0; off < r.size; off += scan::ScanBuffer::kBlock) {
            const size_t len = std::min(r.size - off, scan::ScanBuffer::kBlock);
            if (!mem::Read(r.base + off, buf.Data(), len)) continue;
            const auto* p = reinterpret_cast<const float*>(buf.Data());
            scan::RayFilter::Where w;
            for (size_t i = 0; i + 3 <= len / 4; ++i)
                if (f.Test(p + i, w) && IsRecord(r.base + off + i * 4, f.origin)) return r.base + off + i * 4;
        }
    return 0;
}

}  // namespace

void Probe::Shot(const Vec3& origin, const Vec3& dir, const WeaponInfo& weapon, Telemetry& tel) {
    if (busy_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    thread_ = std::thread([this, origin, dir, weapon, t = Clock::NowMs(), tel = &tel] {
        Run(origin, dir, weapon, t, tel);
    });
}

void Probe::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    busy_ = false;
}

void Probe::Remember(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return;
    const scan::Region r{reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize};
    for (const scan::Region& h : hot_)
        if (h.base == r.base) return;
    hot_.push_back(r);
}

void Probe::Run(Vec3 origin, Vec3 dir, WeaponInfo weapon, double t_seen, Telemetry* tel_ptr) {
    Telemetry& tel = *tel_ptr;
    const auto f = scan::RayFilter::Make(origin, dir, 1.0f, 3000.0f, 4.0f, -150.0f, 5.0f);

    // Fast pass: the regions where bullet records were found before (milliseconds).
    uintptr_t record = 0;
    double fast_ms = 0;
    if (!hot_.empty()) {
        scan::ScanBuffer buf;
        record = FindRecordIn(hot_, f, buf);
        fast_ms = Clock::NowMs() - t_seen;
    }

    std::vector<Moving> moving;
    size_t candidates = 0;
    bool truncated = false, full = false;
    double scan_ms = 0, mb = 0, tb = Clock::NowMs();
    const int threads = std::clamp(int(std::thread::hardware_concurrency()) / 2, 2, 8);
    if (!record) {
        full = true;
        std::vector<std::unique_ptr<scan::ScanBuffer>> bufs;
        std::vector<scan::MemRange> ex;
        for (int i = 0; i < threads; ++i) {
            bufs.push_back(std::make_unique<scan::ScanBuffer>());
            ex.push_back(bufs.back()->Range());
        }
        std::vector<std::pair<uintptr_t, size_t>> blocks;
        for (const scan::Region& r : scan::ReadableRegions(true, ex.data(), ex.size()))
            for (size_t off = 0; off < r.size; off += scan::ScanBuffer::kBlock)
                blocks.emplace_back(r.base + off, std::min(r.size - off, scan::ScanBuffer::kBlock));

        // Workers take runs of consecutive blocks so that each mostly stays in the same regions (one
        // VirtualQuery per region and thread, gotcha 1 of the lessons).
        constexpr size_t kRun = 16;
        std::atomic<size_t> next{0};
        std::atomic<uint64_t> bytes{0};
        std::vector<std::vector<Candidate>> found(threads);
        std::vector<std::thread> pool;
        for (int ti = 0; ti < threads; ++ti) {
            pool.emplace_back([&, ti] {
                uint8_t* buf = bufs[ti]->Data();
                auto& out = found[ti];
                for (size_t k0; (k0 = next.fetch_add(kRun)) < blocks.size() && !stop_.load(std::memory_order_relaxed);) {
                    for (size_t k = k0; k < std::min(k0 + kRun, blocks.size()); ++k) {
                        const auto [at, len] = blocks[k];
                        // + 8 bytes for a triplet that starts at the end of the block (not at a region's end)
                        size_t copy = len + 8;
                        if (!mem::Read(at, buf, copy) && !mem::Read(at, buf, copy = len)) continue;
                        bytes.fetch_add(len, std::memory_order_relaxed);
                        const auto* p = reinterpret_cast<const float*>(buf);
                        const size_t n = len / 4, last = copy / 4;
                        scan::RayFilter::Where w;
                        for (size_t i = 0; i < n && i + 3 <= last; ++i) {
                            if (!f.Test(p + i, w)) continue;
                            if (out.size() < kMaxCandidates) out.push_back({at + i * 4, w.along});
                        }
                    }
                }
            });
        }
        for (auto& t : pool) t.join();
        bufs.clear();
        scan_ms = Clock::NowMs() - t_seen;
        mb = double(bytes.load()) / (1 << 20);

        std::vector<Candidate> all;
        for (auto& v : found) {
            truncated = truncated || v.size() >= kMaxCandidates;
            all.insert(all.end(), v.begin(), v.end());
        }
        candidates = all.size();
        std::vector<Vec3> first(all.size());
        std::vector<char> ok(all.size());
        const double ta = Clock::NowMs();
        for (size_t i = 0; i < all.size(); ++i) ok[i] = ReadPos(all[i].addr, first[i]);
        Sleep(30);
        tb = Clock::NowMs();
        for (size_t i = 0; i < all.size(); ++i) {
            Vec3 b;
            if (!ok[i] || !ReadPos(all[i].addr, b)) continue;
            const auto wa = f.Locate(first[i]), wb = f.Locate(b);
            const float ds = wb.along - wa.along, speed = ds / float((tb - ta) / 1000.0);
            scan::RayFilter::Where tmp;
            const float bv[3] = {b.x, b.y, b.z};
            if (ds > 0.3f && speed > 20.0f && speed < 3000.0f && f.Test(bv, tmp))
                moving.push_back({all[i].addr, wa, wb, speed});
        }
        std::sort(moving.begin(), moving.end(), [](const Moving& a, const Moving& b) { return a.addr < b.addr; });
        for (const Moving& m : moving)
            if (!record && IsRecord(m.addr, origin)) record = m.addr;
    }
    if (record) Remember(record);

    const double t0 = tel.SinceStartMs();
    char at[24];
    std::snprintf(at, sizeof at, "%llx", static_cast<unsigned long long>(record));
    {
        Line l;
        JsonLine(l, "bullet_scan", t0, 0)
            .Bool("full", full)
            .Num("fast_ms", fast_ms)
            .Num("scan_ms", scan_ms)
            .Num("mb", mb)
            .Int("threads", threads)
            .Int("candidates", int64_t(candidates))
            .Bool("truncated", truncated)
            .Int("moving", int64_t(moving.size()))
            .Str("record", at)
            .Num("weapon_speed", weapon.speed)
            .Num("weapon_damage", weapon.damage)
            .Num("ox", origin.x).Num("oy", origin.y).Num("oz", origin.z)
            .Num("dx", f.dir.x).Num("dy", f.dir.y).Num("dz", f.dir.z)
            .Finish();
        Push(tel, l);
    }
    for (size_t i = 0; i < moving.size() && int(i) < kMaxReported; ++i) {
        const Moving& m = moving[i];
        std::snprintf(at, sizeof at, "%llx", static_cast<unsigned long long>(m.addr));
        Line l;
        JsonLine(l, "bullet_cand", t0, 0)
            .Str("addr", at)
            .Num("along", m.b.along)
            .Num("lateral", m.b.lateral)
            .Num("vertical", m.b.vertical)
            .Num("speed", m.speed)
            .Num("since_shot_ms", tb - t_seen)
            .Finish();
        Push(tel, l);
        if (int(i) < kMaxDumped) Dump(tel, m.addr, t0);
    }

    // Follow the record (or, without one, a few moving copies) until it stops moving.
    std::vector<uintptr_t> track;
    if (record) {
        track.push_back(record);
    } else {
        for (const Moving& m : moving) {
            if (int(track.size()) >= kMaxTracked) break;
            if (track.empty() || m.addr - track.back() >= 0x40) track.push_back(m.addr);
        }
    }
    if (record) watch_ = record + kRecSpeedOff;
    const double watch_from = Clock::NowMs();
    std::vector<Vec3> last(track.size(), Vec3{-1e30f, 0, 0});
    std::vector<double> changed(track.size(), Clock::NowMs());
    std::vector<char> live(track.size(), 1);
    const double start = Clock::NowMs();
    while (!stop_.load() && Clock::NowMs() - start < kTrackMs) {
        if (Clock::NowMs() - watch_from > kWatchMs) watch_ = 0;
        bool any = false;
        for (size_t k = 0; k < track.size(); ++k) {
            if (!live[k]) continue;
            uint8_t rec[kRecBytes];
            const double now = Clock::NowMs();
            if (!mem::Read(track[k] + kRecFrom, rec, sizeof rec)) {
                live[k] = 0;
                continue;
            }
            any = true;
            Vec3 p;
            std::memcpy(&p, rec - kRecFrom, sizeof p);
            if (p.x == last[k].x && p.y == last[k].y && p.z == last[k].z) {
                if (now - changed[k] > kStillMs) live[k] = 0;
                continue;
            }
            last[k] = p;
            changed[k] = now;
            std::snprintf(at, sizeof at, "%llx", static_cast<unsigned long long>(track[k]));
            Line l;
            if (record) {
                char hex[kRecBytes * 2 + 1];
                Hex(rec, kRecBytes, hex);
                JsonLine(l, "bullet_rec", tel.SinceStartMs(), 0)
                    .Str("addr", at)
                    .Num("ms", now - t_seen)
                    .Str("hex", hex)
                    .Finish();
            } else {
                const auto w = f.Locate(p);
                JsonLine(l, "bullet_track", tel.SinceStartMs(), 0)
                    .Str("addr", at)
                    .Num("ms", now - t_seen)
                    .Num("along", w.along)
                    .Num("lateral", w.lateral)
                    .Num("vertical", w.vertical)
                    .Num("x", p.x).Num("y", p.y).Num("z", p.z)
                    .Finish();
            }
            Push(tel, l);
        }
        if (!any) break;
        SwitchToThread();
    }
    watch_ = 0;
    for (uintptr_t a : track) Dump(tel, a, tel.SinceStartMs());  // the records once the bullets have stopped
    busy_ = false;
}

}  // namespace grw::bullet
