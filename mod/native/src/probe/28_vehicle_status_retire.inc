// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Vehicle status hook, bus posts, retire_player.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// PitLaneSystem::setVehicleStatus (0x1434bc540, rcx system, dl vehicle, r8d status); the current status is
// [system + vehicle*16 + 0x68] -> +4. In a race the box stop logic finishes its tyre stop (InPitBox_CrewCompleted)
// even while the car is BeingPushedIntoGarage and then releases it, so for the broken player's car we refuse
// every change from BeingPushedIntoGarage back to the box stop states.
constexpr const char *sig_set_vehicle_status =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC E8 00 00 00 C5 79 29 84 24 D0 00 00 00 C5 F8 29 BC 24 C0 00 00 00 "
    "C5 F8 29 B4 24 B0 00 00 00 80 FA 1A 0F 87 FD 08 00 00 89 D3 48 89 CE 0F B6 FA 49 89 FC 49 C1 E4 04 4A 8B 44 21 "
    "68 8B 68 04";
constexpr uint8_t set_vehicle_status_prologue[19] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                                     0x55, 0x53, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
using set_vehicle_status_fn = uint64_t (*)(void *, uint64_t, uint64_t);
set_vehicle_status_fn orig_set_vehicle_status;

// How the hook is installed (0.75.10). Up to 0.75.9 the addon patched the function's first bytes itself, found by a
// byte signature. Another addon that wraps Game_Pitlane_SetVehicleStatus through the core (the Spectator HUD does) has
// patched those bytes before this addon loads: the signature then finds 0 hits, pit_lane_system stays 0 and
// practice_like() is false for the whole run, so a practice or qualifying garage is treated like a race (a bent
// suspension is never serviced; log of 2026-11-06 20:34, 'signature has 0 hits, not hooked'). The core's wrap (a mixin
// on the binding of that name, proto void(void *self, uint8_t vehicle, uint32_t status)) stacks with the other
// addons' wraps in any load order. The byte signature stays as the fallback for a core without the binding.
using pit_status_next_fn = uint64_t (*)(void *, uint8_t, uint32_t);  // rax passes through, as the old detour did
pit_status_next_fn pit_status_next;  // the next wrap or the game's function; the core fills it when it installs the wrap
std::atomic<uint32_t> pit_status_calls{0};

// What on_set_vehicle_status passes a change on to: the wrap's next function, in the old signature.
uint64_t pit_status_chain(void *system, uint64_t vehicle, uint64_t status)
{
    return pit_status_next ? pit_status_next(system, uint8_t(vehicle), uint32_t(status)) : 0;
}

uint64_t on_set_vehicle_status(void *system, uint64_t vehicle, uint64_t status)
{
    if (!pit_lane_system.exchange(reinterpret_cast<uintptr_t>(system))) {
        uint8_t flag = 0;
        guarded_read(reinterpret_cast<uintptr_t>(system) + 0x300, flag);
        swofty::log("[td-probe] pit lane system %p, practice/qualifying flag %u", system, unsigned(flag));
    }
    uint8_t id = uint8_t(vehicle);
    uint32_t to = uint32_t(status), from = 0xffffffff;
    uintptr_t entry = 0;
    // Practice/qualifying: a dead car stays InGarage (0.25.1 dropped the crew send-off, but 4 s later the game
    // still set the car OnTrack, and it counted as stopped on track).
    if (car_dead && id < 0x1b && is_player(id) &&
        guarded_read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) && entry &&
        guarded_read(entry + 4, from) && from == 9 && to != 9) {
        if (to == 1) {
            swofty::log("[td-probe] practice/qualifying: your broken car leaves its garage, back in 300 ms unless a "
                        "part was replaced");
            exit_check_at = GetTickCount64() + 300;
            return orig_set_vehicle_status(system, vehicle, status);
        }
        swofty::log("[td-probe] practice/qualifying: your car is broken, refused pit state %u %s", to,
                    label(pit_states, to));
        return 0;
    }
    if (!pushing || id >= 0x1b || !is_player(id) ||
        !guarded_read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) || !entry ||
        !guarded_read(entry + 4, from))
        return orig_set_vehicle_status(system, vehicle, status);
    if (from == 8 && (to == 5 || to == 6 || to == 7 || to == 14)) {
        swofty::log("[td-probe] garage: kept your car BeingPushedIntoGarage, refused pit state %u %s (from %p)", to,
                    label(pit_states, to), __builtin_return_address(0));
        return 0;
    }
    return orig_set_vehicle_status(system, vehicle, status);
}

