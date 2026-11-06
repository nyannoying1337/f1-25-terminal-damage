// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The Ctrl+Shift+F4 test run.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Test run steps: a baseline, every partial failure for 45 s (seconds, so each ends with the "fixed" radio), then a
// 90 s gearbox decline that ends in the gearbox failure. Each step logs the shifts it saw.
struct test_step {
    const char *name;
    int fault;       // fm::FaultType, 0 = none, -1 = gearbox decline, -2 radio lines
    int gear;        // missing gear index (4 = 3rd), 0xff none
    int magnitude;   // slow shifts: shift time scale; rear brakes: % lost (< 0 gained)
    double seconds;  // how long the step runs
};
constexpr test_step test_steps[] = {
    {"baseline", 0, 0xff, 0, 40},
    {"slow shifts x8", fm::f_slow_shift, 0xff, 8, 80},
    {"missing 3rd gear", fm::f_missing_gear, 4, 0, 80},
    {"gear jammed", fm::f_gear_jammed, 0xff, 0, 80},
    {"DRS failure", fm::f_drs, 0xff, 0, 80},
    {"ERS modes locked", fm::f_ers_modes, 0xff, 0, 80},
    {"ERS glitch", fm::f_ers_glitch, 0xff, 0, 80},
    {"lean fuel", fm::f_lean_fuel, 0xff, 0, 80},
    {"rear brakes weak", fm::f_rear_brakes, 0xff, 60, 60},
    {"rear brakes grabbing", fm::f_rear_brakes, 0xff, -80, 60},
    {"MGU-K lost", fm::f_mguk_loss, 0xff, 0, 50},
    {"battery derate", fm::f_battery_derate, 0xff, 0, 50},
    {"misfire", fm::f_misfire, 0xff, 0, 40},
    {"radio lines", -2, 0xff, 0, 7.0 * int(n_radio_lines) + 5},
    {"gearbox decline", -1, 0xff, 0, 120},
};
constexpr int n_test_steps = int(sizeof test_steps / sizeof test_steps[0]);
// When you drive (about 5 min): what changed in 0.51.114 and what you can feel. The log compares each power step's
// full-throttle acceleration with the steps without a power fault. The grabbing rear brakes come late: they spun
// the car into the wall in the 0.51.114 test, before the weak brakes and the gearbox decline had run.
constexpr test_step drive_steps[] = {
    {"baseline", 0, 0xff, 0, 30},
    {"MGU-K lost", fm::f_mguk_loss, 0xff, 0, 40},
    {"battery derate", fm::f_battery_derate, 0xff, 0, 40},
    {"misfire", fm::f_misfire, 0xff, 0, 30},
    {"rear brakes weak", fm::f_rear_brakes, 0xff, 60, 40},
    {"rear brakes grabbing", fm::f_rear_brakes, 0xff, -80, 40},
    {"gearbox decline", -1, 0xff, 0, 60},
};
constexpr int n_drive_steps = int(sizeof drive_steps / sizeof drive_steps[0]);
struct test_run_state {
    bool active = false;
    bool ai = false;      // the AI drives (else you do)
    bool paused = false;  // the car is out: a flashback to before the damage continues the run
    bool decline_over = false;  // the gearbox decline step: the decline failed, the game's breakdown comes
    int step = -1, reason_try = 0;
    double clock = 0, step_clock = 0;
    double speed_sum = 0, speed_time = 0;  // km/h x s this step, for the average
    int top_speed = 0;
    int vehicle = -1;
};
test_run_state test_run;
constexpr int32_t test_reasons[] = {14, 7};  // ROBODRIVER, BENCHMARK

// Ctrl+Shift+F4: one press starts the run with you driving once no second press came within test_double_ms, two
// presses start it with the AI driving, a press while it runs stops it. Returns 0 nothing, 1 start (you drive),
// 2 start (AI drives), 3 stop.
constexpr uint64_t test_double_ms = 600;
uint64_t test_key_pending_at = 0;
int test_key_action(bool edge, uint64_t now, bool running)
{
    if (edge) {
        if (running) {
            test_key_pending_at = 0;
            return 3;
        }
        if (test_key_pending_at && now - test_key_pending_at <= test_double_ms) {
            test_key_pending_at = 0;
            return 2;
        }
        test_key_pending_at = now;
        return 0;
    }
    if (test_key_pending_at && now - test_key_pending_at > test_double_ms) {
        test_key_pending_at = 0;
        return 1;
    }
    return 0;
}
void test_run_reset()
{
    if (test_run.active) {
        swofty::log("[td-probe] test run: stopped (a new session)");
        power_summary_due = true;
    }
    test_run = {};
    test_run_on = false;
}

