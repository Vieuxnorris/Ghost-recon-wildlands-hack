#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include "math/vec.h"
#include "scan/regions.h"

namespace grw {
class Telemetry;
}

// Research (grw.ini bullet_probe = 1, with the execute breakpoint on the fire function): after a shot, finds
// the bullet in flight and records its trajectory. All writable memory is scanned on several threads for
// float triplets near the line of fire (seconds: shoot into the sky so that the bullet lives long enough);
// the triplets that move forward along it between two reads 30 ms apart are the bullet's position. The one
// whose record holds the shot origin 0x40 bytes before it is the bullet record: its region is remembered,
// so that the next shots find their record there within milliseconds, and the record is snapshotted at
// every simulation step ("bullet_rec"). Without a record, the moving copies are followed ("bullet_track").
namespace grw::bullet {

struct WeaponInfo {
    float speed = 0, damage = 0;  // the active weapon's design data, to tell the shots apart
};

class Probe {
public:
    ~Probe() { Stop(); }
    // Writer thread. Ignored while the previous shot is still being processed.
    void Shot(const Vec3& origin, const Vec3& dir, const WeaponInfo& weapon, Telemetry& tel);
    void Stop();
    // Writer thread: an address to watch for writes (the record's speed field) for a short while after the
    // record is found, so that the hardware breakpoint module catches the code that updates it; 0 = none.
    uintptr_t Watch() const { return watch_.load(std::memory_order_relaxed); }

private:
    void Run(Vec3 origin, Vec3 dir, WeaponInfo weapon, double t_seen, Telemetry* tel);
    void Remember(uintptr_t addr);  // probe thread

    std::vector<scan::Region> hot_;  // regions that held bullet records (probe thread only)

    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uintptr_t> watch_{0};
};

}  // namespace grw::bullet
