#pragma once
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "common/shared_control.h"
#include "config/config.h"
#include "core/stats.h"
#include "dll/aimbot.h"
#include "game/world.h"
#include "telemetry/telemetry.h"

namespace grw {

// Everything the DLL owns. OnFrame/OnResize run on the game's render thread, Tick on the telemetry writer
// thread, Start/Stop/Status on the DLL's main thread.
class Runtime {
public:
    bool Start(const std::wstring& base_dir, SharedControl* control);
    void Stop();
    void OnFrame(IDXGISwapChain* sc);
    void OnResize(IDXGISwapChain* sc, bool before);
    void OnPresented(double cpu_ms, double wall_ms);
    void Status(const char* event, const char* detail);

    Telemetry& Tel() { return *tel_; }
    Config Cfg() const { return config_->Snapshot(); }
    bool MenuOpen() const { return menu_open_.load(std::memory_order_relaxed); }
    bool PanelOpen() const { return panel_open_.load(std::memory_order_relaxed); }
    uint64_t Frame() const { return frame_.load(std::memory_order_relaxed); }
    const Percentiles* LastStats() const { return last_stats_; }  // render thread only
    // Render thread, inside the ImGui frame: reads the game and refreshes Esp().
    void UpdateEsp(float width, float height);
    const game::EspFrame& Esp() const { return esp_; }  // render thread only
    bool EspOn() const { return esp_on_.load(std::memory_order_relaxed); }
    const Aimbot::Status& AimStatus() const { return aim_.Last(); }  // render thread only

private:
    void Tick();
    void PushFrameStats();
    void PushEspStats();

    std::unique_ptr<Telemetry> tel_;
    std::unique_ptr<ConfigStore> config_;
    std::unique_ptr<StageStats> stats_;
    SharedControl* control_ = nullptr;
    std::atomic<bool> menu_open_{false};
    std::atomic<bool> panel_open_{true};
    std::atomic<uint64_t> frame_{0};
    std::atomic<int> overlay_mode_{2};  // copied from the config on the writer thread
    bool key_down_[4] = {};
    game::World world_;
    game::EspFrame esp_;
    Aimbot aim_;
    std::atomic<bool> esp_on_{true};
    double esp_window_ms_ = 0;
    double window_start_ms_ = 0;
    uint64_t window_frames_ = 0;
    double last_frame_ms_ = 0;
    Percentiles last_stats_[kStageCount] = {};
    bool overlay_reported_ = false;
};

Runtime& Rt();

}  // namespace grw
