#pragma once
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/shared_control.h"
#include "aim/trajectory.h"
#include "config/config.h"
#include "core/stats.h"
#include "dll/aimbot.h"
#include "dll/bullet_probe.h"
#include "dll/los.h"
#include "game/resources.h"
#include "game/stealth.h"
#include "game/teleport.h"
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
    // The HUD (panel key cycles it): 0 = hidden, 1 = one discreet status line, 2 = with the details.
    int HudLevel() const { return hud_level_.load(std::memory_order_relaxed); }
    void SetHudLevel(int level) { hud_level_.store(level, std::memory_order_relaxed); }
    uint64_t Frame() const { return frame_.load(std::memory_order_relaxed); }
    const Percentiles* LastStats() const { return last_stats_; }  // render thread only
    // Render thread, inside the ImGui frame: reads the game and refreshes Esp().
    void UpdateEsp(float width, float height);
    const game::EspFrame& Esp() const { return esp_; }  // render thread only
    bool EspOn() const { return esp_on_.load(std::memory_order_relaxed); }
    void SetEspOn(bool on) { esp_on_.store(on, std::memory_order_relaxed); }
    const Aimbot::Status& AimStatus() const { return aim_.Last(); }  // render thread only
    bool TrackerArmed() const { return tracker_armed_.load(std::memory_order_relaxed); }
    // Weapon mods (Config::mod_*): factors of the game's values, set from grw.ini when it is (re)loaded and from
    // the menu's sliders in between.
    enum class Mod { Spread, BulletSpeed, Damage, Recoil, Count };
    float ModFactor(Mod m) const { return mods_[size_t(m)].load(std::memory_order_relaxed); }
    void SetModFactor(Mod m, float k) { mods_[size_t(m)].store(k, std::memory_order_relaxed); }
    // Instant bullets: the request's V set to kInstantBulletSpeed (the speed factor is then ignored).
    static constexpr float kInstantBulletSpeed = 30000.0f;
    bool InstantBullets() const { return instant_.load(std::memory_order_relaxed); }
    bool InfiniteAmmo() const { return infinite_ammo_.load(std::memory_order_relaxed); }
    int AmmoFlag() const { return ammo_flag_; }  // render thread: the game's flag as last read (-1 = not found)
    void SetInstantBullets(bool on) { instant_.store(on, std::memory_order_relaxed); }
    // Straight bullets: the request's first segment (no drop) set to game::kStraightFirstSegment.
    bool StraightBullets() const { return straight_.load(std::memory_order_relaxed); }
    void SetStraightBullets(bool on) { straight_.store(on, std::memory_order_relaxed); }
    // Teleported bullets: straight, and their first step's ray reaches the end of their path (hwbp::SetBulletPath).
    bool TeleportBullets() const { return teleport_.load(std::memory_order_relaxed); }
    void SetTeleportBullets(bool on) { teleport_.store(on, std::memory_order_relaxed); }
    // Magic bullets: while the aim holds a target, the player's bullets jump to its head (hwbp::SetMagicTarget).
    bool MagicBullets() const { return magic_.load(std::memory_order_relaxed); }
    void SetMagicBullets(bool on) { magic_.store(on, std::memory_order_relaxed); }
    // The menu's settings: in use at once (SetCfg); SaveCfg writes grw.ini, ReloadCfg reads it again (writer
    // thread, next tick). CfgDirty: edits not saved yet.
    void SetCfg(const Config& c) {
        config_->Set(c);
        cfg_dirty_.store(true, std::memory_order_relaxed);
    }
    bool SaveCfg();
    void ReloadCfg() { config_->Invalidate(); }
    bool CfgDirty() const { return cfg_dirty_.load(std::memory_order_relaxed); }
    // The magic bullets' results (render thread): since injection, and over the last kMagicRecent.
    static constexpr int kMagicRecent = 30;
    struct MagicTally {
        uint32_t shots = 0, kills = 0, recent = 0, recent_kills = 0;
    };
    MagicTally MagicResults() const;
    uint64_t SpreadModHits() const;
    uint64_t RecoilModHits() const;
    float TrackerCallsPerS() const { return tracker_calls_per_s_.load(std::memory_order_relaxed); }
    // Teleport (tp_key / the menu): under the map's cursor when the map is open (once it closes), else to the map
    // waypoint. Requested from any thread, run on the render thread.
    void RequestTeleport() { tp_request_.store(true, std::memory_order_relaxed); }
    struct TpView {
        game::TpPhase phase = game::TpPhase::Idle;
        char msg[96] = {};
        double at_ms = -1e18;  // when msg was set
    };
    const TpView& TeleportView() const { return tp_view_; }  // render thread only
    // The rebels' resources and the skill points (game/resources.h), render thread: read every 0.5 s with the menu
    // open, else every 2 s (the "resources" telemetry line, on a change and every 10 s); set from the menu.
    struct ResView {
        game::ResourceState state;
        bool read = false;  // read at least once
        char msg[112] = {};  // the last set
        bool msg_bad = false;
    };
    void UpdateResources();
    const ResView& ResourcesView() const { return res_view_; }
    void SetResource(game::Resource r, int32_t value);
    // Invisibility (invisible_key / the menu; game/stealth.h), render thread: checked every second, applied to the
    // player's visibility component, found by a scan for its vtable on a thread of its own (~3 s, then only checked).
    bool StealthOn() const { return stealth_on_.load(std::memory_order_relaxed); }
    void SetStealthOn(bool on) {
        stealth_on_.store(on, std::memory_order_relaxed);
        stealth_poke_.store(true, std::memory_order_relaxed);
    }
    struct StealthView {
        bool applied = false;    // our zeros are in the game
        bool searching = false;  // the scan is running
        char msg[96] = {};
        bool msg_bad = false;
    };
    void UpdateStealth();
    const StealthView& StealthStatus() const { return stealth_view_; }  // render thread only

