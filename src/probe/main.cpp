// Read-only probe: who has hooked the DXGI swap chain functions inside a running game?
// Finds Present/Present1/ResizeBuffers in our own process (throwaway swap chain), then reads the same
// addresses in the target with ReadProcessMemory, decodes jumps and names the module they land in.
// Usage: probe --proc GRW.exe [--watch <seconds>]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Module {
    uintptr_t base, size;
    std::string name;
};

struct Fn {
    const char* name;
    uintptr_t addr;   // in our process
    uintptr_t slot;   // vtable slot address in our process
    BYTE local[16];
};

DWORD FindPid(const wchar_t* exe) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

std::vector<Module> Modules(HANDLE proc) {
    std::vector<Module> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof mods, &needed, LIST_MODULES_64BIT)) return out;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        MODULEINFO mi{};
        char name[MAX_PATH] = "?";
        GetModuleInformation(proc, mods[i], &mi, sizeof mi);
        GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
        out.push_back({uintptr_t(mi.lpBaseOfDll), uintptr_t(mi.SizeOfImage), name});
    }
    return out;
}

std::string Owner(const std::vector<Module>& mods, uintptr_t a) {
    for (const Module& m : mods)
        if (a >= m.base && a < m.base + m.size) {
            char buf[MAX_PATH + 32];
            std::snprintf(buf, sizeof buf, "%s+0x%llx", m.name.c_str(), (unsigned long long)(a - m.base));
            return buf;
        }
    return "<no module: private memory>";
}

bool Read(HANDLE proc, uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(proc, reinterpret_cast<void*>(a), out, n, &got) && got == n;
}

// Decodes the common detour shapes; returns 0 when the bytes are not a jump.
uintptr_t JumpTarget(HANDLE proc, uintptr_t a, const BYTE* b) {
    if (b[0] == 0xE9) return a + 5 + int32_t(b[1] | b[2] << 8 | b[3] << 16 | b[4] << 24);
    if (b[0] == 0xFF && b[1] == 0x25) {  // jmp [rip+disp32]
        const uintptr_t at = a + 6 + int32_t(b[2] | b[3] << 8 | b[4] << 16 | b[5] << 24);
        uintptr_t t = 0;
        return Read(proc, at, &t, sizeof t) ? t : 0;
    }
    if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {  // mov rax, imm64; jmp rax
        uintptr_t t;
        std::memcpy(&t, b + 2, sizeof t);
        return t;
    }
    if (b[0] == 0x68 && b[5] == 0xC7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04) {  // push lo; mov [rsp+4], hi; ret
        uint32_t lo, hi;
        std::memcpy(&lo, b + 1, 4);
        std::memcpy(&hi, b + 9, 4);
        return uintptr_t(hi) << 32 | lo;
    }
    return 0;
}

void Hex(const BYTE* b, int n) {
    for (int i = 0; i < n; ++i) std::printf("%02x ", b[i]);
}

void Follow(HANDLE proc, const std::vector<Module>& mods, uintptr_t a) {
    for (int hop = 0; hop < 4; ++hop) {
        BYTE b[16];
        if (!Read(proc, a, b, sizeof b)) {
            std::printf("      unreadable at 0x%llx\n", (unsigned long long)a);
            return;
        }
        const uintptr_t t = JumpTarget(proc, a, b);
        if (!t) return;
        std::printf("      hop %d -> 0x%llx  %s\n", hop + 1, (unsigned long long)t, Owner(mods, t).c_str());
        a = t;
    }
}