// The core's wrap of Game_Pitlane_SetVehicleStatus: every pit status change, on the game thread. A refused change
// (on_set_vehicle_status returns without calling the next function) never reaches the game.
uint64_t pit_status_wrap(void *system, uint8_t vehicle, uint32_t status)
{
    pit_status_calls.fetch_add(1, std::memory_order_relaxed);
    return on_set_vehicle_status(system, vehicle, status);
}
const ::swofty::detail::mixin pit_status_mixin{"Game_Pitlane_SetVehicleStatus", SWOFTY_MIXIN_WRAP, 0,
                                               reinterpret_cast<void *>(&pit_status_wrap),
                                               reinterpret_cast<void **>(&pit_status_next)};

// on_load. The wrap when the core has the binding (it installs the wrap before on_load), else the byte signature.
void install_pit_status_hook()
{
    if (swofty::api && swofty::api->resolve && swofty::api->resolve("Game_Pitlane_SetVehicleStatus")) {
        orig_set_vehicle_status = pit_status_chain;
        swofty::log("[td-probe] hooked PitLaneSystem::setVehicleStatus through the core's wrap "
                    "(Game_Pitlane_SetVehicleStatus%s)", pit_status_next ? ", installed" : ", install pending");
        return;
    }
    swofty::log("[td-probe] the core has no Game_Pitlane_SetVehicleStatus binding: hooking the function by its bytes");
    detour("PitLaneSystem::setVehicleStatus", sig_set_vehicle_status, set_vehicle_status_prologue,
           sizeof set_vehicle_status_prologue, reinterpret_cast<void *>(&on_set_vehicle_status),
           reinterpret_cast<void **>(&orig_set_vehicle_status));
}

// Frame thread. Without the pit lane system practice_like() is always false: a practice or qualifying garage is then
// handled like a race (no garage service, a broken car retires and ends the session for everyone). Say so once per
// session when nothing has called the hook 30 s after the player's car is known.
void pit_lane_watch_tick(uint64_t now)
{
    static uint64_t since = 0;
    static bool said = false;
    if (session_ended || player_vehicle.load() < 0) {
        since = 0;
        said = false;  // the next session says it again
        return;
    }
    if (pit_lane_system.load()) {
        since = 0;
        return;
    }
    if (!since)
        since = now;
    if (!said && now - since >= 30000) {
        said = true;
        swofty::log("[td-probe] the pit lane system is still unknown 30 s into the session (hook %s, %u call(s)): "
                    "practice and qualifying garage handling is off until the hook fires",
                    pit_status_next ? "installed" : "not installed", pit_status_calls.load());
    }
}


void post_crew_event(uint32_t request)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!crew_event_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
        return;
    struct {
        const void *vtable;
        uint64_t zero;
        uint8_t vehicle;
        uint8_t pad[3];
        uint32_t request;
        uint8_t tail[0x28];
    } msg{};
    msg.vtable = crew_event_vtable;
    msg.vehicle = uint8_t(vehicle);
    msg.request = request;
    swofty::log("[td-probe] garage: posting MessageTriggerGarageCrewEvent vehicle %d request %u", vehicle, request);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}

void post_crew_msg(uint32_t code)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!crew_msg_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
        return;
    struct {
        const void *vtable;
        uint64_t zero;
        uint8_t vehicle;
        uint8_t pad[3];
        uint32_t code;
        uint8_t tail[0x28];
    } msg{};
    msg.vtable = crew_msg_vtable;
    msg.vehicle = uint8_t(vehicle);
    msg.code = code;
    swofty::log("[td-probe] garage: posting crew report vehicle %d code %u", vehicle, code);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}

void latch_retirement_drive(int vehicle)
{
    if (vehicle<0 || vehicle>=max_vehicles) return;
    retired_drive_lock_vehicle=vehicle;
    throttle_hook.cap=0.0f;
}

