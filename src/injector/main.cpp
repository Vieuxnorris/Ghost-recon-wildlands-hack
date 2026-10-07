#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <initializer_list>
#include <string>
#include "common/shared_control.h"

namespace fs = std::filesystem;

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = wchar_t(towlower(ch));
    return s;
}

bool ContainsAny(const std::wstring& name, std::initializer_list<const wchar_t*> needles) {
    const std::wstring low = Lower(name);
    for (const wchar_t* n : needles)
        if (low.find(n) != std::wstring::npos) return true;
    return false;
}

DWORD FindProcess(const std::wstring& exe) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (Lower(e.szExeFile) == Lower(exe)) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

bool AnyProcessMatches(std::initializer_list<const wchar_t*> needles, std::wstring& found) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof e};
    bool hit = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !hit; ok = Process32NextW(snap, &e))
        if (ContainsAny(e.szExeFile, needles)) { hit = true; found = e.szExeFile; }
    CloseHandle(snap);
    return hit;
}

bool ModuleMatches(DWORD pid, std::initializer_list<const wchar_t*> needles, std::wstring& found) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{sizeof m};
    bool hit = false;
    for (BOOL ok = Module32FirstW(snap, &m); ok && !hit; ok = Module32NextW(snap, &m))
        if (ContainsAny(m.szModule, needles)) { hit = true; found = m.szModule; }
    CloseHandle(snap);
    return hit;
}

bool GrwLoaded(DWORD pid) {
    std::wstring found;
    return ModuleMatches(pid, {L"grw_"}, found);
}

// Project rule: never inject next to an anti-cheat. This is a refusal, not a bypass.
bool AntiCheatPresent(DWORD pid, std::wstring& what) {
    return ModuleMatches(pid, {L"easyanticheat", L"beclient", L"battleye"}, what) ||
           AnyProcessMatches({L"easyanticheat", L"beservice"}, what);
}

template <class Pred>
bool WaitFor(Pred pred, DWORD timeout_ms) {
    for (DWORD waited = 0; waited < timeout_ms; waited += 50) {
        if (pred()) return true;
        Sleep(50);
    }
    return pred();
}

bool LoadRemote(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr);
    if (ok) {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE t = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr);
        const DWORD wait = t ? WaitForSingleObject(t, 5000) : WAIT_FAILED;
        DWORD code = 0;
        ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code != 0;  // truncated HMODULE
        if (!ok)
            std::fwprintf(stderr, L"remote LoadLibraryW: %ls\n",
                          wait == WAIT_TIMEOUT ? L"timed out after 5 s (loader lock held?)"
                          : wait == WAIT_OBJECT_0 ? L"returned NULL" : L"thread creation or wait failed");
        if (t) CloseHandle(t);
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

void RemoveStaleCopies(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().wstring().starts_with(L"grw_")) fs::remove(e.path(), ec);  // fails if loaded
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path bin = fs::path(self).parent_path();  // build\bin\RelWithDebInfo
    std::wstring proc_name = L"testhost.exe";
    fs::path dll = bin / L"grw.dll";
    fs::path base = bin.parent_path().parent_path().parent_path() / L"runtime";
    bool unload = false;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!wcscmp(argv[i], L"--proc") && has_value) proc_name = argv[++i];
        else if (!wcscmp(argv[i], L"--dll") && has_value) dll = argv[++i];
        else if (!wcscmp(argv[i], L"--base") && has_value) base = argv[++i];
        else if (!wcscmp(argv[i], L"--unload")) unload = true;
    }

    const DWORD pid = FindProcess(proc_name);
    if (!pid) {
        std::fwprintf(stderr, L"process %ls not found\n", proc_name.c_str());
        return 1;
    }

    if (unload) {
        grw::ControlMapping control;
        if (!control.Open(pid)) {
            std::fwprintf(stderr, L"grw is not loaded in %lu\n", pid);
            return 1;
        }
        InterlockedExchange(&control.Get()->unload_request, 1);
        const bool gone = WaitFor([&] { return control.Get()->loaded == 0 && !GrwLoaded(pid); }, 5000);
        std::fwprintf(gone ? stdout : stderr, gone ? L"unloaded from %lu\n" : L"unload timed out in %lu\n", pid);
        return gone ? 0 : 1;
    }

    std::wstring what;
    if (AntiCheatPresent(pid, what)) {
        std::fwprintf(stderr, L"anti-cheat detected (%ls): refusing to inject (project rule)\n", what.c_str());
        return 4;
    }
    if (GrwLoaded(pid)) {
        std::fwprintf(stderr, L"grw already loaded in %lu: run --unload first\n", pid);
        return 1;
    }

    std::error_code ec;
    fs::create_directories(base, ec);
    const fs::path tmp = fs::temp_directory_path() / L"grw";
    fs::create_directories(tmp, ec);
    RemoveStaleCopies(tmp);
    const fs::path copy = tmp / (L"grw_" + std::to_wstring(GetTickCount64()) + L".dll");
    if (!fs::copy_file(dll, copy, fs::copy_options::overwrite_existing, ec)) {
        std::fwprintf(stderr, L"cannot copy %ls: %hs\n", dll.c_str(), ec.message().c_str());
        return 1;
    }

    grw::ControlMapping control;
    if (!control.Create(pid)) {
        std::fwprintf(stderr, L"cannot create the control mapping\n");
        return 1;
    }
    grw::SharedControl* c = control.Get();
    c->magic = grw::kControlMagic;
    c->unload_request = 0;
    c->loaded = 0;
    wcsncpy_s(c->base_dir, fs::absolute(base).c_str(), _TRUNCATE);

    if (!LoadRemote(pid, copy.wstring())) {
        std::fwprintf(stderr, L"LoadLibraryW failed in %lu\n", pid);
        return 1;
    }
    if (!WaitFor([&] { return c->loaded == 1; }, 5000)) {
        std::fwprintf(stderr, L"grw did not report loaded within 5 s\n");
        return 1;
    }
    std::wprintf(L"loaded in %lu, base %ls\n", pid, c->base_dir);
    return 0;
}
