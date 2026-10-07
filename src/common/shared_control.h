#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace grw {

inline constexpr uint32_t kControlMagic = 0x47525721;  // "GRW!"

// Shared between the injector and the DLL loaded in process <pid>.
struct SharedControl {
    uint32_t magic;
    volatile LONG unload_request;
    volatile LONG loaded;
    wchar_t base_dir[MAX_PATH];
};

inline std::wstring ControlName(DWORD pid) { return L"Local\\grw_control_" + std::to_wstring(pid); }

class ControlMapping {
public:
    ControlMapping() = default;
    ControlMapping(const ControlMapping&) = delete;
    ControlMapping& operator=(const ControlMapping&) = delete;
    ~ControlMapping() { Close(); }

    // Injector side: create (or reuse) and initialise.
    bool Create(DWORD pid) {
        h_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedControl),
                                ControlName(pid).c_str());
        return Map();
    }

    // DLL side, and injector --unload: open an existing mapping.
    bool Open(DWORD pid) {
        h_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ControlName(pid).c_str());
        return Map();
    }

    SharedControl* Get() const { return p_; }

    void Close() {
        if (p_) UnmapViewOfFile(p_);
        if (h_) CloseHandle(h_);
        p_ = nullptr;
        h_ = nullptr;
    }

private:
    bool Map() {
        if (!h_) return false;
        p_ = static_cast<SharedControl*>(MapViewOfFile(h_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl)));
        return p_ != nullptr;
    }

    HANDLE h_ = nullptr;
    SharedControl* p_ = nullptr;
};

}  // namespace grw