// Sess_Result_ChangeVehicleStatus 0x142c7bae0 (result in rcx; u8 vehicle +0, status +0x18, reason +0x19, pending
// request status +0x11d, reason +0x11e, pending flag +0x11f) commits the pending request. At the session end the race
// ruleset (0x142d2b7b0, loop at 0x142d2c26c) requests DNF for every car short of the classification distance, without
// checking for a disqualification, so the player's DSQ became DNF (0.51.64/0.51.65 tests). Keep a disqualified player
// car disqualified: turn that pending DNF back into DSQ.
using change_vehicle_status_fn = uint64_t (*)(void *, double, float, uint8_t, uint32_t, uint32_t);
change_vehicle_status_fn orig_change_vehicle_status = nullptr;
uint64_t on_change_vehicle_status(void *result, double a, float b, uint8_t c, uint32_t d, uint32_t e)
{
    uintptr_t r = reinterpret_cast<uintptr_t>(result);
    uint8_t vehicle = 0xff, status = 0, pending = 0, flag = 0;
    if (r && guarded_read(r, vehicle) && int(vehicle) == player_vehicle.load() &&
        (player_controller_flags.load() & (1u << 2)) && guarded_read(r + 0x18, status) && status == 5 &&
        guarded_read(r + 0x11f, flag) && flag && guarded_read(r + 0x11d, pending) && pending == 4) {
        *reinterpret_cast<volatile uint8_t *>(r + 0x11d) = 5;
        swofty::log("[td-probe] result: kept vehicle %u disqualified (the game asked for DNF)", vehicle);
    }
    return orig_change_vehicle_status(result, a, b, c, d, e);
}
bool hook_change_vehicle_status()
{
    constexpr uint8_t prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x60, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x50,
                                    0xC5, 0xF9, 0x6F, 0xF1};
    return detour("Sess_Result_ChangeVehicleStatus",
                  "56 48 83 EC 60 C5 F8 29 74 24 50 C5 F9 6F F1 48 89 CE 0F B6 41 18 3C 02 75 69 C5 F9 6E 84 24 98 00 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_change_vehicle_status),
                  reinterpret_cast<void **>(&orig_change_vehicle_status));
}

// MessageVehicleStatusRequest {vtable, 0, u8 vehicle, u8 status, u8 reason, u8 apply}: the session result handler
// (0x14e716f20, "onStatusRequest() for Vehicle %d") passes it to Sess_Result_ChangeVehicleStatus 0x142c7bae0, which
// sets result+0x18/+0x19 and posts the status-changed message. Status (0x14565c1f8): 2 active, 3 finished, 4 DNF,
// 5 disqualified, 6 not classified, 7 retired. Reason (0x1456e6a28): 1 retired, 3 terminal damage, 6 black flagged,
// 8 canned event failure. A request for the current status is ignored.
bool post_vehicle_status(int vehicle, uint32_t status, uint32_t reason)
{
    uintptr_t holder = 0, bus = 0;
    if (!status_request_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
        return false;
    struct {
        const void *vtable;
        uint64_t zero;
        uint32_t packed;
        uint8_t pad[0x2c];
    } msg{};
    msg.vtable = status_request_vtable;
    msg.packed = uint32_t(vehicle) | status << 8 | reason << 16 | 1u << 24;
    swofty::log("[td-probe] status request: vehicle %d status %u reason %u (0x%08x)", vehicle, status, reason, msg.packed);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
    return true;
}

// What the pause menu's Retire does, for the player's car.
void retire_player(bool after_dsq = false)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!status_request_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus) {
        swofty::log("[td-probe] retire: not possible (vehicle %d)", vehicle);
        return;
    }
    // A disqualified car stays DSQ; a retirement would overwrite the result with DNF.
    if (!after_dsq && (player_controller_flags.load() & (1u << 2))) {
        swofty::log("[td-probe] retire: skipped, vehicle %d is disqualified", vehicle);
        return;
    }
    struct {
        const void *vtable;
        uint64_t zero;
        uint32_t packed;
        uint8_t pad[0x2c];
    } msg{};
    msg.vtable = status_request_vtable;
    msg.packed = uint32_t(vehicle) | status_retired << 8 | 2u << 16 | 1u << 24;
    latch_retirement_drive(vehicle);
    own_retire_until = GetTickCount64() + 10000;
    // Posting retirement can synchronously deliver NOT_IN_SESSION. Arm the
    // shutdown first; lifecycle cleanup must not restart the DNF car's audio.
    if (engine_off_sound) {
        retired_engine_off_vehicle = vehicle;
        engine_off_vehicle = vehicle;
        swofty::log("[td-probe] engine: keeping vehicle %d off through retirement", vehicle);
    }
    swofty::log("[td-probe] retire: posting MessageVehicleStatusRequest 0x%08x", msg.packed);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}
