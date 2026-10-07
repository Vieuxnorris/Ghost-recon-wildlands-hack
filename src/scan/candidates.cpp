#include "scan/candidates.h"
#include <cmath>

namespace grw::scan {

bool CandidateSet::Add(const Candidate& c) {
    if (items_.size() >= kMax) return false;
    items_.push_back(c);
    return true;
}

// In place: items_ never reallocates (Storage() stays valid). Pass 1 refreshes values and flags each item
// (0 unreadable, 1 readable, 2 kept); pass 2 compacts on the kept ones, or on the readable ones if none is kept.
template <class Keep> RefineResult CandidateSet::Filter(Keep keep, Reader read, void* ctx) {
    RefineResult r;
    r.before = items_.size();
    std::vector<uint8_t> flag(items_.size(), 0);
    for (size_t i = 0; i < items_.size(); ++i) {
        Candidate& c = items_[i];
        float fresh[16];
        if (!read(c.addr, fresh, c.kind * sizeof(float), ctx)) {
            ++r.unreadable;
            continue;
        }
        Candidate now = c;
        for (uint32_t k = 0; k < c.kind; ++k) now.v[k] = fresh[k];
        flag[i] = keep(c, now) ? 2 : 1;
        if (flag[i] == 2) ++r.kept;
        c = now;
    }
    r.applied = r.kept > 0;
    const uint8_t min_flag = r.applied ? 2 : 1;
    size_t w = 0;
    for (size_t i = 0; i < items_.size(); ++i)
        if (flag[i] >= min_flag) items_[w++] = items_[i];
    items_.resize(w);
    return r;
}

RefineResult CandidateSet::Refine(RefineMode mode, float eps, Reader read, void* ctx) {
    return Filter(
        [&](const Candidate& old, const Candidate& now) {
            bool moved = false;
            for (uint32_t i = 0; i < old.kind && !moved; ++i) moved = std::fabs(now.v[i] - old.v[i]) > eps;
            return mode == RefineMode::Changed ? moved : !moved;
        },
        read, ctx);
}

RefineResult CandidateSet::KeepMatching(bool (*pred)(const float* v16, void* pctx), void* pctx, Reader read,
                                        void* ctx) {
    return Filter([&](const Candidate&, const Candidate& now) { return pred(now.v, pctx); }, read, ctx);
}

}  // namespace grw::scan
