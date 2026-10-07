#include "aim/target.h"

namespace grw {

namespace {
bool VisibleNow(const Candidate& c, const TargetParams& p) {
    return c.los_visible && c.los_age_ms <= p.max_los_age_ms;
}
}  // namespace

uint32_t TargetSelector::Update(double now_ms, bool key_held, const Candidate* c, size_t n,
                                const TargetParams& p) {
    if (!key_held) {
        current_ = 0;
        return 0;
    }

    if (current_ != 0) {
        const Candidate* cur = nullptr;
        for (size_t i = 0; i < n; ++i)
            if (c[i].id == current_) cur = &c[i];
        if (cur && cur->alive && cur->enemy) {
            if (VisibleNow(*cur, p)) last_visible_ms_ = now_ms;
            if (now_ms - last_visible_ms_ <= p.hidden_ms) return current_;
        }
        current_ = 0;
    }

    const Candidate* best = nullptr;
    for (size_t i = 0; i < n; ++i) {
        const Candidate& k = c[i];
        if (k.id == 0 || !k.enemy || !k.alive || !VisibleNow(k, p) || k.angle_rad > p.cone_rad) continue;
        if (!best || k.angle_rad < best->angle_rad) best = &k;
    }
    if (best) {
        current_ = best->id;
        last_visible_ms_ = now_ms;
    }
    return current_;
}

}  // namespace grw