bool LocalFunctions(Fn (&fns)[3], uintptr_t& vtable) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"grw_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)))
        return false;
    void** vt = *reinterpret_cast<void***>(sc);
    vtable = uintptr_t(vt);
    fns[0] = {"Present", uintptr_t(vt[8]), uintptr_t(&vt[8]), {}};
    fns[2] = {"ResizeBuffers", uintptr_t(vt[13]), uintptr_t(&vt[13]), {}};
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        void** vt1 = *reinterpret_cast<void***>(sc1);
        fns[1] = {"Present1", uintptr_t(vt1[22]), uintptr_t(&vt1[22]), {}};
        sc1->Release();
    }
    for (Fn& f : fns)
        if (f.addr) std::memcpy(f.local, reinterpret_cast<void*>(f.addr), sizeof f.local);
    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* exe = nullptr;
    int watch_s = 0;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--proc") && i + 1 < argc) exe = argv[++i];
        else if (!wcscmp(argv[i], L"--watch") && i + 1 < argc) watch_s = _wtoi(argv[++i]);
    }
    if (!exe) {
        std::printf("usage: probe --proc <exe> [--watch <seconds>]\n");
        return 2;
    }
    const DWORD pid = FindPid(exe);
    HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) : nullptr;
    if (!proc) {
        std::printf("process not found or not readable\n");
        return 1;
    }
    Fn fns[3] = {};
    uintptr_t vtable = 0;
    if (!LocalFunctions(fns, vtable)) {
        std::printf("could not create a swap chain\n");
        return 1;
    }

    // System DLLs share one base per boot; check it rather than assume it.
    const uintptr_t local_dxgi = uintptr_t(GetModuleHandleW(L"dxgi.dll"));
    const std::vector<Module> mods = Modules(proc);
    uintptr_t remote_dxgi = 0;
    for (const Module& m : mods)
        if (_stricmp(m.name.c_str(), "dxgi.dll") == 0) remote_dxgi = m.base;
    if (!remote_dxgi) {
        std::printf("dxgi.dll not loaded in pid %lu\n", pid);
        return 1;
    }
    const intptr_t delta = intptr_t(remote_dxgi) - intptr_t(local_dxgi);
    std::printf("pid %lu, dxgi local 0x%llx remote 0x%llx (delta %lld), %zu modules\n", pid,
                (unsigned long long)local_dxgi, (unsigned long long)remote_dxgi, (long long)delta, mods.size());

    for (const Fn& f : fns) {
        if (!f.addr) continue;
        const uintptr_t ra = f.addr + delta;
        uintptr_t slot = 0;
        Read(proc, f.slot + delta, &slot, sizeof slot);
        BYTE remote[16] = {};
        Read(proc, ra, remote, sizeof remote);
        std::printf("\n%s  dxgi+0x%llx\n", f.name, (unsigned long long)(f.addr - local_dxgi));
        std::printf("   vtable slot -> 0x%llx  %s%s\n", (unsigned long long)slot, Owner(mods, slot).c_str(),
                    slot == ra ? "" : "   <-- vtable hooked");
        std::printf("   clean : ");
        Hex(f.local, 16);
        std::printf("\n   remote: ");
        Hex(remote, 16);
        std::printf("%s\n", std::memcmp(f.local, remote, 16) ? "  <-- patched" : "");
        Follow(proc, mods, ra);
    }

    if (watch_s > 0) {
        std::printf("\nwatching prologues every 10 ms for %d s...\n", watch_s);
        BYTE last[3][16];
        for (int i = 0; i < 3; ++i) Read(proc, fns[i].addr + delta, last[i], 16);
        int changes = 0;
        const ULONGLONG end = GetTickCount64() + ULONGLONG(watch_s) * 1000;
        while (GetTickCount64() < end) {
            for (int i = 0; i < 3; ++i) {
                if (!fns[i].addr) continue;
                BYTE now[16];
                if (!Read(proc, fns[i].addr + delta, now, 16) || !std::memcmp(now, last[i], 16)) continue;
                ++changes;
                std::printf("  %llu ms %s changed: ", GetTickCount64(), fns[i].name);
                Hex(now, 16);
                std::printf("\n");
                std::memcpy(last[i], now, 16);
            }
            Sleep(10);
        }
        std::printf("%d prologue changes\n", changes);
    }
    CloseHandle(proc);
    return 0;
}