void test_run_log_shifts(const char *step, bool power_step = false)
{
    char gears[96] = {};
    int at = 0;
    for (int g = 0; g < 12 && at < int(sizeof gears) - 1; ++g)
        if (uint32_t n = test_shifts.gear_seen[g].load()) {
            // snprintf returns the length it WOULD have written: stop at the end of the buffer, never past it.
            int wrote = snprintf(gears + at, sizeof gears - at, " [%d]=%u", g, n);
            at += wrote < 0 ? 0 : std::min(wrote, int(sizeof gears) - 1 - at);
        }
    uint32_t nu = test_shifts.count[0].load(), nd = test_shifts.count[1].load();
    swofty::log("[td-probe] test run: %s: %u upshifts avg %.0f ms max %.0f ms, %u downshifts avg %.0f ms max %.0f ms, "
                "shifts ended in gear index%s, speed avg %.0f top %d now %d km/h",
                step, nu, nu ? test_shifts.total_us[0].load() / 1000.0 / nu : 0.0, test_shifts.max_us[0].load() / 1000.0,
                nd, nd ? test_shifts.total_us[1].load() / 1000.0 / nd : 0.0, test_shifts.max_us[1].load() / 1000.0,
                at ? gears : " (none)", test_run.speed_time > 0 ? test_run.speed_sum / test_run.speed_time : 0.0,
                test_run.top_speed, game_speed.load());
    test_shifts.reset = true;
    brake_summary_power = power_step;
    brake_summary_step = step;
    test_run.speed_sum = test_run.speed_time = 0;
    test_run.top_speed = 0;
}

// The AI takes the car (on) or gives it back. Only the ROBODRIVER/BENCHMARK flag: a RETIRED or other AI reason the
// game set stays.
void test_run_hand_to_ai(bool on)
{
    void *sys = controller_system.load();
    if (test_run.ai && sys && !session_ended && test_run.vehicle >= 0 && test_run.vehicle == player_vehicle.load())
        orig_set_controller(sys, uint64_t(test_run.vehicle), on ? 1 : 0, test_reasons[test_run.reason_try]);
}

// The effects the test started end with it: its power fault, a timed rear brake fault, its gearbox decline.
void test_run_end_effects(const char *why)
{
    if (power_fault_test)
        end_power_fault(why);
    if (rear_brake_until.load()) {  // a timed one is the test's
        rear_brake_loss_target = 0.0f;
        rear_brake_until = 0;
    }
    if (gearbox_decline.test) {
        swofty::log("[td-probe] gearbox decline: ended with the test run");
        fault_request = 0;  // its slow-shift record, if the game thread has not registered it yet
        gearbox_decline = {};
        decline_shift_scale = 0.0f;
        shown_decline_stage = -1;
    }
}

void test_run_stop(const char *why)
{
    if (!test_run.active)
        return;
    swofty::log("[td-probe] test run: stopped (%s)", why);
    if (!test_run.paused)  // a paused run gave the car back already
        test_run_hand_to_ai(false);
    test_run = {};
    test_run_on = false;
    power_summary_due = true;
    test_run_end_effects("test run over");
    radio_pending = 0;  // 0.51.112 log: the radio lines step kept talking after the run stopped
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", why, 4);
}

// Ctrl+Shift+F4 while the run goes: the step so far is logged (shifts, brakes, power) before it stops.
void test_run_stop_by_you()
{
    const test_run_state &t = test_run;
    const test_step *steps = t.ai ? test_steps : drive_steps;
    int n = t.ai ? n_test_steps : n_drive_steps;
    if (t.active && !t.paused && t.step >= 0 && t.step < n)
        test_run_log_shifts(steps[t.step].name, fm::power_kind_of_fault(steps[t.step].fault) != pl::none);
    test_run_stop("stopped by you");
}

