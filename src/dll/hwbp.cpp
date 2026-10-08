#include "dll/hwbp.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw::hwbp {

namespace {

constexpr int kStackSlots = 64;  // qwords scanned above RSP
constexpr int kMaxReturns = 12;  // image addresses kept per hit
constexpr int kRing = 32;
constexpr uintptr_t kNone[4] = {};
constexpr int kDump = 0x100;  // bytes copied at [rcx] and [rdx] on each hit

struct Hit {
    uint32_t tid;
    int slot;  // which DR fired
    uint64_t rip;
    uint64_t regs[16];  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint64_t returns[kMaxReturns];
    int n_returns;
    uint8_t mem_rcx[kDump], mem_rdx[kDump];
    bool ok_rcx, ok_rdx;
};

Hit g_ring[kRing];
std::atomic<uint32_t> g_written{0};  // hits recorded by the handler
uint32_t g_read = 0;                 // writer thread only
std::atomic<int> g_hits{0};
std::atomic<int> g_max_hits{8};
std::atomic<bool> g_armed{false};
PVOID g_veh = nullptr;
uintptr_t g_image_lo = 0, g_image_hi = 0;
uintptr_t g_self_lo = 0, g_self_hi = 0;  // this DLL: its own reads (ESP scans) are not the game's
uintptr_t g_addrs[4] = {};
int g_len = 0;
Mode g_mode = Mode::Write;
std::atomic<bool> g_exec{false};

// SEH-guarded copy (no C++ objects here): the pointer in a register may be anything.
bool SafeCopy(void* dst, uint64_t src, size_t n) {
    if (src < 0x10000 || src > 0x7FFFFFFFFFFF - n) return false;
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const uint64_t fired = c->Dr6 & 0xF;
    if (!fired) return EXCEPTION_CONTINUE_SEARCH;  // not one of our DR0-DR3
    c->Dr6 = 0;
    // An instruction breakpoint fires before the instruction: resume with RF so it is not hit again at once.
    if (g_exec.load()) c->EFlags |= 0x10000;
    if (c->Rip >= g_self_lo && c->Rip < g_self_hi) return EXCEPTION_CONTINUE_EXECUTION;
    const int n = g_hits.fetch_add(1) + 1;
    if (n > g_max_hits.load()) {
        c->Dr7 &= ~uint64_t(0xFFFF00FF);  // stop on this thread; Configure(all 0) clears the others
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint32_t slot = g_written.load() % kRing;
    Hit& h = g_ring[slot];
    h.tid = GetCurrentThreadId();
    h.slot = (fired & 1) ? 0 : (fired & 2) ? 1 : (fired & 4) ? 2 : 3;
    h.rip = c->Rip;
    const uint64_t r[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                            c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
    for (int i = 0; i < 16; ++i) h.regs[i] = r[i];
    h.n_returns = 0;
    const auto* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
    for (int i = 0; i < kStackSlots && h.n_returns < kMaxReturns; ++i) {
        const uint64_t v = sp[i];  // the faulting thread's own stack: readable
        if (v >= g_image_lo && v < g_image_hi) h.returns[h.n_returns++] = v;
    }
    h.ok_rcx = SafeCopy(h.mem_rcx, c->Rcx, kDump);
    h.ok_rdx = SafeCopy(h.mem_rdx, c->Rdx, kDump);
    g_written.fetch_add(1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Applies DR0/DR7 to every other thread of the process. No allocation while a thread is suspended:
// it may hold the heap lock.
void ApplyAll(const uintptr_t (&addrs)[4], int len, Mode mode) {
    std::vector<DWORD> tids;
    tids.reserve(512);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);

    const uint64_t len_bits = mode == Mode::Execute ? 0 : len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
    const uint64_t rw_bits = mode == Mode::Execute ? 0 : mode == Mode::ReadWrite ? 3 : 1;
    for (DWORD tid : tids) {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                uint64_t dr7 = ctx.Dr7 & ~uint64_t(0xFFFF00FF);  // our L0-L3/G0-G3 and RW/LEN fields
                DWORD64* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                for (int i = 0; i < 4; ++i) {
                    *regs[i] = addrs[i];
                    if (addrs[i]) dr7 |= (1ull << (2 * i)) | (rw_bits << (16 + 4 * i)) | (len_bits << (18 + 4 * i));
                }
                ctx.Dr7 = dr7;
                ctx.Dr6 = 0;
                SetThreadContext(t, &ctx);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
}

}  // namespace

void Configure(const uintptr_t (&addrs)[4], int len, int max_hits, Mode mode) {
    const bool any = addrs[0] || addrs[1] || addrs[2] || addrs[3];
    bool same = len == g_len && mode == g_mode;
    for (int i = 0; i < 4; ++i) same = same && addrs[i] == g_addrs[i];
    if (same && (!any || g_armed.load())) return;
    if (!g_image_lo) {
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        g_image_lo = reinterpret_cast<uintptr_t>(base);
        g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Handler), &self);
        auto* sb = reinterpret_cast<const uint8_t*>(self);
        const auto* snt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            sb + reinterpret_cast<const IMAGE_DOS_HEADER*>(sb)->e_lfanew);
        g_self_lo = reinterpret_cast<uintptr_t>(sb);
        g_self_hi = g_self_lo + snt->OptionalHeader.SizeOfImage;
    }
    for (int i = 0; i < 4; ++i) g_addrs[i] = addrs[i];
    g_len = len;
    g_mode = mode;
    g_exec = mode == Mode::Execute;
    if (any) {
        g_hits = 0;
        g_max_hits = max_hits;
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &Handler);
        ApplyAll(addrs, len, mode);
        g_armed = true;
    } else {
        ApplyAll(kNone, 0, Mode::Write);
        g_armed = false;
    }
}

