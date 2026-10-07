#pragma once

namespace grw {

// Wall time from QPC; thread CPU time from QueryThreadCycleTime divided by an rdtsc rate measured once.
class Clock {
public:
    static void Calibrate();
    static double NowMs();
    static double ThreadCpuMs();
    static double TscPerMs();
};

}  // namespace grw