void test_run_start(bool ai)
{
    void *sys = controller_system.load();
    int v = player_vehicle.load();
    if (session_ended || !sys || v < 0 || player_pit.load() != 1) {
        swofty::log("[td-probe] test run: needs a running session with your car on track (system %p, vehicle %d, "
                    "pit state %d)", sys, v, player_pit.load());
        return;
    }
    test_run = {};
    test_run.active = true;
    test_run.ai = ai;
    test_run.vehicle = v;
    test_shifts.reset = true;
    brake_tel_reset = true;
    test_run_on = true;
    if (!ai) {
        // You drive: the steps begin at once.
        test_run.step = 0;
        swofty::log("[td-probe] test run: started, you drive vehicle %d", v);
        if (swofty::hud::available())
            swofty::hud::toast("TEST RUN", "You drive: MGU-K lost, battery derate, misfire, weak and grabbing rear "
                                           "brakes, gearbox decline (about 5 min). Flat out on the straights. "
                                           "Ctrl+Shift+F4 stops.",
                               6);
        return;
    }
    swofty::log("[td-probe] test run: started, handing vehicle %d to the AI (%s)", v, label(reasons, test_reasons[0]));
    orig_set_controller(sys, uint64_t(v), 1, test_reasons[0]);
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", "The AI drives, the addon runs every fault (about 12 min)", 5);
}

// Start step t.step.
void test_run_begin_step(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    t.step_clock = 0;
    t.decline_over = false;
    const test_step &n = steps[t.step];
    swofty::log("[td-probe] test run: step %d of %d: %s", t.step, n_steps - 1, n.name);
    if (n.fault == -1) {
        // The step waits for the car to stop (test_run_tick).
        start_gearbox_decline(std::max(30.0, 0.75 * n.seconds), fm::decline_hydraulics);
        gearbox_decline.test = true;
    } else if (n.fault == fm::f_rear_brakes) {
        start_rear_brake_fault(n.magnitude / 100.0, n.seconds - 5);
    } else if (fm::power_kind_of_fault(n.fault) != pl::none) {
        start_power_fault(n.fault, n.seconds - 2, true);
    } else if (n.fault == -2) {
        // Every engineer line in turn (6 s apart on the game thread), so the log names what you hear.
        for (int i = 0; i < n_radio_lines; ++i)
            radio_say(i, true);
    } else if (n.fault > 0) {
        // 45 s, counted in seconds (record +0x1d), started at once.
        fault_request = uint64_t(n.fault) | (uint64_t(450) << 8) | (uint64_t(1) << 40) | (uint64_t(n.gear) << 48) |
                        (uint64_t(n.magnitude) << 56);
    }
}

// The car is out (a crash or a failure): the run waits, its step's effects end, the AI gives the car back. The
// gearbox decline as the last step ends in the planned failure: the run is done.
void test_run_pause(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    const test_step *s = t.step >= 0 && t.step < n_steps ? &steps[t.step] : nullptr;
    test_run_log_shifts(s ? s->name : "last step", s && fm::power_kind_of_fault(s->fault) != pl::none);
    if (s && s->fault == -1 && t.step == n_steps - 1) {
        test_run_stop("all steps done, the gearbox failed as planned");
        return;
    }
    if (t.step >= n_steps) {
        test_run_stop("the car is out, all steps were done");
        return;
    }
    if (confirmed_retired_vehicle.load() >= 0) {
        test_run_stop("the car retired");
        return;
    }
    test_run_end_effects("the car is out");
    test_run_hand_to_ai(false);
    t.paused = true;
    swofty::log("[td-probe] test run: paused in %s, the car is out; a flashback to before the damage continues the "
                "run", s ? s->name : "the wait for the AI");
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", "Paused: the car is out. Flash back to before the damage to continue.", 5);
}

// The car is whole again (a flashback to before the damage, or a repair in practice): the run goes on with the next
// step, or the same step again if less than half of it ran.
void test_run_resume(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    t.paused = false;
    test_run_hand_to_ai(true);
    if (t.step < 0) {
        t.clock = 0;
        swofty::log("[td-probe] test run: continues, the car is whole again; waiting for the AI to drive");
        return;
    }
    if (t.step >= n_steps) {
        swofty::log("[td-probe] test run: continues, the car is whole again (all steps done, the AI keeps driving)");
        return;
    }
    bool again = t.step_clock < steps[t.step].seconds / 2;
    if (!again && ++t.step >= n_steps) {
        test_run_stop("all steps done");
        return;
    }
    swofty::log("[td-probe] test run: continues, the car is whole again (flashback or repair), with %s",
                again ? "the same step" : "the next step");
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", steps[t.step].name, 4);
    test_run_begin_step(steps, n_steps);
}

