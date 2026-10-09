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

constexpr float kRadToDeg = 57.29578f;

// measured on 2026-10-09 (camera-findings.md section 7b): v/V against the speed law's angle, every 0.2 deg
constexpr float kSeedCurve[][2] = {
    {0.0f, 1.0f},    {0.2f, 0.936f}, {0.4f, 0.878f}, {0.6f, 0.819f}, {0.8f, 0.768f}, {1.0f, 0.718f},
    {1.2f, 0.671f},  {1.4f, 0.632f}, {1.6f, 0.593f}, {1.8f, 0.561f}, {2.0f, 0.533f}, {2.2f, 0.510f},
    {2.4f, 0.490f},  {2.6f, 0.475f}, {2.8f, 0.463f}, {3.0f, 0.452f}, {3.2f, 0.444f}, {3.4f, 0.437f},
    {3.6f, 0.431f},  {3.8f, 0.427f}, {4.0f, 0.422f}, {4.2f, 0.418f}, {4.4f, 0.414f}, {4.6f, 0.410f},
    {4.8f, 0.405f},  {5.0f, 0.401f}, {5.2f, 0.400f}, {19.98f, 0.400f}};

}  // namespace

SpeedCurve::SpeedCurve() {
    for (const auto& p : kSeedCurve) {
        Bin& b = bins_[size_t(std::lround(p[0] / kBinDeg))];
        b.sum = p[1];
        b.n = 1;
    }
    Rebuild();
}

void SpeedCurve::Rebuild() {
    dense_.assign(kBins, 0.0f);
    int prev = -1;
    for (int i = 0; i < kBins; ++i) {
        if (!bins_[size_t(i)].n) continue;
        const float y = float(bins_[size_t(i)].sum / bins_[size_t(i)].n);
        if (prev < 0) {
            for (int j = 0; j <= i; ++j) dense_[size_t(j)] = y;
        } else {
            const float y0 = dense_[size_t(prev)];
            for (int j = prev + 1; j <= i; ++j) dense_[size_t(j)] = y0 + (y - y0) * float(j - prev) / float(i - prev);
        }
        prev = i;
    }
    if (prev >= 0)
        for (int j = prev + 1; j < kBins; ++j) dense_[size_t(j)] = dense_[size_t(prev)];
}

float SpeedCurve::Lookup(float angle_deg) const {
    const float x = std::fmax(0.0f, angle_deg) / kBinDeg;
    if (!(x < float(kBins - 1))) return dense_.back();
    const int i = int(x);
    return dense_[size_t(i)] + (dense_[size_t(i) + 1] - dense_[size_t(i)]) * (x - float(i));
}

bool SpeedCurve::Add(float angle_deg, float ratio) {
    if (!(angle_deg >= 0) || !(ratio > 0.05f && ratio < 1.2f)) return false;
    if (std::fabs(ratio - Lookup(angle_deg)) > 0.08f) return false;
    const long i = std::lround(angle_deg / kBinDeg);
    if (i >= kBins) return false;
    bins_[size_t(i)].sum += ratio;
    ++bins_[size_t(i)].n;
    ++samples_;
    Rebuild();
    return true;
}

std::string SpeedCurve::Serialize() const {
    std::string out = "steps " + std::to_string(samples_) + "\n";
    char line[64];
    for (int i = 0; i < kBins; ++i) {
        const Bin& b = bins_[size_t(i)];
        if (!b.n) continue;
        std::snprintf(line, sizeof line, "%.2f %.5f %u\n", i * kBinDeg, b.sum / b.n, b.n);
        out += line;
    }
    return out;
}

