#pragma once
#include <cstddef>
#include <cstdint>

namespace grw::mem {

// Never crash: each copy is checked against a per-thread cache of whole VirtualQuery regions (20 s)
// and guarded by SEH. A faulting copy drops the cached regions it touched.
bool Read(uintptr_t addr, void* dst, size_t size);
bool Write(uintptr_t addr, const void* src, size_t size);

template <class T> bool Read(uintptr_t addr, T& out) { return Read(addr, &out, sizeof(T)); }
template <class T> bool Write(uintptr_t addr, const T& value) { return Write(addr, &value, sizeof(T)); }

struct CacheStats {
    uint64_t queries = 0;  // VirtualQuery calls
    uint64_t faults = 0;   // guarded copies that faulted
    size_t entries = 0;
};
CacheStats ThreadCacheStats();
void ClearThreadCache();  // drops entries, resets counters

}  // namespace grw::mem