void test_run_tick(double dt)
{
    test_run_state &t = test_run;
    bool restarted = test_run_restarted.exchange(false);
    if (!t.active)
        return;
    if (restarted) {
        test_run_stop("the race restarted");
        return;
    }
    if (session_ended || player_vehicle.load() != t.vehicle) {
        test_run_stop("session ended");
        return;
    }
    const test_step *steps = t.ai ? test_steps : drive_steps;
    int n_steps = t.ai ? n_test_steps : n_drive_steps;
    bool out = player_broken || confirmed_retired_vehicle.load() >= 0;
    if (t.paused) {
        if (confirmed_retired_vehicle.load() >= 0)
            test_run_stop("the car retired");
        else if (!out && player_pit.load() == 1)
            test_run_resume(steps, n_steps);
        return;
    }
    if (out) {
        test_run_pause(steps, n_steps);
        return;
    }
    t.clock += dt;
    t.step_clock += dt;
    if (int v = game_speed.load(); v >= 0) {
        t.speed_sum += v * dt;
        t.speed_time += dt;
        t.top_speed = std::max(t.top_speed, v);
    }
    // The AI must move the car: after 15 s try the next controller reason.
    if (t.step < 0) {
        if (t.clock < 15)
            return;
        if (game_speed.load() < 20) {
            void *sys = controller_system.load();
            if (sys && t.reason_try + 1 < int(sizeof test_reasons / sizeof test_reasons[0])) {
                orig_set_controller(sys, uint64_t(t.vehicle), 0, test_reasons[t.reason_try]);
                ++t.reason_try;
                swofty::log("[td-probe] test run: the car stands (%d km/h), trying %s", game_speed.load(),
                            label(reasons, test_reasons[t.reason_try]));
                orig_set_controller(sys, uint64_t(t.vehicle), 1, test_reasons[t.reason_try]);
                t.clock = 0;
                return;
            }
            test_run_stop("the AI does not drive the car");
            return;
        }
        swofty::log("[td-probe] test run: the AI drives (%d km/h with %s)", game_speed.load(),
                    label(reasons, test_reasons[t.reason_try]));
        t.step = 0;
        t.step_clock = 0;
        test_shifts.reset = true;
        return;
    }
    if (t.step >= n_steps)
        return;  // done: the AI keeps the car until Ctrl+Shift+F4 (no hand-back at full speed)
    const test_step &s = steps[t.step];
    if (s.fault == -1 && gearbox_decline.path < 0 && !t.decline_over) {
        t.decline_over = true;
        swofty::log("[td-probe] test run: the gearbox decline is over, the game's breakdown stops the car (about 40 s)");
    }
    if (t.step_clock < s.seconds)
        return;
    // The gearbox decline step ends when the car stops (test_run_pause: the planned end), at most after three times its
    // time: nursing slows the decline's clock, and the game's breakdown stops the car about 40 s after the decline
    // (0.51.115 log: failed at 08:00:05, planned by the game at 08:00:35, the car out at 08:00:43; the step had ended
    // the run at 08:00:18).
    if (s.fault == -1 && t.step_clock < 3 * s.seconds)
        return;
    test_run_log_shifts(s.name, fm::power_kind_of_fault(s.fault) != pl::none);
    // A step's own effects end with it.
    if (power_fault_test && fm::power_kind_of_fault(s.fault) != pl::none)
        end_power_fault("test step over");
    if (++t.step >= n_steps) {
        if (!t.ai) {
            test_run_stop("all steps done");
            return;
        }
        swofty::log("[td-probe] test run: all steps done, the AI keeps driving (Ctrl+Shift+F4 takes the car back)");
        if (swofty::hud::available())
            swofty::hud::toast("TEST RUN", "All steps done. Ctrl+Shift+F4 takes the car back.", 6);
        return;
    }
    test_run_begin_step(steps, n_steps);
}
