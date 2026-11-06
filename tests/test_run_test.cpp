// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

namespace {
struct call {
    uint64_t vehicle;
    uint32_t ai;
    int32_t reason;
};
call calls[16];
int n_calls = 0;
const char *played[8];
int n_played = 0;
uint64_t fake_play(void *, const char *name, uint64_t, uint64_t)
{
    if (n_played < 8)
        played[n_played++] = name;
    return 1;
}
uint64_t fake_set_controller(void *, uint64_t vehicle, uint32_t ai, int32_t reason)
{
    if (n_calls < 16)
        calls[n_calls++] = {vehicle, ai, reason};
    return 0;
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;
    orig_set_controller = fake_set_controller;

    // ---- Shift timing on the game thread: car+0x538 -> gearbox, +0x680 gear, +0x684 target ----
    alignas(16) static uint8_t car[0x600]{}, gearbox[0x700]{};
    uintptr_t gb = reinterpret_cast<uintptr_t>(gearbox), c = reinterpret_cast<uintptr_t>(car);
    memcpy(car + 0x538, &gb, 8);
    auto set = [&](uint32_t gear, uint32_t target) {
        memcpy(gearbox + 0x680, &gear, 4);
        memcpy(gearbox + 0x684, &target, 4);
    };
    test_run_on = false;
    set(3, 4);
    test_shift_tick(c);
    assert(test_shifts.count[0].load() == 0);  // off: nothing measured
    test_run_on = true;
    test_shifts.reset = true;
    set(3, 3);
    test_shift_tick(c);
    set(3, 4);  // upshift starts
    test_shift_tick(c);
    Sleep(20);
    set(4, 4);  // done
    test_shift_tick(c);
    set(4, 3);  // downshift
    test_shift_tick(c);
    set(3, 3);
    test_shift_tick(c);
    assert(test_shifts.count[0].load() == 1 && test_shifts.count[1].load() == 1);
    assert(test_shifts.max_us[0].load() >= 15000 && test_shifts.gear_seen[4].load() == 1 &&
           test_shifts.gear_seen[3].load() == 1);
    // A reset request clears the stats on the next game-thread tick.
    test_shifts.reset = true;
    test_shift_tick(c);
    assert(test_shifts.count[0].load() == 0 && test_shifts.gear_seen[4].load() == 0);
    test_run_on = false;

    // ---- The run: needs a session and your car on track ----
    // ---- Ctrl+Shift+F4: once -> you drive (after the double-press window), twice -> the AI, while running -> stop ----
    assert(test_key_action(true, 1000, false) == 0);
    assert(test_key_action(false, 1500, false) == 0);
    assert(test_key_action(false, 1000 + test_double_ms + 1, false) == 1);
    assert(test_key_action(true, 5000, false) == 0);
    assert(test_key_action(true, 5300, false) == 2);
    assert(test_key_action(false, 7000, false) == 0);
    assert(test_key_action(true, 8000, true) == 3);
    assert(test_key_action(false, 9000, true) == 0);

    reset_session();
    test_run_start(true);
    assert(!test_run_on.load() && n_calls == 0);
    static int sys_dummy;
    controller_system = &sys_dummy;
    player_vehicle = 5;
    player_pit = 1;
    session_ended = false;
    // You drive: no controller change, the steps begin at once and the run ends after the last one.
    test_run_start(false);
    assert(test_run_on.load() && n_calls == 0 && test_run.step == 0 && !test_run.ai);
    bool saw_mguk = false, saw_derate = false, saw_misfire = false, saw_grab = false, saw_weak = false;
    bool weak_first = false, decline_waited = false;
    while (test_run.active && test_run.step < n_drive_steps) {
        bool derate_ends = power_fault.kind == pl::battery_derate;
        radio_pending = 0;
        int before = test_run.step;
        test_run_tick(drive_steps[test_run.step].seconds + 0.1);
        fault_request = 0;
        // A test fault ends without the "fixed" radio (0.51.114: it played inside the next step).
        if (derate_ends)
            assert((radio_pending.load() & (1u << radio_engine_b)) && !(radio_pending.load() & (1u << radio_fault_fixed)) &&
                   power_fault.kind == pl::misfire);
        decline_waited |= test_run.active && test_run.step == before && drive_steps[before].fault == -1;
        saw_mguk |= power_fault.kind == pl::mguk_loss;
        saw_derate |= power_fault.kind == pl::battery_derate;
        saw_misfire |= power_fault.kind == pl::misfire;
        saw_grab |= rear_brake_loss_target.load() < 0;
        if (rear_brake_loss_target.load() > 0 && !saw_grab)
            weak_first = true;
        saw_weak |= rear_brake_loss_target.load() > 0;
    }
    assert(!test_run_on.load() && !test_run.active && n_calls == 0 && power_summary_due.exchange(false));
    assert(saw_mguk && saw_derate && saw_misfire && saw_grab && saw_weak && weak_first && power_fault.kind == pl::none);
    assert(decline_waited && gearbox_decline.path < 0);  // the step waited for the car; the stop ended the decline

    // The car is out in a step: the run pauses (its fault ends) and a flashback that brings the car back whole
    // continues it: the same step if less than half of it ran, else the next one.
    reset_session();
    controller_system = &sys_dummy;
    player_vehicle = 5;
    player_pit = 1;
    session_ended = false;
    test_run_start(false);
    test_run_tick(drive_steps[0].seconds + 0.1);
    assert(test_run.step == 1 && power_fault.kind == pl::mguk_loss);
    test_run_tick(5);
    player_broken = true;
    test_run_tick(0.1);
    assert(test_run.active && test_run.paused && power_fault.kind == pl::none);
    test_run_tick(10);  // still out: nothing
    assert(test_run.paused && test_run.step == 1);
    player_broken = false;
    test_run_tick(0.1);
    assert(!test_run.paused && test_run.step == 1 && power_fault.kind == pl::mguk_loss && test_run.step_clock < 1);
    test_run_tick(25);
    player_broken = true;
    test_run_tick(0.1);
    player_broken = false;
    test_run_tick(0.1);
    assert(test_run.step == 2 && power_fault.kind == pl::battery_derate);
    // The car breaks in the gearbox decline, the last step: the planned end.
    while (test_run.step < n_drive_steps - 1) {
        test_run_tick(drive_steps[test_run.step].seconds + 0.1);
        fault_request = 0;
    }
    assert(gearbox_decline.path >= 0 && gearbox_decline.test);
    // The decline fails: the step waits for the game's breakdown to stop the car, past its own time.
    test_run_tick(drive_steps[n_drive_steps - 1].seconds + 0.1);
    gearbox_decline = {};
    test_run_tick(drive_steps[n_drive_steps - 1].seconds);
    assert(test_run.active && test_run.decline_over && test_run.step == n_drive_steps - 1);
    player_broken = true;
    test_run_tick(0.1);
    assert(!test_run.active && !test_run_on.load());
    player_broken = false;
    // A retired car ends a paused run (0.51.115 log: the run stayed paused into the retirement).
    test_run_start(false);
    player_broken = true;
    test_run_tick(0.1);
    assert(test_run.active && test_run.paused);
    confirmed_retired_vehicle = 5;
    test_run_tick(0.1);
    assert(!test_run.active);
    player_broken = false;
    test_run_start(false);
    test_run_tick(0.1);
    assert(!test_run.active);  // retired while it ran: out, and done
    confirmed_retired_vehicle = -1;
    // Ctrl+Shift+F4 in a step: the step so far is logged before the run stops.
    test_run_start(false);
    test_run_tick(drive_steps[0].seconds + 0.1);
    brake_summary_step = nullptr;
    power_summary_due = false;
    test_run_stop_by_you();
    assert(!test_run.active && brake_summary_step.load() == drive_steps[1].name && brake_summary_power.load() &&
           power_summary_due.load());
    // A race restart stops the run.
    test_run_start(false);
    assert(test_run.active);
    test_run_restarted = true;
    test_run_tick(0.1);
    assert(!test_run.active && !test_run_restarted.load());
    reset_session();
    controller_system = &sys_dummy;
    player_vehicle = 5;
    player_pit = 1;
    session_ended = false;
    test_run_start(true);
    assert(test_run_on.load() && n_calls == 1 && calls[0].vehicle == 5 && calls[0].ai == 1 && calls[0].reason == 14);
    // The car stands after 15 s: BENCHMARK is tried next (ROBODRIVER taken back first).
    game_speed = 0;
    test_run_tick(16);
    assert(n_calls == 3 && calls[1].ai == 0 && calls[1].reason == 14 && calls[2].ai == 1 && calls[2].reason == 7);
    // It moves now: the baseline step begins.
    game_speed = 150;
    test_run_tick(16);
    assert(test_run.step == 0);
    // Each step requests its fault: slow shifts x8, 45 s counted in seconds.
    test_run_tick(test_steps[0].seconds + 0.1);
    assert(test_run.step == 1);
    uint64_t r = fault_request.exchange(0);
    assert((r & 0xff) == uint64_t(fm::f_slow_shift) && ((r >> 8) & 0xffffffff) == 450 && ((r >> 40) & 1) == 1 &&
           ((r >> 56) & 0xff) == 8);
    test_run_tick(test_steps[1].seconds + 0.1);
    r = fault_request.exchange(0);
    assert((r & 0xff) == uint64_t(fm::f_missing_gear) && ((r >> 48) & 0xff) == 4);
    // Up to the gearbox decline.
    while (test_run.step < n_test_steps - 1) {
        test_run_tick(test_steps[test_run.step].seconds + 0.1);
        fault_request = 0;
    }
    assert(gearbox_decline.path == fm::decline_hydraulics && gearbox_decline.length == 90.0);
    // The decline step waits for the car to stop, up to three times its time, with no hand-back.
    int calls_before = n_calls;
    test_run_tick(test_steps[n_test_steps - 1].seconds + 0.1);
    gearbox_decline = {};
    test_run_tick(test_steps[n_test_steps - 1].seconds);
    assert(test_run.step == n_test_steps - 1 && test_run.decline_over && n_calls == calls_before);
    // The car is out: the planned end; the run gives back only its own controller flag.
    player_broken = true;
    test_run_tick(1);
    assert(!test_run_on.load() && !test_run.active);
    assert(calls[n_calls - 1].ai == 0 && calls[n_calls - 1].reason == 7);
    player_broken = false;
    // No breakdown after three times the step: the step ends and the AI keeps driving (no hand-back at speed) until
    // Ctrl+Shift+F4.
    test_run_start(true);
    test_run_tick(16);
    assert(test_run.step == 0);
    while (test_run.step < n_test_steps - 1) {
        test_run_tick(test_steps[test_run.step].seconds + 0.1);
        fault_request = 0;
    }
    calls_before = n_calls;
    test_run_tick(3 * test_steps[n_test_steps - 1].seconds - 1);
    assert(test_run.step == n_test_steps - 1);
    test_run_tick(1.1);
    assert(test_run.active && test_run.step == n_test_steps && n_calls == calls_before);
    test_run_stop_by_you();
    assert(!test_run.active && n_calls == calls_before + 1 && calls[n_calls - 1].ai == 0);
    // The AI drives, the car is out in a step: the AI gives it back; after the flashback it takes it again.
    game_speed = 150;
    test_run_start(true);
    test_run_tick(16);
    assert(test_run.step == 0);
    int k = n_calls;
    player_broken = true;
    test_run_tick(0.1);
    assert(test_run.paused && n_calls == k + 1 && calls[n_calls - 1].ai == 0 && calls[n_calls - 1].reason == 14);
    player_broken = false;
    test_run_tick(0.1);
    assert(!test_run.paused && n_calls == k + 2 && calls[n_calls - 1].ai == 1 && calls[n_calls - 1].reason == 14);
    // Stopped while paused: no second hand-back.
    player_broken = true;
    test_run_tick(0.1);
    k = n_calls;
    test_run_stop("stopped by you");
    assert(n_calls == k && !test_run.active);
    player_broken = false;
    // A new session clears a running test, and its summary comes.
    test_run_start(true);
    assert(test_run_on.load());
    power_summary_due = false;
    reset_session();
    assert(!test_run_on.load() && !test_run.active && power_summary_due.load());

    // ---- Engineer radio: the player's failure object -> [obj+0x98]+8 speech -> vtable+0x98 ----
    {
        reset_session();
        alignas(16) static uint8_t mgr[0x200]{}, obj[0x100]{}, ctx[0x100]{}, speech[0x10]{}, vtable[0x100]{};
        alignas(16) static uint64_t entries[2]{};
        alignas(16) static uint8_t rcar[0x10]{};
        uintptr_t o = reinterpret_cast<uintptr_t>(obj), cx = reinterpret_cast<uintptr_t>(ctx),
                  sp = reinterpret_cast<uintptr_t>(speech), vt = reinterpret_cast<uintptr_t>(vtable),
                  fn = reinterpret_cast<uintptr_t>(&fake_play), rc = reinterpret_cast<uintptr_t>(rcar);
        entries[1] = o;
        uint64_t b = reinterpret_cast<uint64_t>(entries), e = b + sizeof entries;
        memcpy(mgr + 0x198, &b, 8);
        memcpy(mgr + 0x1a0, &e, 8);
        obj[0x78] = 3;
        memcpy(obj + 0x70, &rc, 8);
        memcpy(obj + 0x98, &cx, 8);
        memcpy(ctx + 8, &sp, 8);
        memcpy(speech, &vt, 8);
        memcpy(vtable + 0x98, &fn, 8);
        failure_mgr = reinterpret_cast<uintptr_t>(mgr);
        player_vehicle = 3;
        radio_say(radio_brakes);
        radio_say(radio_mech_dnf);
        radio_tick(rc);
        assert(n_played == 1 && !strcmp(played[0], "eng_randomfault_brakes"));
        radio_tick(rc);  // 6 s apart
        assert(n_played == 1 && radio_pending.load() == (1u << radio_mech_dnf));
        radio_tick(0);   // no car: nothing
        assert(n_played == 1);
        reset_session();
        assert(radio_pending.load() == 0);
        failure_mgr = 0;
    }

    // ---- Damage panel tiles: nothing wrong -> none (auto) or six fine ones (always); each problem marks its icon ----
    {
        namespace di = dicons;
        reset_session();
        di::badge b[di::n_icons];
        assert(damage_badges(b, false) == 0);
        assert(damage_badges(b, true) == di::n_icons && b[di::gearbox].level == di::fine && !b[di::gearbox].label[0]);
        terminal_mask = 4;
        shown_shift_scale = 6.0f;
        shown_missing_gear = 4;
        rear_brake_loss_target = 0.4f;
        shown_power_loss = 0.1f;
        int n = damage_badges(b, false);
        assert(n == 3);
        assert(b[0].icon == di::gearbox && b[0].level == di::broken && !strcmp(b[0].label, "OUT"));
        assert(b[1].icon == di::brakes && b[1].level == di::problem && !strcmp(b[1].label, "-40%"));
        assert(b[2].icon == di::power && b[2].level == di::problem && !strcmp(b[2].label, "-10%"));
        terminal_mask = 0;
        shown_decline_stage = 2;  // critical: red, no label
        damage_badges(b, false);
        assert(b[0].icon == di::gearbox && b[0].level == di::broken && !b[0].label[0]);
        shown_decline_stage = 0;  // early (yellow): the slow shifts (orange) are worse, the first label wins
        damage_badges(b, false);
        assert(b[0].level == di::problem && !strcmp(b[0].label, "x6"));
        shown_decline_stage = -1;
        shown_shift_scale = 1.0f;
        damage_badges(b, false);
        assert(!strcmp(b[0].label, "NO 3"));
        // A power fault marks its part too: the MGU-K the ERS tile, a misfire the engine.
        reset_session();
        shown_power_loss = 0.18f;
        shown_power_kind = pl::mguk_loss;
        n = damage_badges(b, false);
        assert(n == 2 && b[0].icon == di::ers && b[0].level == di::problem && b[1].icon == di::power &&
               !strcmp(b[1].label, "-18%"));
        shown_power_kind = pl::misfire;
        shown_power_cuts = true;
        n = damage_badges(b, false);
        assert(n == 2 && b[0].icon == di::engine && !strcmp(b[1].label, "CUTS"));
        // Corners: a bent rear left (1) and a collapsed front right (3): the suspension tile is red and names the
        // worst corner (0.51.120: the car with coloured wheels read as a puncture).
        reset_session();
        susp::corner_view = 1u | (3u << 6);
        n = damage_badges(b, false);
        assert(n == 1 && b[0].icon == di::suspension && b[0].level == di::broken && b[0].wheel[0] == di::minor &&
               b[0].wheel[3] == di::broken && b[0].wheel[1] == di::fine && !strcmp(b[0].label, "FR"));
        // Both fronts weakened (2), a rear bent: orange, "FRONT".
        susp::corner_view = 1u | (2u << 4) | (2u << 6);
        n = damage_badges(b, false);
        assert(n == 1 && b[0].level == di::problem && !strcmp(b[0].label, "FRONT"));
        // Limping on a broken front left: red, "LIMP".
        susp::corner_view = 3u << 4;
        limp_state = 1;
        crash_corners = 2u << 6;
        n = damage_badges(b, false);
        assert(n == 1 && b[0].level == di::broken && !strcmp(b[0].label, "LIMP"));
        limp_state = 0;
        crash_corners = 0;
        // The game broke the front right itself (wheel coming off, state 4) while the addon's model only bent the rear
        // right: red, "FR", without limping (0.51.127 test: the tile said RR).
        susp::corner_view = 1u << 2;
        crash_corners = 4u << 9;
        n = damage_badges(b, false);
        assert(n == 1 && b[0].level == di::broken && b[0].wheel[3] == di::broken && b[0].wheel[1] == di::minor &&
               !strcmp(b[0].label, "FR"));
        // The game broke the front right and only damaged the rear left wheel (state 2, a flat tyre): the rear left
        // is no broken corner (0.51.128 test 20:41:10: the tile said FR RL).
        susp::corner_view = 0;
        crash_corners = 2u | (4u << 9);
        n = damage_badges(b, false);
        assert(n == 1 && b[0].level == di::broken && b[0].wheel[3] == di::broken && b[0].wheel[0] == di::fine &&
               !strcmp(b[0].label, "FR"));
        // The addon's model collapsed that rear left: both red.
        susp::corner_view = 3u;
        n = damage_badges(b, false);
        assert(n == 1 && b[0].wheel[0] == di::broken && b[0].wheel[3] == di::broken && !strcmp(b[0].label, "FR RL"));
        crash_corners = 0;
        susp::corner_view = 0;
        // The label for every set of corners (bit 0 RL, 1 RR, 2 FL, 3 FR).
        assert(!strcmp(di::corner_label(1), "RL") && !strcmp(di::corner_label(2), "RR") &&
               !strcmp(di::corner_label(4), "FL") && !strcmp(di::corner_label(8), "FR"));
        assert(!strcmp(di::corner_label(12), "FRONT") && !strcmp(di::corner_label(3), "REAR") &&
               !strcmp(di::corner_label(5), "LEFT") && !strcmp(di::corner_label(10), "RIGHT"));
        assert(!strcmp(di::corner_label(6), "FL RR") && !strcmp(di::corner_label(9), "FR RL") &&
               !strcmp(di::corner_label(7), "x3") && !strcmp(di::corner_label(14), "x3") &&
               !strcmp(di::corner_label(15), "ALL") && !di::corner_label(0)[0]);
        susp::corner_view = 0;
        reset_session();
        assert(damage_badges(b, false) == 0);
    }

    // ---- Breakdown watch: the game started the armed sequence -> the watch lets go, nothing breaks twice ----
    {
        reset_session();
        alignas(16) static uint8_t bobj[0x100]{};
        bobj[0xa0] = 6;
        bobj[0xa1] = 0;
        breakdown_watch = {reinterpret_cast<uintptr_t>(bobj), 6, GetTickCount64()};
        breakdown_watch_tick();
        assert(breakdown_watch.obj != 0);  // still waiting
        bobj[0xa1] = 1;                    // "suspected" radio: the game has it
        breakdown_watch_tick();
        assert(breakdown_watch.obj == 0 && bobj[0xa0] == 6);
        reset_session();
    }

    std::puts("test run ok");
}
