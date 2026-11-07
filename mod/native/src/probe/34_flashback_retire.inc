// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Flashback check, retire request, retire-local-player hook.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// No engine, power unit or gearbox part at failure (1.0). Just before the game's own failure the failing part reads
// 0.99 (record state 4), so the old 0.9 bound kept the lock after such a flashback (0.51.82 test).
bool no_failed_part(uintptr_t car)
{
    uintptr_t engine = 0, gearbox = 0;
    bool ok = car && guarded_read(car + 0x530, engine) && engine && guarded_read(car + 0x538, gearbox) && gearbox;
    for (int i = 0; ok && i < 7; ++i) {
        float wear = 1;
        ok = guarded_read(i < 6 ? engine + part_offsets[i] : gearbox + 0x674, wear) && std::isfinite(wear) &&
             wear >= 0 && wear < 1.0f;
    }
    return ok;
}

// The player's pit state as the PitLaneSystem has it now ([system + v*16 + 0x68] -> +4, as in setVehicleStatus), or
// -1. A flashback rewinds it; whether the game posts MessagePitStateChanged for that is not known, so player_pit is
// read again after one (reset_car_state would otherwise leave it at OnTrack).
int live_pit_state(int vehicle)
{
    uintptr_t sys = pit_lane_system.load(), entry = 0;
    uint32_t state = 0xffffffff;
    if (!sys || vehicle < 0 || vehicle >= 0x1b || !guarded_read(sys + 0x68 + uintptr_t(vehicle) * 16, entry) ||
        !entry || !guarded_read(entry + 4, state) || state >= 16)
        return -1;
    return int(state);
}
void resync_pit_after_flashback()
{
    int pit = live_pit_state(player_vehicle.load());
    if (pit < 0 || pit == player_pit.load())
        return;
    swofty::log("[td-probe] flashback: pit state %d -> %d %s (rewound)", player_pit.load(), pit, label(pit_states, pit));
    player_pit = pit;
}

// Game thread, after the terminal record was read. For 2 s after a flashback (counted from the first update after it:
// the game stays paused about 1.6 s after the message, and the rewound record shows on that first update):
//  - a retirement request without damage is cancelled (the flashback undoes it; request again if wanted). Only a real
//    request: a failure the addon applied whose terminal message is still pending also has no mask yet;
//  - a record that is no longer terminal and no failed part: the flashback went back before the damage, the car is
//    yours again with full power. This also undoes a retirement: the game lets you flash back after a retirement and
//    races the car again (0.51.82 test). A failure the game scheduled comes back, so its next RETIRED hand-over is
//    dropped again and the part breaks again, as the first time;
//  - still terminal after 2 s: the damage stays, the retirement starts over from the rewound position (no stale
//    standstill timer, crew, push or garage work). Damage the rewind removed leaves the mask.
// A withdrawn or red-flagged car is left alone.
void flashback_check(uintptr_t car)
{
    static uint64_t window = 0;
    uint64_t now = GetTickCount64();
    if (flashback_used_at.exchange(0))
        window = now + 2000;
    if (!window)
        return;
    constexpr uint32_t suspended = (1u << 3) | (1u << 10) | (1u << 11);
    bool retired = confirmed_retired_vehicle.load() >= 0 || retire_in.load() <= -2.0;
    if (!keep_control || (!player_broken && !retired) || car_dead || session_ended || red_flag_screen_open.load() ||
        red_flag_repair_wait.load() || (player_controller_flags.load() & suspended)) {
        window = 0;
        return;
    }
    uint32_t mask = terminal_mask.load();
    int state = live_terminal_state.load();
    uint32_t live = live_terminal_mask.load();
    if (retire_requested && !mask && !retired) {
        window = 0;
        reset_car_state();
        resync_pit_after_flashback();
        swofty::log("[td-probe] flashback: your retirement request is cancelled");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT CANCELLED", "Flashback used", 3);
        return;
    }
    if (state != 5 && !live && no_failed_part(car)) {
        window = 0;
        reset_car_state();
        resync_pit_after_flashback();
        player_controller_flags.fetch_and(~(1u << 4));
        own_retire_until = 0;
        status7_at = 0;
        game_failure_deadline = 0;
        game_failure_lap = -1;
        dsq_pending = false;
        int part = game_failure_part.exchange(-1);
        if (part >= 0)
            game_planned_part = part;
        swofty::log("[td-probe] flashback: back before the damage (game record state %d%s), your car is whole again%s",
                    state, retired ? ", retirement undone" : "",
                    part >= 0 ? "; the game's failure may come back and is handled the same way" : "");
        return;
    }
    if (now < window)
        return;
    window = 0;
    if (retired) {
        swofty::log("[td-probe] flashback: your car is retired and still broken (record state %d, parts 0x%x)", state,
                    live);
        return;
    }
    if (state == 5 && live && (live & mask) == live && live != mask) {
        terminal_mask = live;
        if (!(live & 0x18)) {
            limp_state = -1;
            limp_corner = -1;
            crash_corners = 0;
        }
        swofty::log("[td-probe] flashback: damage 0x%x -> 0x%x (the rewind removed part of it)", mask, live);
    }
    cancel_crew_work();
    retire_in = -1.0;
    still_for = 0;
    engine_off_standing = false;
    retired_engine_off_vehicle = -1;
    pit_clearance_hold_vehicle = -1;
    garage_return_wanted = false;
    exit_check_at = 0;
    resync_pit_after_flashback();
    if (limp_state.load() == 1 && race_stop_left.load() > 0)
        race_stop_left = race_stop_grace;
    swofty::log("[td-probe] flashback: after the damage (record state %d, parts 0x%x); still broken, retirement starts "
                "over (pit state %d)", state, live, player_pit.load());
    // Rewound into the box or the push: no new box arrival starts the push again, so the car retires in its box.
    if (int pit = player_pit.load(); !practice_like() && pit >= 4 && pit <= 8) {
        retire_in = 2.0;
        swofty::log("[td-probe] flashback: rewound inside the pit box (pit state %d), the car retires there", pit);
    }
}

