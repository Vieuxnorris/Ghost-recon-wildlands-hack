#pragma once
#include <cstddef>
#include "math/angles.h"

namespace grw {

// Mean of the samples newer than window_ms; fixed capacity (256), oldest overwritten. No allocation.
class TimeWindowAverage {
public:
    explicit TimeWindowAverage(double window_ms) : window_ms_(window_ms) {}
    void Add(double t_ms, const Angles& v);
    Angles Mean(double now_ms) const;
    void Reset() { count_ = 0; }

private:
    static constexpr size_t kCap = 256;
    struct Sample { double t; Angles v; };
    Sample buf_[kCap] = {};
    size_t next_ = 0;
    size_t count_ = 0;
    double window_ms_;
};

}  // namespace grw
