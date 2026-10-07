#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "scan/candidates.h"

namespace grw::scan {

struct Region {
    uintptr_t base = 0;
    size_t size = 0;
};

// Regions minus the excluded ranges (pure; regions may be split).
std::vector<Region> Subtract(std::vector<Region> regions, const MemRange* ex, size_t n);

// Committed, readable regions of this process (no guard, no no-access pages), minus the excluded ranges.
// writable_only keeps PAGE_READWRITE, PAGE_WRITECOPY and PAGE_EXECUTE_READWRITE: game data, not code.
std::vector<Region> ReadableRegions(bool writable_only, const MemRange* ex, size_t n);

// Reader over this process's memory, through mem::Read (SEH + whole-region cache).
bool ProcessReader(uintptr_t addr, void* dst, size_t size, void* ctx);

// Block copy buffer, VirtualAlloc'd so that its range is known and excluded from scans.
class ScanBuffer {
public:
    static constexpr size_t kBlock = 1 << 20;
    static constexpr size_t kOverlap = 60;  // a 64-byte matrix starting in the block may end in the next one
    ScanBuffer();
    ~ScanBuffer();
    ScanBuffer(const ScanBuffer&) = delete;
    ScanBuffer& operator=(const ScanBuffer&) = delete;
    uint8_t* Data() { return data_; }
    MemRange Range() const { return {reinterpret_cast<uintptr_t>(data_), kBlock + kOverlap}; }

private:
    uint8_t* data_ = nullptr;
};

enum class MatrixKind : uint8_t { Rigid, Perspective };

struct ScanStats {
    size_t regions = 0;
    uint64_t bytes = 0;
    size_t unreadable_blocks = 0;
    size_t found = 0;
    bool truncated = false;  // the candidate set was full
    bool cancelled = false;
};

// Adds every matching 4-aligned 4x4 float matrix as a kFloats16 candidate.
ScanStats ScanMatrices(const std::vector<Region>& regions, MatrixKind kind, ScanBuffer& buf, CandidateSet& out,
                       Reader read, void* ctx, const std::atomic<bool>* cancel);
// Adds every 4-aligned float for which pred(value) is true, as a kFloats1 candidate.
ScanStats ScanFloats(const std::vector<Region>& regions, bool (*pred)(float, void*), void* pctx, ScanBuffer& buf,
                     CandidateSet& out, Reader read, void* ctx, const std::atomic<bool>* cancel);

}  // namespace grw::scan