bool SpeedCurve::Parse(const std::string& text) {
    std::vector<Bin> bins(kBins);
    int samples = 0, bins_read = 0;
    std::istringstream in(text);
    std::string row;
    while (std::getline(in, row)) {
        if (row.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream fields(row);
        if (row.rfind("steps", 0) == 0) {  // the steps learned (the seeded points excluded)
            std::string word;
            if (!(fields >> word >> samples) || samples < 0) return false;
            continue;
        }
        float angle, mean;
        uint32_t n;
        std::string extra;
        if (!(fields >> angle >> mean >> n) || (fields >> extra) || !(angle >= 0) || n == 0) return false;
        const long i = std::lround(angle / kBinDeg);
        if (i >= kBins || !std::isfinite(mean)) return false;
        bins[size_t(i)] = {double(mean) * n, n};
        ++bins_read;
    }
    if (!bins_read) return false;
    bins_ = std::move(bins);
    samples_ = samples;
    Rebuild();
    return true;
}

float LaunchLength(float spread, float spread_range, float aim_dist) {
    if (!(std::fabs(spread) > 1e-5f)) return 1.0f;
    const float cap = spread_range > 0 ? spread_range : 100.0f;
    const float L = std::fmax(0.1f, std::fmin(cap, aim_dist));
    return L / std::sqrt(L * L + 1.0f);
}

float FlightTime(const TrajectoryTable& tab, const FlightModel& m, float launch_len, float sin_elevation, float along) {
    if (!(along > 0) || !m.Valid()) return 0;
    const float first = m.V * kFirstStepS;
    if (along <= first) return along / m.V;
    const float el = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sin_elevation)));
    float t = kFirstStepS, x = first, slope = 0;
    int cached = -1;  // whole metre whose drop is in d_cached
    float d_cached = 0;
    auto drop_at = [&](int metre, float& d) {
        float tt;
        return tab.Lookup(float(metre), d, tt);
    };
    while (x < along) {
        const int mi = int(x);
        float d0 = 0, d1 = 0;
        const bool ok0 = mi == cached ? true : drop_at(mi, d0);
        if (mi == cached) d0 = d_cached;
        if (ok0 && drop_at(mi + 1, d1)) {
            slope = d1 - d0;  // else the last slope (beyond the learned range)
            cached = mi + 1;
            d_cached = d1;
        }
        const float delta = std::atan(slope);  // below the launch line: positive
        const float g = std::fmax(0.0f, std::fmin(1.0f, launch_len * std::cos(delta)));
        float v = std::fmax(20.0f, m.V * m.curve->Lookup(std::acos(g) * kRadToDeg));
        v += 4.905f * t * t * std::fmax(-std::sin(el - delta), 0.0f);
        const float h = std::fmin(float(mi + 1) - x, along - x);
        t += h / v;
        x += h;
    }
    return t;
}

namespace {

// the flight time to p: the speed law when the model is valid, else the table's
void ModelTime(const Vec3& origin, const Vec3& p, const TrajectoryTable& tab, const FlightModel* model, float& t,
               TableAim& out) {
    out.modelled = false;
    out.launch_len = 1;
    if (!model || !model->Valid()) return;
    const Vec3 r = p - origin;
    const float along = r.Length();
    if (along < 1e-3f) return;
    out.launch_len = LaunchLength(model->spread, model->spread_range, along);
    t = FlightTime(tab, *model, out.launch_len, r.z / along, along);
    out.modelled = true;
}

}  // namespace

bool TableAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const TrajectoryTable& tab,
                   TableAim& out, const FlightModel* model) {
    Vec3 p = target;
    float drop = 0, t = 0;
    bool extra = false;
    for (int i = 0; i < 2; ++i) {  // the flight time depends on where the led target is
        if (!DropAt(tab, (p - origin).Length(), drop, t, extra)) return false;
        ModelTime(origin, p, tab, model, t, out);
        p = target + velocity * t;
    }
    if (!DropAt(tab, (p - origin).Length(), drop, t, extra)) return false;
    ModelTime(origin, p, tab, model, t, out);
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

const TrajectoryTable* TrajectoryBook::FindSetup(float V, float first_segment, BulletKey* key) const {
    const Entry* best = nullptr;
    for (const Entry& e : tables_)
        if (std::fabs(e.key.V - V) < 0.05f && std::fabs(e.key.first_segment - first_segment) < 0.05f &&
            (!best || e.table.Samples() > best->table.Samples()))
            best = &e;
    if (!best) return nullptr;
    if (key) *key = best->key;
    return &best->table;
}

void TrajectoryBook::AddCurve(float angle_deg, float ratio) {
    if (curve_.Add(angle_deg, ratio)) dirty_ = true;
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
    out += "curve\n" + curve_.Serialize() + "end\n";
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
        } else if (word == "curve") {
            std::string body, row;
            std::getline(in, row);
            while (std::getline(in, row) && row != "end") body += row + "\n";
            if (row != "end" || !next.curve_.Parse(body)) return false;
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
