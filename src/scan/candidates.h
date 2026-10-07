#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace grw::scan {

// Reads size bytes at addr; false when unreadable. ctx is passed through (tests use a fake memory).
using Reader = bool (*)(uintptr_t addr, void* dst, size_t size, void* ctx);

inline constexpr uint32_t kFloats16 = 16;  // a 4x4 matrix
inline constexpr uint32_t kFloats1 = 1;    // one float (angles)

struct Candidate {
    uintptr_t addr = 0;
    uint32_t kind = kFloats16;  // how many floats of v count
    float v[16] = {};
};

enum class RefineMode : uint8_t { Changed, Unchanged };

struct RefineResult {
    size_t before = 0;
    size_t kept = 0;
    size_t unreadable = 0;
    bool applied = false;  // false: it would have kept nothing, the previous list is kept (minus unreadable)
};

struct MemRange {
    uintptr_t base = 0;
    size_t size = 0;
};

// Scan thread only (not thread-safe). Storage is reserved once and never moves (see Storage()).
class CandidateSet {
public:
    static constexpr size_t kMax = 1'000'000;

    CandidateSet() { items_.reserve(kMax); }
    // Our own copies of game values live here: the region scan must skip this range.
    MemRange Storage() const { return {reinterpret_cast<uintptr_t>(items_.data()), kMax * sizeof(Candidate)}; }
    bool Add(const Candidate& c);
    size_t Size() const { return items_.size(); }
    const Candidate& At(size_t i) const { return items_[i]; }
    void Clear() { items_.clear(); }

    // Re-reads every candidate; Changed keeps those where any counted float moved by more than eps.
    RefineResult Refine(RefineMode mode, float eps, Reader read, void* ctx);
    // Re-reads every candidate and keeps those for which pred(fresh values) is true.
    RefineResult KeepMatching(bool (*pred)(const float* v16, void* pctx), void* pctx, Reader read, void* ctx);

private:
    template <class Keep> RefineResult Filter(Keep keep, Reader read, void* ctx);
    std::vector<Candidate> items_;
};

}  // namespace grw::scan
