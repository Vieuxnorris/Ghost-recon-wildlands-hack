#include "aim/trajectory.h"
#include <cmath>
#include <cstdio>
#include <sstream>

namespace grw {

namespace {
constexpr int kMaxGap = 12;  // metres between two measured bins that may be interpolated (steps are ~2-6 m)
}

void TrajectoryTable::Add(float along, float drop, float t) {
    if (!(along >= 0) || !std::isfinite(drop) || !std::isfinite(t)) return;
    const int m = int(std::lround(along));
    if (m > kMaxMetres) return;
    bins_[size_t(m)] = {drop, t, true};
    if (along > max_along_) max_along_ = along;
    ++samples_;
}

bool TrajectoryTable::Lookup(float along, float& drop, float& t) const {
    if (!(along >= 0) || along > max_along_ + 0.5f) return false;
    const int m = int(std::floor(along));
    int lo = m, hi = m + 1;
    while (lo > 0 && !bins_[size_t(lo)].valid && m - lo < kMaxGap) --lo;
    while (hi <= kMaxMetres && !bins_[size_t(hi)].valid && hi - m < kMaxGap) ++hi;
    // the muzzle: no drop, no time
    const Bin a = lo == 0 && !bins_[0].valid ? Bin{0, 0, true} : bins_[size_t(lo)];
    if (!a.valid) return false;
    if (hi > kMaxMetres || !bins_[size_t(hi)].valid) {
        if (along - float(lo) > 1.0f) return false;
        drop = a.drop;
        t = a.t;
        return true;
    }
    const Bin& b = bins_[size_t(hi)];
    const float f = (along - float(lo)) / float(hi - lo);
    drop = a.drop + (b.drop - a.drop) * f;
    t = a.t + (b.t - a.t) * f;
    return true;
}

void TrajectoryTable::Clear() {
    for (Bin& b : bins_) b = Bin{};
    max_along_ = 0;
    samples_ = 0;
}

std::string TrajectoryTable::Serialize() const {
    std::string out;
    char line[64];
    for (int m = 0; m <= kMaxMetres; ++m) {
        const Bin& b = bins_[size_t(m)];
        if (!b.valid) continue;
        std::snprintf(line, sizeof line, "%d %.4f %.5f\n", m, b.drop, b.t);
        out += line;
    }
    return out;
}

bool TrajectoryTable::Parse(const std::string& text) {
    TrajectoryTable next;
    std::istringstream in(text);
    std::string row;
    while (std::getline(in, row)) {
        if (row.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream fields(row);
        int m;
        float drop, t;
        std::string extra;
        if (!(fields >> m >> drop >> t) || (fields >> extra) || m < 0 || m > kMaxMetres) return false;
        next.bins_[size_t(m)] = {drop, t, true};
        if (float(m) > next.max_along_) next.max_along_ = float(m);
        ++next.samples_;
    }
    *this = std::move(next);
    return true;
}

namespace {

// drop and time at `along`, extrapolated past the learned range (drop ~ distance^2, time ~ distance)
bool DropAt(const TrajectoryTable& tab, float along, float& drop, float& t, bool& extrapolated) {
    extrapolated = false;
    if (tab.Lookup(along, drop, t)) return true;
    const float max = tab.MaxAlong();
    if (max < 20.0f || along <= max || !tab.Lookup(max, drop, t)) return false;
    const float k = along / max;
    drop *= k * k;
    t *= k;
    extrapolated = true;
    return true;
}

}  // namespace

bool TableAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const TrajectoryTable& tab,
                   TableAim& out) {
    Vec3 p = target;
    float drop = 0, t = 0;
    bool extra = false;
    for (int i = 0; i < 2; ++i) {  // the flight time depends on where the led target is
        if (!DropAt(tab, (p - origin).Length(), drop, t, extra)) return false;
        p = target + velocity * t;
    }
    if (!DropAt(tab, (p - origin).Length(), drop, t, extra)) return false;
    const Vec3 los = p - origin;
    const float len = los.Length();
    if (len < 1e-3f) return false;
    const Vec3 u = los * (1.0f / len);
    Vec3 up = Vec3{0, 0, 1} - u * u.z;
    const float ul = up.Length();
    up = ul > 1e-4f ? up * (1.0f / ul) : Vec3{0, 0, 1};
    out.point = p + up * drop;
    out.flight_s = t;
    out.drop_m = drop;
    out.extrapolated = extra;
    return true;
}

bool WeaponSig::operator==(const WeaponSig& o) const {
    return std::fabs(speed - o.speed) < 0.01f && std::fabs(damage - o.damage) < 0.01f;
}

TrajectoryTable* TrajectoryBook::Table(const BulletKey& k) {
    for (Entry& e : tables_)
        if (e.key == k) return &e.table;
    tables_.push_back({k, {}});
    return &tables_.back().table;
}

void TrajectoryBook::Link(const WeaponSig& w, const BulletKey& k) {
    if (!k.Valid()) return;
    for (auto& l : links_)
        if (l.weapon == w) {
            if (!(l.key == k)) dirty_ = true;
            l.key = k;
            return;
        }
    links_.push_back({w, k});
    dirty_ = true;
}

void TrajectoryBook::Add(const BulletKey& k, float along, float drop, float t) {
    if (!k.Valid()) return;
    Table(k)->Add(along, drop, t);
    dirty_ = true;
}

const TrajectoryTable* TrajectoryBook::Find(const WeaponSig& w, BulletKey* key) const {
    for (const WeaponLink& l : links_)
        if (l.weapon == w)
            for (const Entry& e : tables_)
                if (e.key == l.key) {
                    if (key) *key = e.key;
                    return &e.table;
                }
    return nullptr;
}

std::string TrajectoryBook::Serialize() const {
    std::string out;
    char line[160];
    for (const WeaponLink& l : links_) {
        std::snprintf(line, sizeof line, "weapon %.3f %.3f %.4f %.4f %.4f\n", l.weapon.speed, l.weapon.damage, l.key.V,
                      l.key.v0, l.key.first_segment);
        out += line;
    }
    for (const Entry& e : tables_) {
        std::snprintf(line, sizeof line, "table %.4f %.4f %.4f\n", e.key.V, e.key.v0, e.key.first_segment);
        out += line + e.table.Serialize() + "end\n";
    }
    return out;
}

bool TrajectoryBook::Parse(const std::string& text) {
    TrajectoryBook next;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        if (word == "weapon") {
            WeaponLink l;
            if (!(in >> l.weapon.speed >> l.weapon.damage >> l.key.V >> l.key.v0 >> l.key.first_segment)) return false;
            next.links_.push_back(l);
        } else if (word == "table") {
            Entry e;
            if (!(in >> e.key.V >> e.key.v0 >> e.key.first_segment)) return false;
            std::string body, row;
            std::getline(in, row);  // rest of the "table" line
            while (std::getline(in, row) && row != "end") body += row + "\n";
            if (row != "end" || !e.table.Parse(body)) return false;
            next.tables_.push_back(std::move(e));
        } else {
            return false;
        }
    }
    *this = std::move(next);
    return true;
}

bool BulletKey::operator==(const BulletKey& o) const {
    return std::fabs(V - o.V) < 0.01f && std::fabs(v0 - o.v0) < 0.01f && std::fabs(first_segment - o.first_segment) < 0.01f;
}

}  // namespace grw
