// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once

// Issue #19: the Cool-down Lap addon (vmknolle) holds back the game's NOT_IN_SESSION hand-over of your car at the
// chequered flag, so you can drive to parc ferme. Terminal Damage takes that hand-over as "your race is over" (the
// failure clocks, the pit calls and the AI cleanup stop), and which hook runs first decides whether it ever sees it.
// Cool-down writes a small shared block per game process instead; this is its layout and how it is read. Pure logic;
// probe/05_controller_switch.inc maps the block and acts on it.
#include <atomic>
#include <cstdint>

namespace cdl {

inline constexpr uint32_t magic = 0x44434D56;  // 'VMCD'
inline constexpr uint32_t version = 1;          // later versions only append; accept any version from 1 up
inline constexpr uint32_t fresh_ms = 5000;      // Cool-down refreshes updated_ms about once a second while active

// Name: L"Local\\vmknolle_cooldown_state_%lu" with GetCurrentProcessId().
inline constexpr const wchar_t *name_format = L"Local\\vmknolle_cooldown_state_%lu";

struct State {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> finished_mask;  // bit v: Cool-down held back the NOT_IN_SESSION hand-over of vehicle v
    std::atomic<uint32_t> parked_mask;    // bit v: an AI car Cool-down parks or holds in parc ferme
    std::atomic<uint32_t> active;         // 1 while a cool-down runs
    std::atomic<uint32_t> updated_ms;     // GetTickCount() of the last write
};
static_assert(sizeof(State) == 24, "the layout Cool-down writes");

// One read of the block.
struct View {
    uint32_t magic = 0, version = 0, finished = 0, parked = 0, active = 0, updated_ms = 0;
};

inline View read(const State &s)
{
    View v;
    v.magic = s.magic;
    v.version = s.version;
    v.finished = s.finished_mask.load();
    v.parked = s.parked_mask.load();
    v.active = s.active.load();
    v.updated_ms = s.updated_ms.load();
    return v;
}

inline bool valid(const View &v) { return v.magic == magic && v.version >= version; }

// A cool-down runs now: active and written within fresh_ms (GetTickCount wraps; the difference does not care).
inline bool live(const View &v, uint32_t now_ms)
{
    return valid(v) && v.active == 1 && uint32_t(now_ms - v.updated_ms) <= fresh_ms;
}

inline bool bit(uint32_t mask, int vehicle) { return vehicle >= 0 && vehicle < 32 && ((mask >> vehicle) & 1u); }

// The race is over for this car: treat it like the NOT_IN_SESSION hand-over.
inline bool finished(const View &v, int vehicle, uint32_t now_ms) { return live(v, now_ms) && bit(v.finished, vehicle); }

// An AI car Cool-down parks: the stopped-car cleanup leaves it alone.
inline bool parked(const View &v, int vehicle, uint32_t now_ms) { return live(v, now_ms) && bit(v.parked, vehicle); }

}  // namespace cdl
