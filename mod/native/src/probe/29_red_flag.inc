// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Red flag strategy.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Red flag: the game's RedFlagRaceStrategy state (Enter 0x142dfbf80, Update 0x142dfcdc0, `this` in rcx) shows the
// pit stop editor where the team repairs and changes tyres. The close callback 0x142dfd2b0 sets state+0xdb1, which
// Update tests. What the team may do follows the FIA Sporting Regulations, B5.14.4 a: during a suspension only the
// listed work is allowed. Item viii is the repair of genuine accident damage including the replacement of
// assemblies; power unit or gearbox work is not listed. So:
//  - mechanical damage (mask bits 0-2): popup, no editor, retire.
//  - lost wheel / broken suspension (bits 3-4): the car limps on only with one broken corner (decide_limp, a
//    dice roll). A car that could not be driven was abandoned on the circuit (B1.8.9): popup, no editor, retire.
//    A limping car is accident damage the team may repair: a second dice roll (80 %, lost wheel 50 %) decides whether
//    it is done in time. Yes: the editor runs; when it closes, the addon rebuilds the corner (and the front wing) the
//    way the game's multiplayer damage sync does (repair_crash_damage). No: popup, no editor, retire.
//  - other accident damage (tyres, aero, bodywork): the editor runs and the game's own red flag repair (loop at
//    0x1435847da, vehicle status 3 "Repaired") restores it.
// Afterwards the addon reads the game's terminal damage record (read_terminal_record): no longer terminal, the car
// takes part in the restart; still terminal 20 s after the editor closed, it retires.
// (The red_flag_* state variables are declared in 02_core_state.inc, next to red_flag_repair_wait.)
void (*orig_red_flag_strategy_enter)(void *) = nullptr;
void red_flag_toast_tick()
{
    uint8_t closed = 0;
    uintptr_t st = red_flag_state.load();
    if (red_flag_screen_open.load() && st && guarded_read(st + 0xdb1, closed) && closed &&
        red_flag_screen_open.exchange(false)) {
        swofty::log("[td-probe] red flag: strategy screen closed, retiring your car");
        red_flag_toast_at = GetTickCount64() + 3000;
        retire_player();
    }
    if (red_flag_repaired_toast.exchange(false) && swofty::hud::available())
        swofty::hud::toast("CAR REPAIRED", "Your team repaired the accident damage. You take part in the restart.", 8);
    if (red_flag_repair_wait.load()) {
        uint64_t now = GetTickCount64();
        // The editor closed: the team works now. Give the repairs 20 s.
        if (st && guarded_read(st + 0xdb1, closed) && closed && red_flag_repair_deadline.load() > now + 20000) {
            red_flag_repair_deadline = now + 20000;
            if (red_flag_fix_pending.exchange(false))
                red_flag_fix_now = true;
            swofty::log("[td-probe] red flag: strategy screen closed, waiting for the repairs");
        }
        if (confirmed_retired_vehicle.load() >= 0) {
            red_flag_repair_wait = false;
        } else if (now >= red_flag_repair_deadline.load() && red_flag_repair_wait.exchange(false)) {
            swofty::log("[td-probe] red flag: still terminal after the repairs (state %d, parts 0x%x), retiring your car",
                        live_terminal_state.load(), live_terminal_mask.load());
            retire_player();
            if (swofty::hud::available())
                swofty::hud::toast("REPAIRS NOT COMPLETED",
                                   "The damage could not be repaired before the restart. You will not take part.", 10);
        }
    }
    uint64_t at = red_flag_toast_at.load();
    if (!at || GetTickCount64() < at)
        return;
    red_flag_toast_at = 0;
    const char *title = "CAR SEVERELY DAMAGED";
    const char *text = "Your car was severely damaged and cannot be repaired. You will not take part in the restart.";
    if (red_flag_verdict.load() == 1) {
        title = "POWER UNIT FAILURE";
        text = "No power unit or gearbox work is allowed during a red flag, so your team withdraws the car. "
               "You will not take part in the restart.";
    } else if (red_flag_verdict.load() == 2) {
        text = "Your team cannot rebuild the broken corner before the restart. You will not take part in the restart.";
    } else if (red_flag_verdict.load() == 3) {
        title = "CAR ABANDONED ON TRACK";
        text = "Your car could not be driven on. A car abandoned on the circuit is withdrawn from the race.";
    }
    if (red_flag_screen_open.load() && swofty::popup::available()) {
        auto r = swofty::popup::show(title, text);
        swofty::log("[td-probe] red flag: popup %s", swofty::popup::name(r));
        if (r == swofty::popup::Result::ok) {
            // Skip the pit stop editor: what its close callback (0x142dfd2b0) does. The next tick retires the car.
            uintptr_t st = red_flag_state.load();
            if (st && swofty::mem::readable(st + 0xdb1, 1)) {
                *reinterpret_cast<volatile uint8_t *>(st + 0xdb1) = 1;
                swofty::log("[td-probe] red flag: pit stop editor skipped");
            }
            return;
        }
    }
    if (swofty::hud::available())
        swofty::hud::toast(title, text, 10);
}
void on_red_flag_strategy_enter(void *state)
{
    // If the game enters this state again and again, stay out of the way: no logging, no probing.
    static uint64_t last_enter = 0;
    uint64_t now_enter = GetTickCount64();
    bool repeat = now_enter - last_enter < 1000;
    last_enter = now_enter;
    if (repeat) {
        orig_red_flag_strategy_enter(state);
        return;
    }
    // The game sends its terminal damage message only after a delay: a crash just before the red flag is already in
    // the record but not yet known to the addon. Take it from the record.
    if (keep_control && !player_broken && live_terminal_state.load() == 5 && live_terminal_mask.load()) {
        terminal_mask.fetch_or(live_terminal_mask.load());
        player_broken = true;
        decide_limp(throttle_hook.car);
        swofty::log("[td-probe] red flag: terminal damage 0x%x from the game's record (message still pending)",
                    live_terminal_mask.load());
    }
    uint32_t mask = terminal_mask.load();
    int corners = damaged_corners(throttle_hook.car);
    swofty::log("[td-probe] red flag: strategy screen opens (player vehicle %d, broken %d, terminal mask 0x%x, "
                "game record state %d parts 0x%x, damaged corners %d, pit state at the suspension %d)",
                player_vehicle.load(), int(player_broken.load()), mask, live_terminal_state.load(),
                live_terminal_mask.load(), corners, pit_at_suspension.load());
    orig_red_flag_strategy_enter(state);
    red_flag_state = reinterpret_cast<uintptr_t>(state);
    if (!red_flag_retire || !keep_control || !player_broken || !mask || confirmed_retired_vehicle.load() >= 0)
        return;
    if (retire_in.load() >= 0) {
        swofty::log("[td-probe] red flag: pending retirement timer %.1f s dropped, the red flag rules decide",
                    retire_in.load());
        retire_in = -1.0;
    }
    bool corner_damage = mask & 0x18;
    int verdict = 0;
    unsigned chance = 0, roll = 0;
    if (mask & 0x7) {
        verdict = 1;
    } else if (corner_damage && limp_state.load() != 1) {
        verdict = 3;  // could not be driven: abandoned on the circuit
    } else if (corner_damage) {
        // The car crawled on: whether the team can rebuild the corner in time is a dice roll; a lost wheel is harder.
        chance = ((crash_corners.load() & 0xfff) && corners == 1 && crash_repair_available())
                     ? (has_detached_corner(crash_corners.load()) ? 50 : 80) : 0;
        roll = forced_roll(debug_repair_roll, chance, roll_percent());
        if (roll >= chance)
            verdict = 2;
        swofty::log("[td-probe] red flag: corner repair chance %u%%, roll %u -> %s", chance, roll,
                    verdict ? "not repairable in time" : "the team repairs it");
    }
    if (verdict) {
        if (red_flag_screen_open.exchange(true))
            return;
        red_flag_verdict = verdict;
        swofty::log("[td-probe] red flag: %s; your car retires when you close the screen",
                    verdict == 1 ? "mechanical failure, not repairable under a red flag (FIA B5.14.4 a)" :
                    verdict == 2 ? "broken corner, the team cannot repair it before the restart" :
                                   "broken corner, the car could not be driven: abandoned on the circuit (FIA B1.8.9)");
        red_flag_toast_at = GetTickCount64() + 1500;
        return;
    }
    // Accident damage the team may repair (FIA B5.14.4 a viii): the editor runs as usual. Aero or body damage only
    // counts wherever the car was: it keeps its drive since 0.51.126 (it used to roll out, and on track at the
    // suspension it was taken as abandoned).
    red_flag_fix_pending = corner_damage || (mask & 0x40);
    red_flag_repair_deadline = GetTickCount64() + 600000;
    red_flag_repair_wait = true;
    swofty::log("[td-probe] red flag: accident damage 0x%x may be repaired (%s); waiting for the editor to close", mask,
                red_flag_fix_pending.load() ? "the addon replaces the corner / front wing" : "the game's repairs");
}
bool hook_red_flag_strategy()
{
    constexpr uint8_t enter_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                          0x55, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0xCE};
    bool ok = detour("RedFlagRaceStrategy::enter",
                     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 48 89 CE C6 81 B2 0D 00 00 01 80 B9 B0 0D 00 00 00",
                     enter_prologue, sizeof enter_prologue, reinterpret_cast<void *>(&on_red_flag_strategy_enter),
                     reinterpret_cast<void **>(&orig_red_flag_strategy_enter));
    return ok;
}
