#include "core/mem.h"
#include <windows.h>
#include <cstring>
#include <iterator>
#include <map>
#include "core/clock.h"

namespace grw::mem {

namespace {

constexpr double kTtlMs = 20'000.0;
constexpr uintptr_t kMinAddress = 0x10000;

struct Region {
    uintptr_t end;
    bool readable;
    bool writable;
    double expires_ms;
};

struct Cache {
    std::map<uintptr_t, Region> regions;  // key = region base; ranges are disjoint
    uint64_t queries = 0;
    uint64_t faults = 0;
};

thread_local Cache t_cache;

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

bool Accessible(DWORD protect, DWORD wanted) {
    return !(protect & (PAGE_GUARD | PAGE_NOACCESS)) && (protect & wanted) != 0;
}

// The cached region containing addr; on a miss, one VirtualQuery caches the whole region.
const Region* Lookup(uintptr_t addr) {
    Cache& c = t_cache;
    const double now = Clock::NowMs();
    auto it = c.regions.upper_bound(addr);
    if (it != c.regions.begin()) {
        auto prev = std::prev(it);
        if (addr < prev->second.end) {
            if (now < prev->second.expires_ms) return &prev->second;
            c.regions.erase(prev);
        }
    }

    MEMORY_BASIC_INFORMATION mbi{};
    ++c.queries;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = base + mbi.RegionSize;
    const bool committed = mbi.State == MEM_COMMIT;

    // Regions can be reshaped (VirtualProtect, free): drop or trim stale overlapping entries.
    auto lo = c.regions.lower_bound(base);
    while (lo != c.regions.end() && lo->first < end) lo = c.regions.erase(lo);
    if (lo != c.regions.begin()) {
        auto before = std::prev(lo);
        if (before->second.end > base) before->second.end = base;
    }

    const Region r{end, committed && Accessible(mbi.Protect, kReadable),
                   committed && Accessible(mbi.Protect, kWritable), now + kTtlMs};
    return &c.regions.insert_or_assign(base, r).first->second;
}

bool RangeOk(uintptr_t addr, size_t size, bool write) {
    if (addr < kMinAddress || size == 0 || addr + size < addr) return false;
    const uintptr_t end = addr + size;
    for (uintptr_t cur = addr; cur < end;) {
        const Region* r = Lookup(cur);
        if (!r || !(write ? r->writable : r->readable)) return false;
        cur = r->end;
    }
    return true;
}

void Invalidate(uintptr_t addr, size_t size) {
    Cache& c = t_cache;
    ++c.faults;
    auto it = c.regions.upper_bound(addr);
    if (it != c.regions.begin()) --it;
    while (it != c.regions.end() && it->first < addr + size) {
        if (it->second.end > addr) it = c.regions.erase(it);
        else ++it;
    }
}

// No C++ objects with destructors here: __try cannot be mixed with unwinding in one function.
// Guard-page hits are caught too, so a read can't crash on another thread's stack guard.
bool GuardedCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                        GetExceptionCode() == STATUS_GUARD_PAGE_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool GuardedPieces(uintptr_t addr, const Piece* pieces, size_t n) {
    __try {
        for (size_t i = 0; i < n; ++i)
            std::memcpy(pieces[i].dst, reinterpret_cast<const void*>(addr + pieces[i].off), pieces[i].size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                        GetExceptionCode() == STATUS_GUARD_PAGE_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

}  // namespace

bool ReadPieces(uintptr_t addr, size_t span, const Piece* pieces, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (pieces[i].off > span || pieces[i].size > span - pieces[i].off) return false;
    if (!RangeOk(addr, span, false)) return false;
    if (GuardedPieces(addr, pieces, n)) return true;
    Invalidate(addr, span);
    return false;
}

bool Read(uintptr_t addr, void* dst, size_t size) {
    if (!RangeOk(addr, size, false)) return false;
    if (GuardedCopy(dst, reinterpret_cast<const void*>(addr), size)) return true;
    Invalidate(addr, size);
    return false;
}

bool Write(uintptr_t addr, const void* src, size_t size) {
    if (!RangeOk(addr, size, true)) return false;
    if (GuardedCopy(reinterpret_cast<void*>(addr), src, size)) return true;
    Invalidate(addr, size);
    return false;
}

CacheStats ThreadCacheStats() {
    return {t_cache.queries, t_cache.faults, t_cache.regions.size()};
}

void ClearThreadCache() {
    t_cache.regions.clear();
    t_cache.queries = 0;
    t_cache.faults = 0;
}

}  // namespace grw::mem
