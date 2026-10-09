#pragma once
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "common/shared_control.h"
#include "aim/trajectory.h"
#include "config/config.h"
#include "core/stats.h"
#include "dll/aimbot.h"
#include "dll/bullet_probe.h"
#include "dll/los.h"
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
    bool TrackerArmed() const { return tracker_armed_.load(std::memory_order_relaxed); }
    float TrackerCallsPerS() const { return tracker_calls_per_s_.load(std::memory_order_relaxed); }

private:
    void Tick();
    bool LearningWanted() const;
    void PushTrackerStats(bool armed);
    void SaveTrajectories();
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
    bullet::Probe probe_;
    std::atomic<float> cam_x_{0}, cam_y_{0}, cam_z_{0};  // render thread -> writer thread (player's shots)
    LineOfSight los_;
    std::unordered_map<uintptr_t, LosVerdict> los_verdicts_;  // render thread
    std::vector<LosRequest> los_reqs_;                        // render thread
    double los_purge_ms_ = 0;                                 // render thread
    void UpdateLineOfSight();                                 // render thread
    std::atomic<float> weapon_speed_{0}, weapon_damage_{0};
    std::atomic<uint64_t> weapon_id_{0};
    std::atomic<double> aim_beyond_ms_{-1e18};  // last time the aim extrapolated past the learned path
    // Writer thread: the projectile tracker (proj_track = 2) and the weapons whose link was confirmed by a shot.
    std::vector<uint64_t> confirmed_weapons_;
    double track_until_ms_ = 0;
    std::atomic<bool> tracker_armed_{false};
    std::atomic<float> tracker_calls_per_s_{0};
    double tracker_window_ms_ = 0;
    uint64_t tracker_calls0_ = 0, tracker_samples0_ = 0;
    // Learned bullet paths (writer thread adds the player's projectile steps, render thread aims with them).
    TrajectoryBook book_;
    mutable SRWLOCK book_lock_ = SRWLOCK_INIT;
    std::wstring book_path_;
    double book_saved_ms_ = 0;
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
