// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Scripted failure and the Ctrl+Shift hotkeys.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Debug (Ctrl+Shift+F7): replay a Career scripted failure the way the game does it: its failure planner picks the part
// for your car (on_failure_planner sets game_planned_part), then the scheduler hands the car to the AI
// (0x1409be12c: setController(vehicle, AI, RETIRED) with no retire request from you). With Keep control after a
// scripted failure the addon takes the hand-over over (note_controller_switch); otherwise the AI keeps the car and
// the part reaches terminal damage 30 s later (Flo's case: TERMINAL_DAMAGE followed 31 s after the hand-over).
// 0.51.123 log: without the planned part (a condition since 0.51.63) the AI took the car.
std::atomic<uint64_t> scripted_part_at{0};
void trigger_scripted_failure()
{
    void *sys = controller_system.load();
    int vehicle = player_vehicle.load();
    if (session_ended || !sys || vehicle < 0) {
        swofty::log("[td-probe] scripted failure: no controller system or player vehicle (system %p, vehicle %d)", sys, vehicle);
        return;
    }
    int part = choice_int(debug_failure_part, 0);
    game_planned_part = part >= 0 && part <= 6 ? part : 0;
    swofty::log("[td-probe] scripted failure: the game's planner picks part %d, then its RETIRED hand-over of vehicle %d",
                game_planned_part.load(), vehicle);
    on_set_controller(sys, uint64_t(vehicle), 1, 4);
    if (game_planned_part.load() == -1 && game_failure_part.load() >= 0) {
        swofty::log("[td-probe] scripted failure: the addon kept the car (part %d broken, pit window open)",
                    game_failure_part.load());
        return;
    }
    game_planned_part = -1;
    if (confirmed_retired_vehicle.load() == vehicle) {
        swofty::log("[td-probe] scripted failure: the AI has the car, part failure in 30 s");
        scripted_part_at = GetTickCount64() + 30000;
    } else {
        swofty::log("[td-probe] scripted failure: no hand-over happened");
    }
}
void scripted_failure_tick()
{
    uint64_t at = scripted_part_at.load();
    if (!at || GetTickCount64() < at || !scripted_part_at.compare_exchange_strong(at, 0))
        return;
    swofty::log("[td-probe] scripted failure: part failure now");
    trigger_failure();
}

void poll_hotkey()
{
    // Every hotkey is Ctrl+Shift+<key>. Without both held no setting and no other key is read and every key counts as
    // up (0.51.120: four setting reads and up to 18 key reads a frame before).
    bool held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool debug_on = held && debug_failure_hotkey;
    bool retire_on = held && retire_hotkey;
    auto key_down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Debug: Ctrl+Shift+F9 cycles the player's throttle cap: 0.0 (engine off) -> 0.3 -> automatic.
    static bool cut_was_down;
    bool cut = debug_on && key_down(VK_F9);
    if (cut && !cut_was_down) {
        float t = test_cap.load();
        test_cap = t < 0 ? 0.0f : t < 0.1f ? 0.3f : -1.0f;
        swofty::log("[td-probe] throttle test: cap %.1f (car %p, hooked %d)", double(test_cap.load()),
                    reinterpret_cast<void *>(player_car()), int(throttle_hooked.load()));
    }
    cut_was_down = cut;
    // Debug: Ctrl+Shift+<debug_failure_key> (F10 by default) triggers a failure, once per press.
    static bool failure_was_down, retire_was_down;
    bool down = debug_on && key_down(function_key(debug_failure_key, 10));
    if (down && !failure_was_down)
        trigger_failure();
    failure_was_down = down;
    // Debug: Ctrl+Shift+F4 once starts the test run with you driving, twice quickly with the AI driving; while it
    // runs, it stops it.
    static bool test_was_down;
    bool test_key = debug_on && key_down(VK_F4);
    switch (test_key_action(test_key && !test_was_down, GetTickCount64(), test_run_on.load())) {
    case 1: test_run_start(false); break;
    case 2: test_run_start(true); break;
    case 3: test_run_stop_by_you(); break;
    default: break;
    }
    test_was_down = test_key;
    static bool scripted_was_down;
    // Debug: Ctrl+Shift+F6 registers the next partial failure (the game's temporary faults) at once, cycling the types.
    static bool fault_was_down;
    static int fault_next = 0;
    bool fault_key = debug_on && key_down(VK_F6);
    if (fault_key && !fault_was_down) {
        static const int types[] = {fm::f_slow_shift, fm::f_missing_gear, fm::f_gear_jammed, fm::f_drs,
                                    fm::f_ers_modes,  fm::f_ers_glitch,   fm::f_lean_fuel,   fm::f_rear_brakes,
                                    fm::f_mguk_loss,  fm::f_battery_derate, fm::f_misfire,   -1};
        constexpr int n_types = int(sizeof types / sizeof types[0]);
        int t = types[fault_next++ % n_types];
        if (t < 0) {
            // Last in the cycle: a whole gearbox decline in 2 minutes (both ways in turn), ending in the failure.
            static int path = 0;
            swofty::log("[td-probe] debug fault: gearbox decline requested");
            start_gearbox_decline(120.0, path++ % 2);
        } else if (t == fm::f_rear_brakes) {
            swofty::log("[td-probe] debug fault: rear brake failure requested");
            start_rear_brake_fault(fm::rear_brake_change(uniform01(), uniform01()), 60);
        } else if (fm::power_kind_of_fault(t) != pl::none) {
            swofty::log("[td-probe] debug fault: %s requested (60 s)", fm::fault_name(t));
            start_power_fault(t, 60, true);
        } else {
        fm::FaultSpec spec = fm::fault_spec(t);
        double duration = spec.seconds ? 60.0 : 3.0;  // short, so a test sees the "fixed" radio too
        // Missing gear: 3rd (index 4), used in most corners. Slow shifts: a random severity like the real ones.
        fm::ShiftSeverity sev = fm::slow_shift_severity(uniform01(), uniform01());
        fault_request = uint64_t(t) | (uint64_t(duration * 10.0) << 8) | (uint64_t(spec.seconds ? 1 : 0) << 40) |
                        (uint64_t(t == fm::f_missing_gear ? 4 : 0xff) << 48) |
                        (uint64_t(t == fm::f_slow_shift ? sev.scale : 0) << 56);
        if (t == fm::f_slow_shift)
            swofty::log("[td-probe] debug fault: %s requested (%s, x%d)", fm::fault_name(t), sev.name, sev.scale);
        else
            swofty::log("[td-probe] debug fault: %s requested", fm::fault_name(t));
        }
    }
    fault_was_down = fault_key;
    bool scripted = debug_on && key_down(VK_F7);
    if (scripted && !scripted_was_down)
        trigger_scripted_failure();
    scripted_was_down = scripted;
    scripted_failure_tick();
    down = retire_on && key_down(function_key(retire_key, 11));
    if (down && !retire_was_down)
        request_retire("retire hotkey");
    retire_was_down = down;
}
