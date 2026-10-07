#include "aim/sway.h"

namespace grw {

void TimeWindowAverage::Add(double t_ms, const Angles& v) {
    buf_[next_] = {t_ms, v};
    next_ = (next_ + 1) % kCap;
    if (count_ < kCap) ++count_;
}

Angles TimeWindowAverage::Mean(double now_ms) const {
    double yaw = 0, pitch = 0;
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
        const Sample& s = buf_[(next_ + kCap - 1 - i) % kCap];
        if (s.t <= now_ms - window_ms_) break;  // samples are in time order: the rest are older
        yaw += s.v.yaw;
        pitch += s.v.pitch;
        ++n;
    }
    if (n == 0) return {};
    return {float(yaw / double(n)), float(pitch / double(n))};
}

}  // namespace grw