uintptr_t player_car()
{
    return throttle_hook.car;
}


// Retire request (hotkey, pause menu or radio): nothing breaks and power stays. You pull over
// and stand still (retire after 10 s) or drive into your box (crew pushes it in). A second request cancels it
// while you are still on track and no real failure or retirement is under way.
void request_retire(const char *source)
{
    if (player_vehicle.load() < 0) {
        swofty::log("[td-probe] %s: your car is not known yet", source);
    } else if (retire_requested && player_broken && player_pit.load() == 1 && retire_in.load() == -1.0 &&
               !terminal_mask.load() && throttle_hook.cap >= 1.0f) {
        retire_requested = false;
        player_broken = false;
        flashback_tracker.clear_lock();
        swofty::log("[td-probe] %s: retirement cancelled", source);
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT CANCELLED", {}, 3);
    } else if (!player_broken && retire_in.load() > -2.0 && player_pit.load() == 1) {
        retire_requested = true;
        flashback_tracker.mark(GetTickCount64());
        player_broken = true;
        swofty::log("[td-probe] %s: pull over (retire after 10 s standing) or drive into your box", source);
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
    } else {
        swofty::log("[td-probe] %s: ignored (broken %d, pit state %d)", source, int(player_broken.load()),
                    player_pit.load());
    }
}

// Pause menu Retire: Game::retireLocalPlayer (0x142cea5b0, `push rsi; push rdi; sub rsp, 0x28; mov edi, edx`, then
// setLocalPlayersStatuses(7)). On track it becomes a retire request instead: nothing breaks, you pull over or drive
// into your box. Choosing Retire again changes nothing. In practice and qualifying the same call is End Session: the
// game ends it (a friend's log 2026-11-07: End Session became a retire request and the session went on).
constexpr const char *sig_retire_local_player =
    "56 57 48 83 EC 28 89 D7 48 89 CE 80 3D ?? ?? ?? ?? 01 75 ?? B8 5F 01 00 00";

// The pause menu's thread. True swallows the game's retirement.
extern "C" bool on_retire_local_player(void *, uint32_t reason)
{
    if (!keep_control || !pause_menu_retire || player_vehicle.load() < 0 || player_pit.load() != 1 ||
        retire_in.load() != -1.0 || practice_like()) {
        swofty::log("[td-probe] pause menu retire (reason %u): the game %s", reason,
                    practice_like() ? "ends your practice or qualifying session" : "retires your car");
        own_retire_until = GetTickCount64() + 10000;
        return false;
    }
    if (player_controller_flags.load() & (1u << 2)) {
        // Disqualified: the AI holds the car. Ask again for the disqualified result, which ends your session; a
        // retirement would turn the DSQ into a DNF.
        swofty::log("[td-probe] pause menu retire: car is disqualified, requesting the DSQ result again");
        post_vehicle_status(player_vehicle.load(), 5, 6);
        return true;
    }
    if (player_broken) {
        // Already retiring: no shortcut, you still pull over or drive into your box.
        swofty::log("[td-probe] pause menu retire again: still pull over or box");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
        return true;
    }
    request_retire("pause menu retire");
    return true;
}

void hook_retire_local_player()
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", sig_retire_local_player, &at);
    uint8_t *stub = n == 1 && at ? alloc_near(at) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] pause menu retire: %d hits or no stub memory, Retire stays the game's", n);
        return;
    }
    emitter e{stub};
    e.bytes({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});    // push rcx; push rdx; push r8; push r9
    e.bytes({0x48, 0x83, 0xEC, 0x28});                // sub rsp, 0x28
    e.call(reinterpret_cast<uintptr_t>(&on_retire_local_player));
    e.bytes({0x48, 0x83, 0xC4, 0x28});                // add rsp, 0x28
    e.bytes({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});    // pop r9; pop r8; pop rdx; pop rcx
    e.bytes({0x84, 0xC0, 0x74, 0x01, 0xC3});          // test al, al; jz +1; ret (swallowed)
    e.bytes({0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xD7}); // the game's prologue
    e.jmp(at + 8);
    bool ok = jump_to_stub(at, 8, stub, e.p);
    swofty::log("[td-probe] pause menu retire: %s Game::retireLocalPlayer at %p", ok ? "hooked" : "could not hook",
                reinterpret_cast<void *>(at));
}