void Drain(Telemetry& tel) {
    const uint32_t w = g_written.load();
    char buf[24];
    static const char* const kReg[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (; g_read < w; ++g_read) {
        const Hit h = g_ring[g_read % kRing];
        Line l1;
        JsonLine j1(l1, "hwbp_hit", tel.SinceStartMs(), 0);
        j1.Int("n", g_read).Int("tid", h.tid).Int("slot", h.slot);
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.rip - g_image_lo));
        j1.Str("rip_rva", buf);
        for (int i = 0; i < 16; ++i) {
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.regs[i]));
            j1.Str(kReg[i], buf);
        }
        j1.Finish();
        tel.Push(Channel::Writer, l1);
        Line l2;
        JsonLine j2(l2, "hwbp_stack", tel.SinceStartMs(), 0);
        j2.Int("n", g_read);
        char key[8];
        for (int i = 0; i < h.n_returns; ++i) {
            std::snprintf(key, sizeof key, "r%d", i);
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.returns[i] - g_image_lo));
            j2.Str(key, buf);
        }
        j2.Finish();
        tel.Push(Channel::Writer, l2);
        // memory at [rcx] and [rdx], 0x40 bytes of hex per line
        char hex[0x40 * 2 + 1];
        for (int which = 0; which < 2; ++which) {
            const bool ok = which ? h.ok_rdx : h.ok_rcx;
            if (!ok) continue;
            const uint8_t* m = which ? h.mem_rdx : h.mem_rcx;
            for (int off = 0; off < kDump; off += 0x40) {
                for (int i = 0; i < 0x40; ++i) std::snprintf(hex + i * 2, 3, "%02x", m[off + i]);
                Line l3;
                JsonLine(l3, "hwbp_mem", tel.SinceStartMs(), 0)
                    .Int("n", g_read)
                    .Str("reg", which ? "rdx" : "rcx")
                    .Int("off", off)
                    .Str("hex", hex)
                    .Finish();
                tel.Push(Channel::Writer, l3);
            }
        }
    }
}

void Shutdown() {
    if (g_armed.load()) ApplyAll(kNone, 0, Mode::Write);
    g_armed = false;
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
}

}  // namespace grw::hwbp
