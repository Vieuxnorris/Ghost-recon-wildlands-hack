#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace grw {

#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas(64) is the point: no false sharing

// One producer thread, one consumer thread. Push never blocks: a full ring drops and counts.
template <class T, size_t N>
class SpscRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    bool Push(const T& v) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool Pop(T& out) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
    T buf_[N];
};

#pragma warning(pop)

}  // namespace grw