private:
    std::atomic<bool> stealth_on_{false};
    std::atomic<bool> stealth_poke_{false};  // check at the next frame (toggled)
    int cfg_invisible_ = -1;
    game::Stealth stealth_;
    uintptr_t stealth_owner_ = 0;  // the player's entity when it was applied
    float stealth_noise_max_ = 0;  // this second: the largest noise radius the game had set before our 0
    int64_t stealth_noise_writes_ = 0;
    double stealth_check_ms_ = -1e18;
    double stealth_scan_at_ms_ = -1e18;  // the last scan's start
    StealthView stealth_view_;
    // the scan's thread and its results (read once it is done)
    std::thread stealth_scan_;
    std::atomic<bool> stealth_scanning_{false};
    std::atomic<bool> stealth_stop_{false};
    std::atomic<uintptr_t> stealth_found_{0};
    std::atomic<uint32_t> stealth_scan_hits_{0};  // qwords equal to the vtable
    std::atomic<float> stealth_scan_ms_{0};
    bool stealth_scan_logged_ = true;
    void StealthScan(uintptr_t image_base, uintptr_t player);  // its thread
    void StopStealth();  // Stop: the game's values back
    ResView res_view_;
    double res_read_ms_ = -1e18;
    double res_log_ms_ = -1e18;
    game::ResourceState res_logged_;  // the amounts in the last telemetry line
    void UpdateTeleport();  // render thread, after BuildFrame
    void StartTeleport(double now_ms, const Vec3* cursor);  // cursor null: to the waypoint
    void TpEvent(const char* event, const char* msg);  // the menu's line and a "tp" telemetry line
    std::atomic<bool> tp_request_{false};
    game::TpQueue tp_queue_;  // a map cursor kept until the map closes
    game::Teleporter tp_;
    game::TpPhase tp_phase_ = game::TpPhase::Idle;  // last phase reported
    uintptr_t tp_entity_ = 0;  // the player's entity being moved
    int tp_rays_ = 0;
    bool tp_held_ = false;      // the player was written at the hold position (its body is then under the rays)
    float tp_ground_z_ = 0;     // the ground found
    double tp_settle_ms_ = -1;  // when to log where the player ended up (-1 = nothing pending)
    float tp_place_z_ = 0;      // the height written at the end; until the log, the largest rise and fall from it
    float tp_max_up_ = 0, tp_max_down_ = 0;
    TpView tp_view_;
    void Tick();
    bool LearningWanted() const;
    void PushTrackerStats(bool armed);
    void SaveTrajectories();
    void PushFrameStats();
    void PushEspStats();
    void ApplyDataMods();  // render thread
    void RestoreDataMods();  // Stop, once the render hooks are gone
    // Infinite ammo (render thread): the game's flag set while `on`, put back to 0 when off (only where we set it).
    void ApplyAmmoFlag(bool on);
    uintptr_t ammo_flag_obj_ = 0;  // the object whose flag we set
    int ammo_flag_ = -1;

    std::unique_ptr<Telemetry> tel_;
    std::unique_ptr<ConfigStore> config_;
    std::unique_ptr<StageStats> stats_;
    SharedControl* control_ = nullptr;
    std::atomic<bool> menu_open_{false};
    std::atomic<int> hud_level_{1};
    std::atomic<uint64_t> frame_{0};
    std::atomic<int> overlay_mode_{2};  // copied from the config on the writer thread
    bool key_down_[6] = {};
    game::World world_;
    game::EspFrame esp_;
    Aimbot aim_;
    bullet::Probe probe_;
    std::atomic<float> cam_x_{0}, cam_y_{0}, cam_z_{0};  // render thread -> writer thread (player's shots)
    LineOfSight los_;
    std::unordered_map<uintptr_t, LosVerdict> los_verdicts_;  // render thread
    std::vector<LosRequest> los_reqs_;                        // render thread
    double los_purge_ms_ = 0;                                 // render thread
    void UpdateLineOfSight(const Config& c);                  // render thread
    // Magic bullets (render thread): the enemies, their velocity, the approach of each (a hidden one: from above or
    // a side, tested by the LOS worker), published to hwbp; the result of each magic bullet ("magic_shot").
    void UpdateMagic(const Config& c);
    struct MagicTrack {
        Vec3 pos, vel;
        Vec3 feet;  // its root, at the last frame
        double ms = 0;
        int k = -1;          // the approach chosen (-1: none known clear; 10 + k: blocked, started past the obstacle)
        float start_m = 12;  // where its last test starts, m short of the head
        bool est = false;    // head estimated from the feet
        bool vehicle = false;
        game::Category cat = game::Category::Unknown;
    };
    std::unordered_map<uintptr_t, MagicTrack> magic_tracks_;
    struct MagicPending {
        uintptr_t addr;
        double ms;
        float dist;
        int approach;
        uint64_t seq;
    };
    std::vector<MagicPending> magic_pending_;
    // Diagnostics: what the collision test of that bullet's last segment met ("magic_seg", "magic_hit" lines).
    void LogMagicHits(const MagicPending& p, bool killed);
    uint32_t magic_shots_ = 0, magic_kills_ = 0;
    bool magic_recent_[kMagicRecent] = {};
    std::atomic<bool> cfg_dirty_{false};
    uint64_t magic_seq_ = 0;
    double magic_purge_ms_ = 0;
    std::atomic<float> weapon_speed_{0}, weapon_damage_{0};
    std::atomic<uint64_t> weapon_id_{0};
    std::atomic<float> bullet_V_{0}, bullet_first_{0};  // the weapon's ballistics block, 0 = unknown
    std::atomic<float> mods_[size_t(Mod::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
    float cfg_mods_[size_t(Mod::Count)] = {-1, -1, -1, -1};  // writer thread: the config values last copied
    std::atomic<bool> instant_{false};
    int cfg_instant_ = -1;  // writer thread: the config value last copied
    std::atomic<bool> infinite_ammo_{false};
    int cfg_infinite_ammo_ = -1;
    std::atomic<bool> straight_{false};
    int cfg_straight_ = -1;
    std::atomic<bool> teleport_{false};
    int cfg_teleport_ = -1;
    std::atomic<bool> magic_{false};
    int cfg_magic_ = -1;
    std::atomic<uintptr_t> aim_comp_{0};  // the player's aim component (render -> writer)
    std::atomic<uintptr_t> spread_comp_{0};             // the player's spread component (render -> writer)
    std::atomic<uintptr_t> weapon_logic_{0};            // the player's weapon W (render -> writer)
    std::atomic<int> pellets_{1};                       // bullets per shot armed (writer -> render)
    // render thread: the values rewritten by a mod, by address, with the game's own value under each (switching
    // weapons back and forth must not stack a factor)
    struct DataMod {
        int kind = 0;  // 0 = bullet speed (the request), 1 = damage (the ballistics block)
        float game = 0, written = 0;
    };
    std::unordered_map<uintptr_t, DataMod> data_mods_;
    std::atomic<double> aim_beyond_ms_{-1e18};  // last time the aim extrapolated past the learned path
    // Writer thread: the projectile tracker (proj_track = 2) and the weapons whose link was confirmed by a shot.
    std::vector<uint64_t> confirmed_weapons_;
    // Writer thread: each traced projectile's previous step (the speed it shows was computed at that step).
    struct CurveStep {
        float angle_deg, elapsed, dir_z;
    };
    std::unordered_map<uint64_t, CurveStep> curve_prev_;
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
