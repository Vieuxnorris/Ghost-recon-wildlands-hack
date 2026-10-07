#include "core/clock.h"
#include <windows.h>
#include <intrin.h>

namespace grw {

namespace {
double g_qpc_per_ms = 0.0;
double g_tsc_per_ms = 0.0;

double QpcPerMs() {
    if (g_qpc_per_ms == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_per_ms = double(f.QuadPart) / 1000.0;
    }
    return g_qpc_per_ms;
}
}  // namespace

void Clock::Calibrate() {
    if (g_tsc_per_ms != 0.0) return;
    const double w0 = NowMs();
    const unsigned long long t0 = __rdtsc();
    Sleep(50);
    const double w1 = NowMs();
    const unsigned long long t1 = __rdtsc();
    g_tsc_per_ms = double(t1 - t0) / (w1 - w0);
}

double Clock::NowMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / QpcPerMs();
}

double Clock::ThreadCpuMs() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return g_tsc_per_ms > 0.0 ? double(cycles) / g_tsc_per_ms : 0.0;
}

double Clock::TscPerMs() { return g_tsc_per_ms; }

}  // namespace grw
