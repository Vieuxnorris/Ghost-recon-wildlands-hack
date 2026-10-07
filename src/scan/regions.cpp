#include "scan/regions.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "core/mem.h"
#include "scan/matrix_detect.h"

namespace grw::scan {

namespace {

constexpr uint32_t kOneBits = 0x3F800000u;  // 1.0f

uint32_t Bits(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool Prefilter(const uint8_t* p, MatrixKind kind) {
    if (kind == MatrixKind::Rigid) return Bits(p + 60) == kOneBits;
    if (Bits(p + 60) != 0) return false;
    return (Bits(p + 44) & 0x7FFFFFFFu) == kOneBits || (Bits(p + 56) & 0x7FFFFFFFu) == kOneBits;
}

// Calls visit(block_addr, data, block_len, copied_len, stats) for every readable block; stops on cancel, or
// when visit returns false (candidate set full).
template <class Visit>
ScanStats ForEachBlock(const std::vector<Region>& regions, ScanBuffer& buf, Reader read, void* ctx,
                       const std::atomic<bool>* cancel, Visit visit) {
    ScanStats st;
    for (const Region& r : regions) {
        ++st.regions;
        for (size_t off = 0; off < r.size; off += ScanBuffer::kBlock) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                st.cancelled = true;
                return st;
            }
            const size_t block_len = std::min(ScanBuffer::kBlock, r.size - off);
            const size_t copy_len = std::min(block_len + ScanBuffer::kOverlap, r.size - off);
            if (!read(r.base + off, buf.Data(), copy_len, ctx)) {
                ++st.unreadable_blocks;
                continue;
            }
            st.bytes += block_len;
            if (!visit(r.base + off, buf.Data(), block_len, copy_len, st)) {
                st.truncated = true;
                return st;
            }
        }
    }
    return st;
}

bool Readable(DWORD protect, bool writable_only) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = protect & 0xFF;
    if (writable_only) return base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READWRITE;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

std::vector<Region> Subtract(std::vector<Region> regions, const MemRange* ex, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t eb = ex[i].base, ee = ex[i].base + ex[i].size;
        std::vector<Region> next;
        for (const Region& r : regions) {
            const uintptr_t rb = r.base, re = r.base + r.size;
            if (ee <= rb || eb >= re) {
                next.push_back(r);
                continue;
            }
            if (eb > rb) next.push_back({rb, eb - rb});
            if (ee < re) next.push_back({ee, re - ee});
        }
        regions = std::move(next);
    }
    return regions;
}

std::vector<Region> ReadableRegions(bool writable_only, const MemRange* ex, size_t n) {
    std::vector<Region> out;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) == sizeof mbi) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && Readable(mbi.Protect, writable_only)) out.push_back({base, mbi.RegionSize});
        a = base + mbi.RegionSize;
    }
    return Subtract(std::move(out), ex, n);
}

bool ProcessReader(uintptr_t addr, void* dst, size_t size, void*) { return mem::Read(addr, dst, size); }

ScanBuffer::ScanBuffer()
    : data_(static_cast<uint8_t*>(
          VirtualAlloc(nullptr, kBlock + kOverlap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {}

ScanBuffer::~ScanBuffer() {
    if (data_) VirtualFree(data_, 0, MEM_RELEASE);
}

ScanStats ScanMatrices(const std::vector<Region>& regions, MatrixKind kind, ScanBuffer& buf, CandidateSet& out,
                       Reader read, void* ctx, const std::atomic<bool>* cancel) {
    return ForEachBlock(regions, buf, read, ctx, cancel,
                        [&](uintptr_t at, const uint8_t* d, size_t block_len, size_t copy_len, ScanStats& st) {
                            for (size_t p = 0; p < block_len && p + 64 <= copy_len; p += 4) {
                                if (!Prefilter(d + p, kind)) continue;
                                Candidate c{at + p, kFloats16, {}};
                                std::memcpy(c.v, d + p, 64);
                                RigidInfo ri;
                                PerspInfo pi;
                                const bool hit = kind == MatrixKind::Rigid ? IsRigid(c.v, ri) : IsPerspective(c.v, pi);
                                if (!hit) continue;
                                if (!out.Add(c)) return false;
                                ++st.found;
                            }
                            return true;
                        });
}

ScanStats ScanFloats(const std::vector<Region>& regions, bool (*pred)(float, void*), void* pctx, ScanBuffer& buf,
                     CandidateSet& out, Reader read, void* ctx, const std::atomic<bool>* cancel) {
    return ForEachBlock(regions, buf, read, ctx, cancel,
                        [&](uintptr_t at, const uint8_t* d, size_t block_len, size_t, ScanStats& st) {
                            for (size_t p = 0; p + 4 <= block_len; p += 4) {
                                float v;
                                std::memcpy(&v, d + p, 4);
                                if (!pred(v, pctx)) continue;
                                Candidate c{at + p, kFloats1, {}};
                                c.v[0] = v;
                                if (!out.Add(c)) return false;
                                ++st.found;
                            }
                            return true;
                        });
}

}  // namespace grw::scan
